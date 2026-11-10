/*
 * Minimal NGS renderer.
 *
 * SceNgs exposes a synthesis graph (systems, racks, voices, modules, patches).
 * This file implements the subset the titles use: ATRAC9 voices, the HEVAG
 * PCM player (Simple voices) and the master buss mixer/output. Every other
 * buss type is treated as a pass-through.
 *
 * Mixing is done in float stereo at the system sample rate. Source voices
 * decode into a small frame queue; each system update takes one granularity of
 * frames from every active voice, mixes them through the patch volume
 * matrices into the master buss, and converts the result to S16 for
 * sceNgsVoiceGetStateData.
 *
 * NGS callbacks (module buffer switches and end-of-data) are queued during an
 * update and invoked by SceNgs.c after it releases its lock.
 */
#include "modules/SceNgs_render.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libatrac9.h"
#include "ngs_types.h"

#define NGS_R_VOICES            1024
#define NGS_R_PATCHES           1024
#define NGS_R_PARAM_SIZE        256
#define NGS_R_GRANULARITY_MAX   1024
#define NGS_R_QUEUE_INITIAL     4096
#define NGS_R_AT9_MAX_BUFFERS   4
#define NGS_R_PLAYER_MAX_BUFFERS 4
#define NGS_R_MODULE_AT9        0x5CAAu
#define NGS_R_MODULE_PLAYER     0x5CE6u

enum {
    NGS_R_CB_END_OF_DATA = 0,
    NGS_R_CB_SWAPPED_BUFFER = 1,
    NGS_R_CB_LOOPED_BUFFER = 2,
};

typedef struct {
    uint32_t buffer;
    int32_t bytes;
    int16_t loop_count;
    int16_t next_buffer;
    int16_t discard_start;
    int16_t discard_end;
} RAt9Buffer;

typedef struct {
    uint32_t id;
    uint32_t size;
    RAt9Buffer buffers[NGS_R_AT9_MAX_BUFFERS];
    float playback_frequency;
    float playback_scalar;
    int32_t lead_in;
    int32_t limit;
    int8_t channels;
    int8_t channel_map[2];
    int8_t reserved;
    int32_t config;
} RAt9Params;

typedef struct {
    uint32_t buffer;
    int32_t bytes;
    int16_t loop_count;
    int16_t next_buffer;
} RPlayerBuffer;

typedef struct {
    uint32_t id;
    uint32_t size;
    RPlayerBuffer buffers[NGS_R_PLAYER_MAX_BUFFERS];
    float playback_frequency;
    float playback_scalar;
    int32_t lead_in;
    int32_t limit;
    int8_t channels;
    int8_t channel_map[2];
    int8_t type;
    int8_t res;
    int8_t start_buffer;
    int8_t padding[2];
    int32_t reserved;
} RPlayerParams;

static_assert(sizeof(RAt9Params) == 96, "AT9 parameter layout");
static_assert(sizeof(RPlayerParams) == 84, "player parameter layout");

typedef struct {
    bool used;
    uint32_t rack;
    int8_t buss;
    int8_t channels;
    bool playing;
    bool paused;
    bool keyed_off;
    bool finished;

    uint8_t params[NGS_R_PARAM_SIZE];
    bool params_valid;

    float *queue;
    uint32_t queue_frames;
    uint32_t queue_capacity;

    /* ATRAC9 */
    void *at9;
    Atrac9CodecInfo at9_info;
    bool at9_ready;
    int32_t at9_config;
    int at9_buffer;
    uint32_t at9_position;
    int at9_loop;
    int at9_frame_index;
    uint32_t at9_superframe_left;

    /* HEVAG/PCM player */
    int32_t adpcm[2][4];
    int player_buffer;
    uint32_t player_position;
    int player_loop;
    double resample_position;

    /* master buss */
    float *mix;
    int16_t *state;

    void *module_callback;
    void *module_user;
    void *finished_callback;
    void *finished_user;
} RVoice;

typedef struct {
    bool used;
    uint32_t source;
    uint32_t dest;
    float matrix[2][2];
} RPatch;

typedef struct {
    void *callback;
    void *user_data;
    uint32_t voice;
    uint32_t rack;
    uint32_t module_id;
    uint32_t reason;
    uint32_t reason2;
    void *reason_ptr;
} RCallback;

static RVoice g_voices[NGS_R_VOICES];
static RPatch g_patches[NGS_R_PATCHES];
static RCallback g_callbacks[64];
static unsigned int g_callback_count;
static int g_granularity;
static int g_sample_rate;

static int ngs_render_trace(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_NGS_DEBUG") != NULL;
    return enabled;
}

