/*
 * SceAudio: the Vita audio ports.
 *
 * One host device is opened at a fixed rate; each Vita port keeps its own
 * queue that the mixer drains, so sceAudioOutOutput paces the guest exactly as
 * the hardware does. When no device can be opened (headless, no sound card)
 * output is still paced by sleeping, so guest timing does not change.
 */

#include <math.h>
#include <psp2/audioin.h>
#include <psp2/audioout.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceAudio.h"

#include "SceAudio_internal.h"
#include "settings.h"

#define AUDIO_DEVICE_FREQ 48000
#define AUDIO_MAX_PORTS   8

typedef struct {
    bool used;
    SceAudioOutPortType type;
    int len;
    int freq;
    SceAudioOutMode mode;
    int channels;
    int32_t volume[2];
    HostAudioPort *queue;
    size_t queue_frames; /* capacity in device frames */
    double phase;        /* resampler position carried between calls */
    int16_t previous[2];
    bool has_previous;
    int16_t *scratch;
    size_t scratch_frames;
    int16_t *input;
    size_t input_samples;
    uint64_t next_deadline_ns;
    uint64_t debug_last_ns;
    uint64_t debug_last_played;
    unsigned int debug_calls;
    int debug_peak;
} AudioPort;

static AudioPort g_ports[AUDIO_MAX_PORTS];
static bool g_audio_ready;
static uint64_t g_audio_start_ns;

static uint64_t now_ns(void)
{
    return host_monotonic_ns();
}

static int audio_debug(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = vita2native_env_flag("VITA2NATIVE_AUDIO_DEBUG");
    return enabled;
}

static void sleep_until(uint64_t deadline_ns)
{
    uint64_t now = now_ns();

    if (deadline_ns > now)
        host_sleep_ns(deadline_ns - now);
}

static AudioPort *get_port(int port)
{
    if (port < 0 || port >= AUDIO_MAX_PORTS || !g_ports[port].used)
        return NULL;

    return &g_ports[port];
}

/*
 * Audio buffers come from guest memory and titles sometimes hand us a buffer
 * shorter than len * channels. Use process_vm_readv so an unmapped tail becomes
 * silence instead of taking the emulator down; the guest heap can end on a
 * guard page.
 */
static void copy_guest_samples(int16_t *dest, const int16_t *source, size_t bytes)
{
    struct iovec local = { dest, bytes };
    struct iovec remote = { (void *)source, bytes };
    ssize_t copied = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);

    if (copied < 0)
        copied = 0;
    if ((size_t)copied < bytes)
        memset((uint8_t *)dest + copied, 0, bytes - (size_t)copied);
}

static int16_t *port_input(AudioPort *p, size_t samples)
{
    int16_t *grown;

    if (samples <= p->input_samples)
        return p->input;

    grown = realloc(p->input, samples * sizeof(int16_t));
    if (!grown)
        return NULL;

    p->input = grown;
    p->input_samples = samples;
    return grown;
}

/* One stereo input frame, clamped at the edges of the buffer. */
static void read_frame(const AudioPort *p, const int16_t *input, size_t input_frames,
                       int64_t index, int16_t out[2])
{
    if (index < 0) {
        out[0] = p->previous[0];
        out[1] = p->previous[1];
        return;
    }

    if ((size_t)index >= input_frames)
        index = (int64_t)input_frames - 1;

    if (p->channels == 1) {
        out[0] = input[index];
        out[1] = input[index];
    } else {
        out[0] = input[index * 2 + 0];
        out[1] = input[index * 2 + 1];
    }
}

/*
 * Converts `input_frames` guest frames into device-rate stereo frames in
 * `port->scratch`, applying the port volume. Returns the number of frames.
 */
