/*
 * SDL2 audio backend.
 *
 * Every Vita port gets its own queue; the SDL callback mixes all of them into
 * the device stream. Queues are what make sceAudioOutOutput block, which is
 * how guest audio threads pace themselves.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "log.h"

#include "SceAudio_internal.h"

#define AUDIO_MAX_QUEUES 16

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

static SDL_AudioDeviceID g_device;
static bool g_sdl_ready;

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

static void audio_callback(void *userdata, Uint8 *stream, int length)
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
    }
}

bool host_audio_open(int freq)
{
    SDL_AudioSpec want, have;

    if (g_device)
        return true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG("SceAudio: SDL audio init failed: %s", SDL_GetError());
        return false;
    }
    g_sdl_ready = true;

    memset(&want, 0, sizeof(want));
    want.freq = freq;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;

    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_device) {
        LOG("SceAudio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sdl_ready = false;
        return false;
    }

    SDL_PauseAudioDevice(g_device, 0);
    return true;
}

void host_audio_close(void)
{
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }

    if (g_sdl_ready) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
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