/* HEVAG coefficients from the public vgmstream description of Sony's codec. */
static const int16_t hevag_coefs[128][4] = {
    { 0, 0, 0, 0 },
    { 7680, 0, 0, 0 },
    { 14720, -6656, 0, 0 },
    { 12544, -7040, 0, 0 },
    { 15616, -7680, 0, 0 },
    { 14731, -7059, 0, 0 },
    { 14507, -7366, 0, 0 },
    { 13920, -7522, 0, 0 },
    { 13133, -7680, 0, 0 },
    { 12028, -7680, 0, 0 },
    { 10764, -7680, 0, 0 },
    { 9359, -7680, 0, 0 },
    { 7832, -7680, 0, 0 },
    { 6201, -7680, 0, 0 },
    { 4488, -7680, 0, 0 },
    { 2717, -7680, 0, 0 },
    { 910, -7680, 0, 0 },
    { -910, -7680, 0, 0 },
    { -2717, -7680, 0, 0 },
    { -4488, -7680, 0, 0 },
    { -6201, -7680, 0, 0 },
    { -7832, -7680, 0, 0 },
    { -9359, -7680, 0, 0 },
    { -10764, -7680, 0, 0 },
    { -12028, -7680, 0, 0 },
    { -13133, -7680, 0, 0 },
    { -13920, -7522, 0, 0 },
    { -14507, -7366, 0, 0 },
    { -14731, -7059, 0, 0 },
    { 5376, -9216, 3328, -3072 },
    { -6400, -7168, -3328, -2304 },
    { -10496, -7424, -3584, -1024 },
    { -167, -2722, -494, -541 },
    { -7430, -2221, -2298, 424 },
    { -8001, -3166, -2814, 289 },
    { 6018, -4750, 2649, -1298 },
    { 3798, -6946, 3875, -1216 },
    { -8237, -2596, -2071, 227 },
    { 9199, 1982, -1382, -2316 },
    { 13021, -3044, -3792, 1267 },
    { 13112, -4487, -2250, 1665 },
    { -1668, -3744, -6456, 840 },
    { 7819, -4328, 2111, -506 },
    { 9571, -1336, -757, 487 },
    { 10032, -2562, 300, 199 },
    { -4745, -4122, -5486, -1493 },
    { -5896, 2378, -4787, -6947 },
    { -1193, -9117, -1237, -3114 },
    { 2783, -7108, -1575, -1447 },
    { -7334, -2062, -2212, 446 },
    { 6127, -2577, -315, -18 },
    { 9457, -1858, 102, 258 },
    { 7876, -4483, 2126, -538 },
    { -7172, -1795, -2069, 482 },
    { -7358, -2102, -2233, 440 },
    { -9170, -3509, -2674, -391 },
    { -2638, -2647, -1929, -1637 },
    { 1873, 9183, 1860, -5746 },
    { 9214, 1859, -1124, -2427 },
    { 13204, -3012, -4139, 1370 },
    { 12437, -4792, -256, 622 },
    { -2653, -1144, -3182, -6878 },
    { 9331, -1048, -828, 507 },
    { 1642, -620, -946, -4229 },
    { 4246, -7585, -533, -2259 },
    { -8988, -3891, -2807, 44 },
    { -2562, -2735, -1730, -1899 },
    { 3182, -483, -714, -1421 },
    { 7937, -3844, 2821, -1019 },
    { 10069, -2609, 314, 195 },
    { 8400, -3297, 1551, -155 },
    { -8529, -2775, -2432, -336 },
    { 9477, -1882, 108, 256 },
    { 75, -2241, -298, -6937 },
    { -9143, -4160, -2963, 5 },
    { -7270, -1958, -2156, 460 },
    { -2740, 3745, 5936, -1089 },
    { 8993, 1948, -683, -2704 },
    { 13101, -2835, -3854, 1055 },
    { 9543, -1961, 130, 250 },
    { 5272, -4270, 3124, -3157 },
    { -7696, -3383, -2907, -456 },
    { 7309, 2523, 434, -2461 },
    { 10275, -2867, 391, 172 },
    { 10940, -3721, 665, 97 },
    { 24, -310, -1262, 320 },
    { -8122, -2411, -2311, -271 },
    { -8511, -3067, -2337, 163 },
    { 326, -3846, 419, -933 },
    { 8895, 2194, -541, -2880 },
    { 12073, -1876, -2017, -601 },
    { 8729, -3423, 1674, -169 },
    { 12950, -3847, -3007, 1946 },
    { 10038, -2570, 302, 198 },
    { 9385, -2757, 1008, 41 },
    { -4720, -5006, -2852, -1161 },
    { 7869, -4326, 2135, -501 },
    { 2450, -8597, 1299, -2780 },
    { 10192, -2763, 360, 181 },
    { 11313, -4213, 833, 53 },
    { 10154, -2716, 345, 185 },
    { 9638, -1417, -737, 482 },
    { 3854, -4554, 2843, -3397 },
    { 6699, -5659, 2249, -1074 },
    { 11082, -3908, 728, 80 },
    { -1026, -9810, -805, -3462 },
    { 10396, -3746, 1367, -96 },
    { 10287, 988, -1915, -1437 },
    { 7953, 3878, -764, -3263 },
    { 12689, -3375, -3354, 2079 },
    { 6641, 3166, 231, -2089 },
    { -2348, -7354, -1944, -4122 },
    { 9290, -4039, 1885, -246 },
    { 4633, -6403, 1748, -1619 },
    { 11247, -4125, 802, 61 },
    { 9807, -2284, 219, 222 },
    { 9736, -1536, -706, 473 },
    { 8440, -3436, 1562, -176 },
    { 9307, -1021, -835, 509 },
    { 1698, -9025, 688, -3037 },
    { 10214, -2791, 368, 179 },
    { 8390, 3248, -758, -2989 },
    { 7201, 3316, 46, -2614 },
    { -88, -7809, -538, -4571 },
    { 6193, -5189, 2760, -1245 },
    { 12325, -1290, -3284, 253 },
    { 13064, -4075, -2824, 1877 },
    { 5333, 2999, 775, -1132 },
};