static size_t resample_port(AudioPort *p, const int16_t *input, size_t input_frames)
{
    const double step = (double)p->freq / (double)AUDIO_DEVICE_FREQ;
    const int64_t last = (int64_t)input_frames - 1;
    size_t out = 0;
    double position = p->phase;

    while (position < (double)input_frames && out < p->scratch_frames) {
        int64_t index = (int64_t)position;
        double fraction = position - (double)index;
        int16_t a[2], b[2];

        read_frame(p, input, input_frames, index, a);
        read_frame(p, input, input_frames, index + 1, b);

        for (int c = 0; c < 2; c++) {
            double value = (double)a[c] + ((double)b[c] - (double)a[c]) * fraction;

            value = value * (double)p->volume[c] / (double)SCE_AUDIO_OUT_MAX_VOL;
            if (value > 32767.0)
                value = 32767.0;
            if (value < -32768.0)
                value = -32768.0;
            p->scratch[out * 2 + c] = (int16_t)value;
        }

        position += step;
        out++;
    }

    if (last >= 0) {
        int16_t frame[2];

        read_frame(p, input, input_frames, last, frame);
        p->previous[0] = frame[0];
        p->previous[1] = frame[1];
        p->has_previous = true;
    }

    p->phase = position - (double)input_frames;
    if (p->phase < 0.0)
        p->phase = 0.0;

    return out;
}

/* Pacing used when there is no device to drain the queue. */
static void pace_without_device(AudioPort *p, size_t frames)
{
    uint64_t duration = (uint64_t)((double)frames * 1e9 / (double)AUDIO_DEVICE_FREQ);

    if (p->next_deadline_ns == 0)
        p->next_deadline_ns = now_ns();

    p->next_deadline_ns += duration;
    sleep_until(p->next_deadline_ns);
}

EXPORT(SceAudio, 0x5BC341E4, int, sceAudioOutOpenPort, SceAudioOutPortType type, int len, int freq,
       SceAudioOutMode mode)
{
    AudioPort *p = NULL;
    int channels;
    size_t device_frames;
    int port;

    if (len < SCE_AUDIO_MIN_LEN || len > SCE_AUDIO_MAX_LEN)
        return SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
    if (freq < 8000 || freq > 48000)
        return SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
    if (mode != SCE_AUDIO_OUT_MODE_MONO && mode != SCE_AUDIO_OUT_MODE_STEREO)
        return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;

    for (port = 0; port < AUDIO_MAX_PORTS; port++) {
        if (!g_ports[port].used) {
            p = &g_ports[port];
            break;
        }
    }
    if (!p)
        return SCE_AUDIO_OUT_ERROR_PORT_FULL;

    channels = mode == SCE_AUDIO_OUT_MODE_STEREO ? 2 : 1;
    device_frames = (size_t)((double)len * AUDIO_DEVICE_FREQ / (double)freq) + 2;

    memset(p, 0, sizeof(*p));
    p->type = type;
    p->len = len;
    p->freq = freq;
    p->mode = mode;
    p->channels = channels;
    p->volume[0] = SCE_AUDIO_OUT_MAX_VOL;
    p->volume[1] = SCE_AUDIO_OUT_MAX_VOL;
    p->queue_frames = device_frames * 4;

    if (g_audio_ready) {
        p->scratch_frames = device_frames + 2;
        p->scratch = malloc(p->scratch_frames * 2 * sizeof(int16_t));
        if (p->scratch)
            p->queue = host_audio_port_create((int)p->queue_frames);
        if (!p->scratch || !p->queue) {
            host_audio_port_destroy(p->queue);
            free(p->scratch);
            memset(p, 0, sizeof(*p));
            return SCE_AUDIO_OUT_ERROR_OUT_OF_MEMORY;
        }
    }

    p->used = true;
    LOG("SceAudio: port %d opened (len %d, %d Hz, %s)", port, len, freq,
        channels == 2 ? "stereo" : "mono");

    return port;
}

EXPORT(SceAudio, 0x6184B939, int, sceAudioOutOpenExtPort, SceAudioOutPortType type, int len,
       int freq, SceAudioOutMode mode, int option)
{
    (void)option;

    return sceAudioOutOpenPort(type, len, freq, mode);
}

EXPORT(SceAudio, 0x69E2E6B5, int, sceAudioOutReleasePort, int port)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    host_audio_port_destroy(p->queue);
    free(p->scratch);
    free(p->input);
    memset(p, 0, sizeof(*p));

    return 0;
}

