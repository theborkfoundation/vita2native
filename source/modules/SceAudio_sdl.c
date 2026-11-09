/*
 * SDL2 audio backend.
 *
 * Every Vita port gets its own queue; the SDL callback mixes all of them into
 * the device stream. Queues are what make sceAudioOutOutput block, which is
 * how guest audio threads pace themselves.
 *
 * SDL is loaded with dlopen rather than linked, like host_video_sdl.c. That
 * keeps the ARM build free of a build-time SDL dependency while still using
 * the SDL the target already has at runtime.
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

#include "SceAudio_internal.h"

#define AUDIO_MAX_QUEUES 16

#define SDL_INIT_AUDIO 0x00000010u
#define SDL_AUDIO_S16LSB 0x8010u

typedef struct SDL_AudioSpec {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SDL_AudioSpec;

typedef struct SdlAudioApi {
    void *library;
    int (*init_subsystem)(uint32_t flags);
    void (*quit_subsystem)(uint32_t flags);
    const char *(*get_error)(void);
    uint32_t (*open_audio_device)(const char *device, int iscapture,
                                  const SDL_AudioSpec *desired, SDL_AudioSpec *obtained,
                                  int allowed_changes);
    void (*close_audio_device)(uint32_t dev);
    void (*pause_audio_device)(uint32_t dev, int pause_on);
    const char *(*get_current_audio_driver)(void);
} SdlAudioApi;

struct HostAudioPort {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int16_t *ring; /* interleaved stereo */
    size_t capacity;
    size_t head;
    size_t tail;
    size_t used;
};

static pthread_mutex_t g_queues_lock = PTHREAD_MUTEX_INITIALIZER;
static HostAudioPort *g_queues[AUDIO_MAX_QUEUES];
static unsigned int g_queue_count;

static uint32_t g_device;
static bool g_sdl_ready;
static SdlAudioApi *g_api;
static uint64_t g_frames_played;

static void queue_push(HostAudioPort *port, const int16_t *frames, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        port->ring[port->tail * 2 + 0] = frames[i * 2 + 0];
        port->ring[port->tail * 2 + 1] = frames[i * 2 + 1];
        port->tail = (port->tail + 1) % port->capacity;
        port->used++;
    }
}

static void queue_pop(HostAudioPort *port, int32_t *mix, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (port->used == 0)
            return;
        mix[i * 2 + 0] += port->ring[port->head * 2 + 0];
        mix[i * 2 + 1] += port->ring[port->head * 2 + 1];
        port->head = (port->head + 1) % port->capacity;
        port->used--;
    }
}

static void audio_callback(void *userdata, uint8_t *stream, int length)
{
    int32_t mix[4096];
    int16_t *out = (int16_t *)stream;
    size_t frames = (size_t)length / (2 * sizeof(int16_t));

    (void)userdata;
    memset(stream, 0, (size_t)length);

    while (frames > 0) {
        size_t chunk = frames > 2048 ? 2048 : frames;

        memset(mix, 0, chunk * 2 * sizeof(int32_t));

        pthread_mutex_lock(&g_queues_lock);
        for (unsigned int i = 0; i < g_queue_count; i++) {
            HostAudioPort *port = g_queues[i];

            pthread_mutex_lock(&port->mutex);
            queue_pop(port, mix, chunk);
            pthread_cond_broadcast(&port->cond);
            pthread_mutex_unlock(&port->mutex);
        }
        pthread_mutex_unlock(&g_queues_lock);

        for (size_t i = 0; i < chunk * 2; i++) {
            if (mix[i] > 32767)
                mix[i] = 32767;
            if (mix[i] < -32768)
                mix[i] = -32768;
            out[i] = (int16_t)mix[i];
        }

        out += chunk * 2;
        frames -= chunk;
        __atomic_add_fetch(&g_frames_played, chunk, __ATOMIC_RELAXED);
    }
}

static bool load_sdl_audio(void)
{
    static const char *const library_names[] = {
        "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so", NULL,
    };
    SdlAudioApi *api;

    if (g_api)
        return true;

    api = calloc(1, sizeof(*api));
    if (!api)
        return false;

    for (int i = 0; library_names[i] && !api->library; i++)
        api->library = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        LOG("SceAudio: SDL is not available; output will be muted");
        free(api);
        return false;
    }