static const int nibble_lookup[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };

static RVoice *voice_at(uint32_t handle)
{
    uint32_t index = (handle >> 4) & 0x3ffu;

    if (index >= NGS_R_VOICES || !g_voices[index].used)
        return NULL;
    return &g_voices[index];
}

static uint8_t *guest_ptr(uint32_t address)
{
    return (uint8_t *)(uintptr_t)address;
}

int ngs_render_module_count(int buss)
{
    switch (buss) {
    case NGS_BUSS_MASTER: return 2;
    case NGS_BUSS_SIMPLE:
    case NGS_BUSS_SIMPLE_ATRAC9: return 6;
    case NGS_BUSS_ATRAC9:
    case NGS_BUSS_NORMAL_PLAYER: return 10;
    default: return 1;
    }
}

int ngs_render_output_count(int buss)
{
    switch (buss) {
    case NGS_BUSS_ATRAC9:
    case NGS_BUSS_NORMAL_PLAYER: return 4;
    case NGS_BUSS_SIMPLE:
    case NGS_BUSS_SIMPLE_ATRAC9:
    case NGS_BUSS_SCREAM:
    case NGS_BUSS_SCREAM_ATRAC9: return 2;
    default: return 1;
    }
}

static void callback_push(RVoice *voice, uint32_t module_id, uint32_t reason, uint32_t reason2,
                          const void *reason_ptr)
{
    RCallback *callback;

    if (g_callback_count >= sizeof(g_callbacks) / sizeof(g_callbacks[0]))
        return;

    callback = &g_callbacks[g_callback_count++];
    callback->callback = voice->module_callback;
    callback->user_data = voice->module_user;
    callback->voice = (uint32_t)((voice - g_voices) << 4) | 0x4e470003u;
    callback->rack = voice->rack;
    callback->module_id = module_id;
    callback->reason = reason;
    callback->reason2 = reason2;
    callback->reason_ptr = (void *)reason_ptr;
}

static void callback_finished(RVoice *voice)
{
    RCallback *callback;

    if (!voice->finished_callback ||
        g_callback_count >= sizeof(g_callbacks) / sizeof(g_callbacks[0]))
        return;

    callback = &g_callbacks[g_callback_count++];
    callback->callback = voice->finished_callback;
    callback->user_data = voice->finished_user;
    callback->voice = (uint32_t)((voice - g_voices) << 4) | 0x4e470003u;
    callback->rack = voice->rack;
    callback->module_id = 0;
    callback->reason = 0;
    callback->reason2 = 0;
    callback->reason_ptr = NULL;
}

void ngs_render_reset(void)
{
    for (uint32_t i = 0; i < NGS_R_VOICES; i++)
        ngs_render_voice_remove((i << 4) | 0x4e470003u);
    memset(g_patches, 0, sizeof(g_patches));
    g_callback_count = 0;
}

void ngs_render_configure(int granularity, int sample_rate)
{
    if (granularity > 0 && granularity <= NGS_R_GRANULARITY_MAX)
        g_granularity = granularity;
    else
        g_granularity = 512;
    g_sample_rate = sample_rate > 0 ? sample_rate : 48000;
}