EXPORT(SceAudio, 0x02DB3F5F, int, sceAudioOutOutput, int port, const void *buf)
{
    AudioPort *p = get_port(port);
    const int16_t *samples = buf;
    size_t input_frames;
    size_t frames;

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    if (!buf) {
        if (!g_audio_ready) {
            p->next_deadline_ns = 0;
            return 0;
        }
        host_audio_port_drain(p->queue);
        p->next_deadline_ns = 0;
        return 0;
    }

    input_frames = (size_t)p->len;

    {
        size_t sample_count = input_frames * (size_t)p->channels;
        int16_t *staging = port_input(p, sample_count);

        if (!staging)
            return SCE_AUDIO_OUT_ERROR_OUT_OF_MEMORY;
        copy_guest_samples(staging, samples, sample_count * sizeof(int16_t));
        samples = staging;
    }

    if (audio_debug()) {
        uint64_t now = now_ns();
        int peak = 0;

        for (size_t i = 0; i < input_frames * (size_t)p->channels; i++) {
            int value = samples[i] < 0 ? -samples[i] : samples[i];

            if (value > peak)
                peak = value;
        }

        if (peak > p->debug_peak)
            p->debug_peak = peak;
        p->debug_calls++;

        if (now - p->debug_last_ns >= 2000000000ull) {
            uint64_t played = host_audio_frames_played();

            LOG("SceAudio: port %d calls %u peak %d vol %d/%d pending %u played %llu", port,
                p->debug_calls, p->debug_peak, p->volume[0], p->volume[1],
                (unsigned)host_audio_port_pending(p->queue),
                (unsigned long long)(played - p->debug_last_played));
            p->debug_last_ns = now;
            p->debug_last_played = played;
            p->debug_calls = 0;
            p->debug_peak = 0;
        }
    }

    if (!g_audio_ready) {
        size_t device_frames = (size_t)((double)input_frames * AUDIO_DEVICE_FREQ /
                                        (double)p->freq) + 1;

        pace_without_device(p, device_frames);
        return 0;
    }

    frames = resample_port(p, samples, input_frames);
    return host_audio_port_write(p->queue, p->scratch, frames) == 0
               ? 0
               : SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
}

EXPORT(SceAudio, 0x64167F11, int, sceAudioOutSetVolume, int port, SceAudioOutChannelFlag ch,
       int *vol)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (!vol)
        return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;

    if (ch & SCE_AUDIO_VOLUME_FLAG_L_CH) {
        if (vol[0] < 0 || vol[0] > SCE_AUDIO_OUT_MAX_VOL)
            return SCE_AUDIO_OUT_ERROR_INVALID_VOLUME;
        p->volume[0] = vol[0];
    }
    if (ch & SCE_AUDIO_VOLUME_FLAG_R_CH) {
        int index = (ch & SCE_AUDIO_VOLUME_FLAG_L_CH) ? 1 : 0;

        if (vol[index] < 0 || vol[index] > SCE_AUDIO_OUT_MAX_VOL)
            return SCE_AUDIO_OUT_ERROR_INVALID_VOLUME;
        p->volume[1] = vol[index];
    }

    return 0;
}

EXPORT(SceAudio, 0xFF635DA5, int, sceAudioOutGetPortVolume_forUser, int port, int *left,
       int *right)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (!left || !right)
        return SCE_AUDIO_OUT_ERROR_INVALID_POINTER;

    *left = p->volume[0];
    *right = p->volume[1];

    return 0;
}

EXPORT(SceAudio, 0x65840F3D, int, sceAudioOutSetPortVolume_forUser, int port, int left, int right)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (left < 0 || left > SCE_AUDIO_OUT_MAX_VOL || right < 0 || right > SCE_AUDIO_OUT_MAX_VOL)
        return SCE_AUDIO_OUT_ERROR_INVALID_VOLUME;

    p->volume[0] = left;
    p->volume[1] = right;

    return 0;
}