#define LOAD_SDL(field, name)                                                                      \
    do {                                                                                           \
        *(void **)(&api->field) = dlsym(api->library, name);                                       \
        if (!api->field) {                                                                         \
            LOG("SceAudio: SDL is missing %s", name);                                              \
            dlclose(api->library);                                                                 \
            free(api);                                                                             \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

    LOAD_SDL(init_subsystem, "SDL_InitSubSystem");
    LOAD_SDL(quit_subsystem, "SDL_QuitSubSystem");
    LOAD_SDL(get_error, "SDL_GetError");
    LOAD_SDL(open_audio_device, "SDL_OpenAudioDevice");
    LOAD_SDL(close_audio_device, "SDL_CloseAudioDevice");
    LOAD_SDL(pause_audio_device, "SDL_PauseAudioDevice");
    *(void **)(&api->get_current_audio_driver) = dlsym(api->library, "SDL_GetCurrentAudioDriver");

#undef LOAD_SDL

    g_api = api;
    return true;
}

bool host_audio_open(int freq)
{
    SDL_AudioSpec want, have;

    if (g_device)
        return true;
    if (!load_sdl_audio())
        return false;

    if (g_api->init_subsystem(SDL_INIT_AUDIO) != 0) {
        LOG("SceAudio: SDL audio init failed: %s", g_api->get_error());
        return false;
    }
    g_sdl_ready = true;

    memset(&want, 0, sizeof(want));
    want.freq = freq;
    want.format = SDL_AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    g_device = g_api->open_audio_device(NULL, 0, &want, &have, 0);
    if (!g_device) {
        LOG("SceAudio: SDL_OpenAudioDevice failed: %s", g_api->get_error());
        g_api->quit_subsystem(SDL_INIT_AUDIO);
        g_sdl_ready = false;
        return false;
    }

    g_api->pause_audio_device(g_device, 0);
    LOG("SceAudio: SDL device %u opened (%d Hz, %u ch, fmt 0x%04x, %u frames, driver %s)",
        g_device, have.freq, (unsigned)have.channels, (unsigned)have.format, (unsigned)have.samples,
        g_api->get_current_audio_driver && g_api->get_current_audio_driver()
            ? g_api->get_current_audio_driver()
            : "?");
    return true;
}

uint64_t host_audio_frames_played(void)
{
    return __atomic_load_n(&g_frames_played, __ATOMIC_RELAXED);
}

void host_audio_close(void)
{
    if (g_device) {
        g_api->close_audio_device(g_device);
        g_device = 0;
    }

    if (g_sdl_ready) {
        g_api->quit_subsystem(SDL_INIT_AUDIO);
        g_sdl_ready = false;
    }
}

HostAudioPort *host_audio_port_create(int capacity_frames)
{
    HostAudioPort *port = calloc(1, sizeof(*port));

    if (!port)
        return NULL;

    port->capacity = capacity_frames > 0 ? (size_t)capacity_frames : 1024;
    port->ring = calloc(port->capacity * 2, sizeof(int16_t));
    if (!port->ring) {
        free(port);
        return NULL;
    }

    pthread_mutex_init(&port->mutex, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&port->cond, &attr);
    pthread_condattr_destroy(&attr);

    pthread_mutex_lock(&g_queues_lock);
    if (g_queue_count >= AUDIO_MAX_QUEUES) {
        pthread_mutex_unlock(&g_queues_lock);
        pthread_cond_destroy(&port->cond);
        pthread_mutex_destroy(&port->mutex);
        free(port->ring);
        free(port);
        return NULL;
    }
    g_queues[g_queue_count++] = port;
    pthread_mutex_unlock(&g_queues_lock);

    return port;
}

void host_audio_port_destroy(HostAudioPort *port)
{
    if (!port)
        return;

    pthread_mutex_lock(&g_queues_lock);
    for (unsigned int i = 0; i < g_queue_count; i++) {
        if (g_queues[i] == port) {
            memmove(&g_queues[i], &g_queues[i + 1],
                    (g_queue_count - i - 1) * sizeof(g_queues[0]));
            g_queue_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_queues_lock);

    pthread_mutex_lock(&port->mutex);
    pthread_cond_broadcast(&port->cond);
    pthread_mutex_unlock(&port->mutex);

    pthread_cond_destroy(&port->cond);
    pthread_mutex_destroy(&port->mutex);
    free(port->ring);
    free(port);
}

static void deadline_in_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

int host_audio_port_write(HostAudioPort *port, const int16_t *frames, size_t frame_count)
{
    size_t offset = 0;

    if (!port)
        return -1;

    pthread_mutex_lock(&port->mutex);
    while (offset < frame_count) {
        size_t space = port->capacity - port->used;
        size_t chunk = frame_count - offset;

        if (chunk > space)
            chunk = space;

        if (chunk > 0) {
            queue_push(port, frames + offset * 2, chunk);
            offset += chunk;
        }

        if (offset < frame_count) {
            struct timespec ts;

            deadline_in_ms(&ts, 100);
            pthread_cond_timedwait(&port->cond, &port->mutex, &ts);
        }
    }
    pthread_mutex_unlock(&port->mutex);

    return 0;
}

size_t host_audio_port_pending(HostAudioPort *port)
{
    size_t used;

    if (!port)
        return 0;

    pthread_mutex_lock(&port->mutex);
    used = port->used;
    pthread_mutex_unlock(&port->mutex);

    return used;
}

void host_audio_port_drain(HostAudioPort *port)
{
    struct timespec ts;

    if (!port)
        return;

    pthread_mutex_lock(&port->mutex);
    while (port->used > 0) {
        deadline_in_ms(&ts, 100);
        pthread_cond_timedwait(&port->cond, &port->mutex, &ts);
    }
    pthread_mutex_unlock(&port->mutex);
}