void ngs_render_voice_add(uint32_t voice, uint32_t rack, int buss, int channels)
{
    uint32_t index = (voice >> 4) & 0x3ffu;
    RVoice *v;

    if (index >= NGS_R_VOICES)
        return;

    v = &g_voices[index];
    memset(v, 0, sizeof(*v));
    v->used = true;
    v->rack = rack;
    v->buss = (int8_t)buss;
    v->channels = (int8_t)(channels > 0 ? channels : 2);
    v->player_buffer = 0;
    v->at9_buffer = 0;

    if (buss == NGS_BUSS_MASTER && g_granularity > 0) {
        v->mix = calloc((size_t)g_granularity * 2, sizeof(float));
        v->state = calloc((size_t)g_granularity * 2, sizeof(int16_t));
    }
}

void ngs_render_voice_remove(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (!v)
        return;

    if (v->at9)
        Atrac9ReleaseHandle(v->at9);
    free(v->queue);
    free(v->mix);
    free(v->state);
    memset(v, 0, sizeof(*v));
}

void ngs_render_voice_params(uint32_t voice, uint32_t module, const void *data, uint32_t size)
{
    RVoice *v = voice_at(voice);

    if (!v || module != 0 || !data)
        return;

    if (size > sizeof(v->params))
        size = sizeof(v->params);
    memcpy(v->params, data, size);
    v->params_valid = true;
}

void ngs_render_voice_play(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (!v)
        return;

    v->playing = true;
    v->paused = false;
    v->keyed_off = false;
    v->finished = false;
    v->queue_frames = 0;
    v->at9_buffer = 0;
    v->at9_position = 0;
    v->at9_loop = 0;
    v->at9_frame_index = 0;
    v->at9_superframe_left = 0;
    v->player_buffer = 0;
    v->player_position = 0;
    v->player_loop = 0;
    v->resample_position = 0.0;
    memset(v->adpcm, 0, sizeof(v->adpcm));
}

void ngs_render_voice_key_off(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (v)
        v->keyed_off = true;
}

void ngs_render_voice_kill(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (!v)
        return;
    v->playing = false;
    v->finished = true;
    v->queue_frames = 0;
}

void ngs_render_voice_pause(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (v)
        v->paused = true;
}

void ngs_render_voice_resume(uint32_t voice)
{
    RVoice *v = voice_at(voice);

    if (v)
        v->paused = false;
}

void ngs_render_voice_callbacks(uint32_t voice, void *module_callback, void *module_user,
                                void *finished_callback, void *finished_user)
{
    RVoice *v = voice_at(voice);

    if (!v)
        return;
    v->module_callback = module_callback;
    v->module_user = module_user;
    v->finished_callback = finished_callback;
    v->finished_user = finished_user;
}

void ngs_render_patch_add(uint32_t patch, uint32_t source, uint32_t dest, int output_index,
                          int sub_index, int input_index)
{
    uint32_t index = (patch >> 4) & 0x3ffu;
    RPatch *p;

    (void)output_index;
    (void)sub_index;
    (void)input_index;

    if (index >= NGS_R_PATCHES)
        return;

    p = &g_patches[index];
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->source = source;
    p->dest = dest;
}

void ngs_render_patch_remove(uint32_t patch)
{
    uint32_t index = (patch >> 4) & 0x3ffu;

    if (index < NGS_R_PATCHES)
        memset(&g_patches[index], 0, sizeof(g_patches[index]));
}

void ngs_render_patch_volume(uint32_t patch, const float matrix[2][2])
{
    uint32_t index = (patch >> 4) & 0x3ffu;

    if (index < NGS_R_PATCHES && g_patches[index].used)
        memcpy(g_patches[index].matrix, matrix, sizeof(g_patches[index].matrix));
}

static void queue_reserve(RVoice *v, uint32_t frames)
{
    uint32_t needed = v->queue_frames + frames;
    uint32_t capacity;
    float *queue;

    if (needed <= v->queue_capacity)
        return;

    capacity = v->queue_capacity ? v->queue_capacity : NGS_R_QUEUE_INITIAL;
    while (capacity < needed)
        capacity *= 2;

    queue = realloc(v->queue, (size_t)capacity * 2 * sizeof(float));
    if (!queue)
        return;

    v->queue = queue;
    v->queue_capacity = capacity;
}

static void queue_push(RVoice *v, const float *data, uint32_t frames)
{
    queue_reserve(v, frames);
    if (v->queue_frames + frames > v->queue_capacity)
        return;

    memcpy(v->queue + (size_t)v->queue_frames * 2, data, (size_t)frames * 2 * sizeof(float));
    v->queue_frames += frames;
}