EXPORT(SceAudio, 0xB8BA0D07, int, sceAudioOutSetConfig, int port, SceSize len, int freq,
       SceAudioOutMode mode)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    if (len != (SceSize)-1) {
        if ((int)len < SCE_AUDIO_MIN_LEN || (int)len > SCE_AUDIO_MAX_LEN)
            return SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
        p->len = (int)len;
    }

    if (freq != -1) {
        if (freq < 8000 || freq > 48000)
            return SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
        if (freq != p->freq)
            LOG("SceAudio: port %d changed to %d Hz (device runs at %d Hz)", port, freq,
                AUDIO_DEVICE_FREQ);
        p->freq = freq;
        p->phase = 0.0;
        p->has_previous = false;
    }

    if (mode != (SceAudioOutMode)-1) {
        if (mode != SCE_AUDIO_OUT_MODE_MONO && mode != SCE_AUDIO_OUT_MODE_STEREO)
            return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;
        p->mode = mode;
        p->channels = mode == SCE_AUDIO_OUT_MODE_STEREO ? 2 : 1;
    }

    return 0;
}

EXPORT(SceAudio, 0x9C8EDAEA, int, sceAudioOutGetConfig, int port, SceAudioOutConfigType type)
{
    AudioPort *p = get_port(port);

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    switch (type) {
    case SCE_AUDIO_OUT_CONFIG_TYPE_LEN:
        return p->len;
    case SCE_AUDIO_OUT_CONFIG_TYPE_FREQ:
        return p->freq;
    case SCE_AUDIO_OUT_CONFIG_TYPE_MODE:
        return p->mode;
    default:
        return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;
    }
}

EXPORT(SceAudio, 0x9A5370C4, int, sceAudioOutGetRestSample, int port)
{
    AudioPort *p = get_port(port);
    size_t pending;

    if (!p)
        return SCE_AUDIO_OUT_ERROR_INVALID_PORT;

    if (!g_audio_ready)
        return 0;

    pending = host_audio_port_pending(p->queue);

    /* Report in guest samples, not device frames. */
    return (int)((double)pending * (double)p->freq / (double)AUDIO_DEVICE_FREQ);
}

EXPORT(SceAudio, 0x12FB1767, int, sceAudioOutGetAdopt, SceAudioOutPortType type)
{
    for (int i = 0; i < AUDIO_MAX_PORTS; i++) {
        if (g_ports[i].used && g_ports[i].type == type)
            return 1;
    }

    return 0;
}

EXPORT(SceAudio, 0x940CE469, int, sceAudioOutSetAlcMode, SceAudioOutAlcMode mode)
{
    (void)mode;

    return 0;
}

EXPORT(SceAudio, 0xDCC90EDB, int, sceAudioOutSetAdoptMode, int mode)
{
    (void)mode;

    return 0;
}

EXPORT(SceAudio, 0x26C82AF2, int, sceAudioOutSetAdopt_forUser, int mode)
{
    (void)mode;

    return 0;
}

EXPORT(SceAudio, 0x3601D375, int, sceAudioOutSetCompress, int type, int value)
{
    (void)type;
    (void)value;

    return 0;
}

EXPORT(SceAudio, 0xB41FACCE, int, sceAudioOutSetEffectType, int type)
{
    (void)type;

    return 0;
}

DECLARE_LIBRARY(SceAudio, 0x438bb957);

/* ------------------------------------------------------------- SceAudioIn */

#define AUDIO_IN_MAX_PORTS 4

typedef struct {
    bool used;
    int grain;
    int freq;
} AudioInPort;

static AudioInPort g_in_ports[AUDIO_IN_MAX_PORTS];

static AudioInPort *get_in_port(int port)
{
    if (port < 0 || port >= AUDIO_IN_MAX_PORTS || !g_in_ports[port].used)
        return NULL;

    return &g_in_ports[port];
}

