#ifndef GXM_DECODE_BANDS_H
#define GXM_DECODE_BANDS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

/*
 * Splits a texture decode across worker threads.
 *
 * Decoding is pure per-pixel work: the source is read-only and each row of the
 * destination belongs to exactly one band, so bands can run concurrently
 * without synchronisation. `alignment` keeps a boundary away from the unit of
 * work a decoder uses (a 32x32 Morton tile, a 4x4 compressed block); a worker
 * must never start or end in the middle of one.
 *
 * Set VITA2NATIVE_DECODE_THREADS=1 to keep everything on the calling thread,
 * which is the A/B control for measuring the split.
 */
#define GXM_DECODE_MAX_WORKERS 4

/* Below this the hand-off costs more than the work it shares. */
#define GXM_DECODE_MIN_PIXELS (512u * 512u)

typedef void (*GxmBandFunction)(void *context, uint32_t first_row, uint32_t last_row);

typedef struct {
    GxmBandFunction function;
    void *context;
    uint32_t first_row;
    uint32_t last_row;
    pthread_t thread;
    bool started;
} GxmBandJob;

static void *gxm_band_thread(void *argument)
{
    GxmBandJob *job = argument;

    job->function(job->context, job->first_row, job->last_row);
    return NULL;
}

static inline unsigned gxm_decode_worker_count(uint32_t width, uint32_t height, uint32_t alignment)
{
    static int configured = -1;
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    unsigned workers;

    if (configured < 0) {
        long value = vita2native_env_int("VITA2NATIVE_DECODE_THREADS", 3);

        if (value < 1)
            value = 1;
        if (value > GXM_DECODE_MAX_WORKERS)
            value = GXM_DECODE_MAX_WORKERS;
        configured = (int)value;
    }

    if (pixels < GXM_DECODE_MIN_PIXELS || alignment == 0)
        return 1;

    workers = (unsigned)configured;
    if (workers > height / alignment)
        workers = height / alignment;
    if (workers < 1)
        workers = 1;
    return workers;
}

static inline void gxm_decode_in_bands(uint32_t width, uint32_t height, uint32_t alignment,
                                       GxmBandFunction function, void *context)
{
    GxmBandJob jobs[GXM_DECODE_MAX_WORKERS];
    unsigned workers = gxm_decode_worker_count(width, height, alignment);
    unsigned band = 0;
    uint32_t rows;
    uint32_t first = 0;

    if (workers <= 1 || !function) {
        function(context, 0, height);
        return;
    }

    /* Round each band up to the decoder's unit; the last one takes the rest. */
    rows = height / workers;
    rows = (rows + alignment - 1) / alignment * alignment;

    for (unsigned i = 0; i < workers && first < height; i++) {
        uint32_t last = first + rows;

        if (last > height || i + 1 == workers)
            last = height;

        jobs[band].function = function;
        jobs[band].context = context;
        jobs[band].first_row = first;
        jobs[band].last_row = last;
        jobs[band].started = false;
        band++;
        first = last;
    }

    /* The caller takes the last band, so it is never idle. */
    for (unsigned i = 0; i + 1 < band; i++) {
        if (pthread_create(&jobs[i].thread, NULL, gxm_band_thread, &jobs[i]) == 0)
            jobs[i].started = true;
        else
            jobs[i].function(jobs[i].context, jobs[i].first_row, jobs[i].last_row);
    }

    jobs[band - 1].function(jobs[band - 1].context, jobs[band - 1].first_row,
                            jobs[band - 1].last_row);

    for (unsigned i = 0; i + 1 < band; i++) {
        if (jobs[i].started)
            pthread_join(jobs[i].thread, NULL);
    }
}

#endif /* GXM_DECODE_BANDS_H */