static uint32_t queue_take(RVoice *v, float *dest, uint32_t frames)
{
    uint32_t count = v->queue_frames < frames ? v->queue_frames : frames;

    memcpy(dest, v->queue, (size_t)count * 2 * sizeof(float));
    if (count < frames)
        memset(dest + (size_t)count * 2, 0, (size_t)(frames - count) * 2 * sizeof(float));

    if (count) {
        uint32_t left = v->queue_frames - count;

        memmove(v->queue, v->queue + (size_t)count * 2, (size_t)left * 2 * sizeof(float));
        v->queue_frames = left;
    }

    return count;
}

static void at9_next_buffer(RVoice *v, const RAt9Params *params)
{
    const RAt9Buffer *buffer = &params->buffers[v->at9_buffer];
    int next;

    v->at9_loop++;
    v->at9_position = 0;

    if (buffer->loop_count == -1 || v->at9_loop <= buffer->loop_count) {
        callback_push(v, NGS_R_MODULE_AT9, NGS_R_CB_LOOPED_BUFFER, (uint32_t)v->at9_loop,
                      guest_ptr(buffer->buffer));
        return;
    }

    v->at9_loop = 0;
    next = buffer->next_buffer;
    if (next < 0 || next >= NGS_R_AT9_MAX_BUFFERS || !params->buffers[next].buffer ||
        params->buffers[next].bytes <= 0 || !v->module_callback) {
        v->finished = true;
        v->playing = false;
        callback_push(v, NGS_R_MODULE_AT9, NGS_R_CB_END_OF_DATA, 0, NULL);
        callback_finished(v);
        return;
    }

    {
        uint32_t previous = (uint32_t)v->at9_buffer;

        v->at9_buffer = next;
        callback_push(v, NGS_R_MODULE_AT9, NGS_R_CB_SWAPPED_BUFFER, previous,
                      guest_ptr(params->buffers[next].buffer));
    }
}

static bool at9_step(RVoice *v)
{
    RAt9Params params;
    const RAt9Buffer *buffer;
    const uint8_t *input;
    int16_t pcm[2048];
    float frames[1024 * 2];
    int used = 0;
    uint32_t consumed;
    uint32_t frame_samples;
    uint32_t channels;

    if (!v->params_valid)
        return false;

    memcpy(&params, v->params, sizeof(params));
    if (v->at9_buffer < 0 || v->at9_buffer >= NGS_R_AT9_MAX_BUFFERS) {
        v->finished = true;
        v->playing = false;
        return false;
    }

    buffer = &params.buffers[v->at9_buffer];
    if (!buffer->buffer || buffer->bytes <= 0) {
        at9_next_buffer(v, &params);
        return !v->finished;
    }
    if (v->at9_position >= (uint32_t)buffer->bytes) {
        at9_next_buffer(v, &params);
        return !v->finished;
    }

    if (!v->at9_ready || v->at9_config != params.config) {
        if (v->at9)
            Atrac9ReleaseHandle(v->at9);
        v->at9 = Atrac9GetHandle();
        v->at9_ready = false;
        if (!v->at9 || Atrac9InitDecoder(v->at9, (unsigned char *)&params.config) != 0 ||
            Atrac9GetCodecInfo(v->at9, &v->at9_info) != 0) {
            v->finished = true;
            v->playing = false;
            return false;
        }
        v->at9_config = params.config;
        v->at9_ready = true;
        v->at9_frame_index = 0;
        v->at9_superframe_left = (uint32_t)v->at9_info.superframeSize;
    }

    input = guest_ptr(buffer->buffer) + v->at9_position;
    if (Atrac9Decode(v->at9, input, pcm, &used) != 0 || used <= 0) {
        v->finished = true;
        v->playing = false;
        callback_push(v, NGS_R_MODULE_AT9, NGS_R_CB_END_OF_DATA, 0, NULL);
        callback_finished(v);
        return false;
    }

    consumed = (uint32_t)used;
    if (v->at9_superframe_left >= consumed)
        v->at9_superframe_left -= consumed;
    else
        v->at9_superframe_left = 0;

    v->at9_frame_index++;
    if (v->at9_frame_index >= v->at9_info.framesInSuperframe) {
        consumed += v->at9_superframe_left;
        v->at9_frame_index = 0;
        v->at9_superframe_left = (uint32_t)v->at9_info.superframeSize;
    }

    v->at9_position += consumed;

    frame_samples = (uint32_t)v->at9_info.frameSamples;
    channels = (uint32_t)v->at9_info.channels;
    if (channels < 1)
        channels = 1;
    if (channels > 2)
        channels = 2;
    if (frame_samples > 1024)
        frame_samples = 1024;

    for (uint32_t i = 0; i < frame_samples; i++) {
        float left = (float)pcm[i * channels] / 32768.0f;
        float right = channels > 1 ? (float)pcm[i * channels + 1] / 32768.0f : left;

        frames[i * 2] = left;
        frames[i * 2 + 1] = right;
    }

    queue_push(v, frames, frame_samples);
    return true;
}