EXPORT(SceAudioIn, 0x39B50DC1, int, sceAudioInOpenPort, SceAudioInPortType port_type, int grain,
       int freq, SceAudioInParam param)
{
    AudioInPort *p = NULL;
    int port;

    if (param != SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO)
        return SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM;

    if (port_type == SCE_AUDIO_IN_PORT_TYPE_VOICE) {
        if (freq != 16000)
            return SCE_AUDIO_IN_ERROR_INVALID_SAMPLE_FREQ;
        if (grain != 256 && grain != 512)
            return SCE_AUDIO_IN_ERROR_INVALID_PARAMETER;
    } else if (port_type == SCE_AUDIO_IN_PORT_TYPE_RAW) {
        if (freq != 16000 && freq != 48000)
            return SCE_AUDIO_IN_ERROR_INVALID_SAMPLE_FREQ;
        if ((freq == 16000 && grain != 256) || (freq == 48000 && grain != 768))
            return SCE_AUDIO_IN_ERROR_INVALID_PARAMETER;
    } else {
        return SCE_AUDIO_IN_ERROR_INVALID_PORT_TYPE;
    }

    for (port = 0; port < AUDIO_IN_MAX_PORTS; port++) {
        if (!g_in_ports[port].used) {
            p = &g_in_ports[port];
            break;
        }
    }
    if (!p)
        return SCE_AUDIO_IN_ERROR_PORT_FULL;

    memset(p, 0, sizeof(*p));
    p->used = true;
    p->grain = grain;
    p->freq = freq;

    return port;
}

EXPORT(SceAudioIn, 0x3A61B8C4, int, sceAudioInReleasePort, int port)
{
    AudioInPort *p = get_in_port(port);

    if (!p)
        return SCE_AUDIO_IN_ERROR_INVALID_PORT;

    memset(p, 0, sizeof(*p));
    return 0;
}

EXPORT(SceAudioIn, 0x638ADD2D, int, sceAudioInInput, int port, void *dest)
{
    AudioInPort *p = get_in_port(port);

    if (!p)
        return SCE_AUDIO_IN_ERROR_NOT_OPENED;
    if (!dest)
        return SCE_AUDIO_IN_ERROR_INVALID_POINTER;

    /* No microphone is wired up on this host; report silence. The hardware
       delivers mono S16 regardless of the requested parameter. */
    memset(dest, 0, (size_t)p->grain * sizeof(int16_t));

    return 0;
}

EXPORT(SceAudioIn, 0x343E8251, int, sceAudioInInputWithInputDeviceState, int port, void *dest)
{
    return sceAudioInInput(port, dest);
}

EXPORT(SceAudioIn, 0x566AC433, int, sceAudioInGetAdopt, SceAudioInPortType port_type)
{
    (void)port_type;

    return 0;
}

EXPORT(SceAudioIn, 0x2F940377, int, sceAudioInGetStatus, int select)
{
    (void)select;

    return 0;
}

EXPORT(SceAudioIn, 0x86118097, int, sceAudioInGetMicGain, int port)
{
    (void)port;

    return 0;
}

EXPORT(SceAudioIn, 0x0F34DD73, int, sceAudioInSetMicGain, int port, int gain)
{
    (void)port;
    (void)gain;

    return 0;
}

EXPORT(SceAudioIn, 0x1DFE7698, int, sceAudioInSetMute, int port, int mute)
{
    (void)port;
    (void)mute;

    return 0;
}

EXPORT(SceAudioIn, 0xA0EB852F, int, sceAudioInSelectInput, int port, int input)
{
    (void)port;
    (void)input;

    return 0;
}

EXPORT(SceAudioIn, 0x08105392, int, sceAudioInGetInput, int port)
{
    (void)port;

    return 0;
}

EXPORT(SceAudioIn, 0xC6962E84, int, sceAudioInOpenPortForDiag, SceAudioInPortType port_type,
       int grain, int freq, SceAudioInParam param)
{
    return sceAudioInOpenPort(port_type, grain, freq, param);
}

DECLARE_LIBRARY(SceAudioIn, 0xf8dc61a3);

int SceAudio_init(void)
{
    g_audio_start_ns = now_ns();
    g_audio_ready = host_audio_open(AUDIO_DEVICE_FREQ);

    if (!g_audio_ready)
        LOG("SceAudio: no audio device, output will be paced without sound");

    return 0;
}

int SceAudio_finish(void)
{
    for (int i = 0; i < AUDIO_MAX_PORTS; i++) {
        if (!g_ports[i].used)
            continue;
        host_audio_port_destroy(g_ports[i].queue);
        free(g_ports[i].scratch);
        free(g_ports[i].input);
        memset(&g_ports[i], 0, sizeof(g_ports[i]));
    }

    host_audio_close();
    g_audio_ready = false;

    (void)g_audio_start_ns;
    return 0;
}