static void player_next_buffer(RVoice *v, const RPlayerParams *params)
{
    const RPlayerBuffer *buffer = &params->buffers[v->player_buffer];
    int next;

    v->player_loop++;
    v->player_position = 0;

    if (buffer->loop_count == -1 || v->player_loop <= buffer->loop_count) {
        callback_push(v, NGS_R_MODULE_PLAYER, NGS_R_CB_LOOPED_BUFFER, (uint32_t)v->player_loop,
                      guest_ptr(buffer->buffer));
        return;
    }

    v->player_loop = 0;
    next = buffer->next_buffer;
    if (next < 0 || next >= NGS_R_PLAYER_MAX_BUFFERS || !params->buffers[next].buffer ||
        params->buffers[next].bytes <= 0 || !v->module_callback) {
        v->finished = true;
        v->playing = false;
        callback_push(v, NGS_R_MODULE_PLAYER, NGS_R_CB_END_OF_DATA, 0, NULL);
        callback_finished(v);
        return;
    }

    {
        uint32_t previous = (uint32_t)v->player_buffer;

        v->player_buffer = next;
        callback_push(v, NGS_R_MODULE_PLAYER, NGS_R_CB_SWAPPED_BUFFER, previous,
                      guest_ptr(params->buffers[next].buffer));
    }
}

static void hevag_decode_frame(const uint8_t *frame, int32_t hist[4], float *out)
{
    uint8_t coef_index = (uint8_t)((frame[0] >> 4) & 0xf);
    uint8_t shift_factor = (uint8_t)(frame[0] & 0xf);
    uint8_t flag;
    const int samples_per_frame = 28;

    coef_index = (uint8_t)(((frame[1] & 0xf0) | coef_index));
    flag = frame[1] & 0x0f;

    if (coef_index > 127)
        coef_index = 0;
    shift_factor = (uint8_t)(20 - shift_factor);

    for (int j = 0; j < samples_per_frame; j++) {
        int32_t sample = 0;

        if (flag < 0x07) {
            uint8_t nibbles = frame[0x02 + j / 2];

            sample = (j & 1 ? nibble_lookup[nibbles >> 4] : nibble_lookup[nibbles & 0xf])
                     << shift_factor;
            sample += ((hist[0] * hevag_coefs[coef_index][0] +
                        hist[1] * hevag_coefs[coef_index][1] +
                        hist[2] * hevag_coefs[coef_index][2] +
                        hist[3] * hevag_coefs[coef_index][3]) >>
                       5);
            sample >>= 8;
        }

        if (sample > 32767)
            sample = 32767;
        if (sample < -32768)
            sample = -32768;

        out[j] = (float)sample / 32768.0f;

        hist[3] = hist[2];
        hist[2] = hist[1];
        hist[1] = hist[0];
        hist[0] = sample;
    }
}

static bool player_step(RVoice *v)
{
    RPlayerParams params;
    const RPlayerBuffer *buffer;
    const uint8_t *input;
    float frames[1024 * 2];
    uint32_t samples = 0;
    int channels;

    if (!v->params_valid)
        return false;

    memcpy(&params, v->params, sizeof(params));
    if (v->player_buffer < 0 || v->player_buffer >= NGS_R_PLAYER_MAX_BUFFERS) {
        v->finished = true;
        v->playing = false;
        return false;
    }

    buffer = &params.buffers[v->player_buffer];
    if (!buffer->buffer || buffer->bytes <= 0) {
        player_next_buffer(v, &params);
        return !v->finished;
    }
    if (v->player_position >= (uint32_t)buffer->bytes) {
        player_next_buffer(v, &params);
        return !v->finished;
    }

    channels = params.channels > 1 ? 2 : 1;
    input = guest_ptr(buffer->buffer) + v->player_position;

    if (ngs_render_trace()) {
        static unsigned int logged;

        if (logged < 8) {
            fprintf(stderr,
                    "SceNgs: player voice %u freq %.0f ch %d type %d bytes %d pos %u\n",
                    (uint32_t)(v - g_voices), (double)params.playback_frequency, channels,
                    params.type, buffer->bytes, v->player_position);
            logged++;
        }
    }

    if (params.type == 0) {
        uint32_t available = ((uint32_t)buffer->bytes - v->player_position) /
                             ((uint32_t)channels * sizeof(int16_t));
        int16_t *pcm = (int16_t *)(uintptr_t)input;

        if (available > 1024)
            available = 1024;
        for (uint32_t i = 0; i < available; i++) {
            float left = (float)pcm[i * channels] / 32768.0f;
            float right = channels > 1 ? (float)pcm[i * channels + 1] / 32768.0f : left;

            frames[i * 2] = left;
            frames[i * 2 + 1] = right;
        }
        samples = available;
        v->player_position += available * (uint32_t)channels * sizeof(int16_t);
    } else {
        uint32_t available = ((uint32_t)buffer->bytes - v->player_position) / 16u;

        if (available == 0) {
            player_next_buffer(v, &params);
            return !v->finished;
        }
        if (available > 64)
            available = 64;

        if (channels == 1) {
            for (uint32_t index = 0; index < available && samples + 28 <= 1024; index++) {
                float mono[28];

                hevag_decode_frame(input + index * 16, v->adpcm[0], mono);
                for (uint32_t i = 0; i < 28; i++) {
                    frames[(samples + i) * 2] = mono[i];
                    frames[(samples + i) * 2 + 1] = mono[i];
                }
                samples += 28;
            }
            v->player_position += available * 16u;
        } else {
            uint32_t index = 0;

            while (index + 1 < available && samples + 28 <= 1024) {
                float left[28];
                float right[28];

                hevag_decode_frame(input + index * 16, v->adpcm[0], left);
                hevag_decode_frame(input + (index + 1) * 16, v->adpcm[1], right);
                for (uint32_t i = 0; i < 28; i++) {
                    frames[(samples + i) * 2] = left[i];
                    frames[(samples + i) * 2 + 1] = right[i];
                }
                samples += 28;
                index += 2;
            }
            v->player_position += index * 16u;
        }
    }

    if (samples == 0) {
        player_next_buffer(v, &params);
        return !v->finished;
    }

    queue_push(v, frames, samples);
    return true;
}

static bool voice_render(RVoice *v, float *out, uint32_t frames)
{
    uint32_t guard = 0;
    double ratio = 1.0;
    uint32_t needed = frames;

    if (v->buss == NGS_BUSS_MASTER)
        return false;

    if (v->buss == NGS_BUSS_SIMPLE || v->buss == NGS_BUSS_NORMAL_PLAYER) {
        RPlayerParams params;

        if (v->params_valid && v->params[0] == 0xe6 && v->params[1] == 0x5c) {
            memcpy(&params, v->params, sizeof(params));
            if (params.playback_frequency > 0.0f)
                ratio = (double)params.playback_frequency / (double)g_sample_rate;
        }
    }

    if (ratio != 1.0) {
        uint32_t drop = (uint32_t)v->resample_position;

        if (drop && drop <= v->queue_frames) {
            memmove(v->queue, v->queue + (size_t)drop * 2,
                    (size_t)(v->queue_frames - drop) * 2 * sizeof(float));
            v->queue_frames -= drop;
            v->resample_position -= drop;
        }
        needed = (uint32_t)(v->resample_position + (double)frames * ratio) + 2;
    }

    while (v->queue_frames < needed && !v->finished && guard++ < 512) {
        bool progressed;

        if (v->buss == NGS_BUSS_SIMPLE_ATRAC9 || v->buss == NGS_BUSS_ATRAC9)
            progressed = at9_step(v);
        else if (v->buss == NGS_BUSS_SIMPLE || v->buss == NGS_BUSS_NORMAL_PLAYER)
            progressed = player_step(v);
        else
            break;

        if (!progressed)
            break;
    }

    if (ratio == 1.0) {
        queue_take(v, out, frames);
        return true;
    }

    for (uint32_t i = 0; i < frames; i++) {
        double position = v->resample_position + (double)i * ratio;
        uint32_t index = (uint32_t)position;
        double fraction = position - (double)index;
        float left = 0.0f;
        float right = 0.0f;

        if (index + 1 < v->queue_frames) {
            const float *a = &v->queue[(size_t)index * 2];
            const float *b = &v->queue[(size_t)(index + 1) * 2];

            left = (float)((double)a[0] * (1.0 - fraction) + (double)b[0] * fraction);
            right = (float)((double)a[1] * (1.0 - fraction) + (double)b[1] * fraction);
        } else if (index < v->queue_frames) {
            left = v->queue[(size_t)index * 2];
            right = v->queue[(size_t)index * 2 + 1];
        }

        out[i * 2] = left;
        out[i * 2 + 1] = right;
    }

    v->resample_position += (double)frames * ratio;
    return true;
}

void ngs_render_update(void)
{
    float out[NGS_R_GRANULARITY_MAX * 2];

    if (g_granularity <= 0)
        return;

    for (uint32_t i = 0; i < NGS_R_VOICES; i++) {
        RVoice *v = &g_voices[i];

        if (v->used && v->buss == NGS_BUSS_MASTER && v->mix)
            memset(v->mix, 0, (size_t)g_granularity * 2 * sizeof(float));
    }

    for (uint32_t i = 0; i < NGS_R_VOICES; i++) {
        RVoice *source = &g_voices[i];

        if (!source->used || !source->playing || source->paused)
            continue;

        memset(out, 0, (size_t)g_granularity * 2 * sizeof(float));
        if (!voice_render(source, out, (uint32_t)g_granularity))
            continue;

        for (uint32_t p = 0; p < NGS_R_PATCHES; p++) {
            RPatch *patch = &g_patches[p];
            RVoice *dest;

            if (!patch->used || patch->source != ((i << 4) | 0x4e470003u))
                continue;

            dest = voice_at(patch->dest);
            if (!dest || !dest->mix)
                continue;

            for (int f = 0; f < g_granularity; f++) {
                float left = out[f * 2];
                float right = out[f * 2 + 1];

                dest->mix[f * 2] += left * patch->matrix[0][0] + right * patch->matrix[1][0];
                dest->mix[f * 2 + 1] += left * patch->matrix[0][1] + right * patch->matrix[1][1];
            }
        }
    }

    for (uint32_t i = 0; i < NGS_R_VOICES; i++) {
        RVoice *v = &g_voices[i];

        if (!v->used || v->buss != NGS_BUSS_MASTER || !v->state || !v->mix)
            continue;

        for (int f = 0; f < g_granularity * 2; f++) {
            float sample = v->mix[f] * 32768.0f;

            if (sample > 32767.0f)
                sample = 32767.0f;
            if (sample < -32768.0f)
                sample = -32768.0f;
            v->state[f] = (int16_t)sample;
        }
    }

    if (ngs_render_trace()) {
        static unsigned int updates;

        if (updates < 4 || (updates % 94) == 0) {
            for (uint32_t i = 0; i < NGS_R_VOICES; i++) {
                RVoice *v = &g_voices[i];
                int peak = 0;

                if (!v->used || v->buss != NGS_BUSS_MASTER || !v->state)
                    continue;
                for (int f = 0; f < g_granularity * 2; f++) {
                    int value = v->state[f];

                    if (value < 0)
                        value = -value;
                    if (value > peak)
                        peak = value;
                }
                fprintf(stderr,
                        "SceNgs: render %u master %u peak %d samples %d %d %d %d\n",
                        updates, i, peak, v->state[0], v->state[1], v->state[2], v->state[3]);
            }
        }
        updates++;
    }
}

void ngs_render_dispatch_callbacks(void)
{
    unsigned int count = g_callback_count;

    g_callback_count = 0;
    for (unsigned int i = 0; i < count; i++) {
        const RCallback *job = &g_callbacks[i];
        SceNgsCallbackInfo info;

        if (!job->callback)
            continue;

        memset(&info, 0, sizeof(info));
        info.voice_handle = job->voice;
        info.rack_handle = job->rack;
        info.module_id = job->module_id;
        info.callback_reason = job->reason;
        info.callback_reason2 = job->reason2;
        info.callback_ptr = job->reason_ptr;
        info.user_data = job->user_data;

        if (ngs_render_trace()) {
            static unsigned int logged;

            if (logged < 12) {
                fprintf(stderr, "SceNgs: callback module %u reason %u reason2 %u ptr %p\n",
                        job->module_id, job->reason, job->reason2, job->reason_ptr);
                logged++;
            }
        }

        ((void (*)(const SceNgsCallbackInfo *))job->callback)(&info);
    }
}

bool ngs_render_get_state(uint32_t voice, uint32_t module, void *dest, uint32_t size)
{
    RVoice *v = voice_at(voice);
    uint32_t state_size;

    if (!dest || !size)
        return false;

    if (!v || v->buss != NGS_BUSS_MASTER || module != 1 || !v->state) {
        memset(dest, 0, size);
        return true;
    }

    state_size = (uint32_t)g_granularity * 2 * sizeof(int16_t);
    memset(dest, 0, size);
    memcpy(dest, v->state, size < state_size ? size : state_size);
    return true;
}
