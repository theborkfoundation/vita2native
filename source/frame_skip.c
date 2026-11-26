#include "frame_skip.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "settings.h"

#define TARGET_NS 16666667ull
#define TARGET_MS 16.666667
#define MAX_EVERY 4
#define COOLDOWN_FRAMES 120
#define WARMUP_FRAMES 120
#define SLACK 1.3

enum
{
    MODE_OFF,
    MODE_AUTO,
    MODE_FIXED
};

static int g_mode;
static bool g_ready;
static atomic_bool g_skip_next;
static atomic_bool g_hold;
static unsigned int g_every = 1;
static unsigned int g_index;
static double g_drawn_ms;
static double g_skipped_ms;
static unsigned int g_cooldown;
static unsigned int g_bad_skips;
static unsigned int g_frames_seen;
static unsigned int g_skipped_total;
static uint64_t g_last_ns;
static bool g_paused_logged;

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * With one frame drawn in every E, the loop costs drawn + (E-1) * skipped per E
 * frames, so E frames fit in 60 Hz when E = (drawn - skipped) / (TARGET - skipped).
 * A skipped frame is not free: it still runs the guest and waits for the next
 * vblank, and getting that cost wrong is what leaves the loop short of 60.
 */
static unsigned int every_for(double drawn_ms, double skipped_ms)
{
    double slack;
    unsigned int every;

    if (drawn_ms < TARGET_MS * SLACK)
        return 1;
    slack = TARGET_MS - skipped_ms;
    if (slack < 1.0)
        return MAX_EVERY;
    every = (unsigned int)ceil((drawn_ms - skipped_ms) / slack);
    if (every < 2)
        every = 2;
    if (every > MAX_EVERY)
        every = MAX_EVERY;
    return every;
}

static void init(void)
{
    const char *value;

    g_ready = true;
    value = vita2native_env_str("VITA2NATIVE_FRAME_SKIP");
    if (!value || !value[0])
        return;

    if (strcmp(value, "auto") == 0) {
        g_mode = MODE_AUTO;
        fprintf(stderr, "vita2native: frame skip: automatic, targets 60 frames per second\n");
        return;
    }

    {
        char *end = NULL;
        long count = strtol(value, &end, 10);

        if (!end || *end || count < 2 || count > MAX_EVERY)
            return;
        g_mode = MODE_FIXED;
        g_every = (unsigned int)count;
        fprintf(stderr, "vita2native: frame skip: one frame in %u is drawn\n", g_every);
    }
}

bool frame_skip_frame_current(void)
{
    if (!g_ready)
        init();
    if (g_mode == MODE_OFF)
        return true;
    if (atomic_load_explicit(&g_hold, memory_order_relaxed))
        return true;
    /* Loading creates the textures, render targets and shaders a title needs;
       skipping there only delays them. */
    if (g_frames_seen < WARMUP_FRAMES)
        return true;
    return !atomic_load_explicit(&g_skip_next, memory_order_relaxed);
}

void frame_skip_frame_end(bool skipped)
{
    uint64_t now;
    uint64_t elapsed;
    double elapsed_ms;

    if (!g_ready)
        init();
    if (g_mode == MODE_OFF)
        return;

    now = now_ns();
    elapsed = g_last_ns ? now - g_last_ns : TARGET_NS;
    elapsed_ms = (double)elapsed / 1e6;
    g_last_ns = now;
    if (g_frames_seen < 1000)
        g_frames_seen++;
    if (skipped)
        g_skipped_total++;

    if (g_mode == MODE_FIXED) {
        g_index = (g_index + 1) % g_every;
        atomic_store_explicit(&g_skip_next,
                              g_index != 0 &&
                                  !atomic_load_explicit(&g_hold, memory_order_relaxed),
                              memory_order_relaxed);
        return;
    }

    if (!skipped) {
        g_drawn_ms = g_drawn_ms > 0.0 ? g_drawn_ms * 0.7 + elapsed_ms * 0.3 : elapsed_ms;
        g_bad_skips = 0;
    } else {
        g_skipped_ms = g_skipped_ms > 0.0 ? g_skipped_ms * 0.7 + elapsed_ms * 0.3 : elapsed_ms;
        if (g_drawn_ms > 0.0 && elapsed_ms * 4.0 >= g_drawn_ms * 3.0)
            g_bad_skips++;
        else
            g_bad_skips = 0;
    }
    if (g_cooldown == 0 && g_bad_skips >= 3) {
        /*
         * A skipped frame that costs as much as a drawn one means the guest,
         * not the renderer, is the limit: skipping saves nothing and would only
         * freeze the image. Draw every frame for a while instead.
         */
        g_cooldown = COOLDOWN_FRAMES;
        if (!g_paused_logged) {
            fprintf(stderr,
                    "vita2native: frame skip: paused, a skipped frame took %.1f ms of a "
                    "%.1f ms drawn frame\n",
                    elapsed_ms, g_drawn_ms);
            g_paused_logged = true;
        }
    }

    if (g_frames_seen < WARMUP_FRAMES) {
        /* Let the first frames (asset loading, shader cache misses) settle
           before deciding anything from their cost. */
        g_every = 1;
    } else if (g_cooldown) {
        g_cooldown--;
        g_every = 1;
    } else if (!skipped) {
        g_every = every_for(g_drawn_ms, g_skipped_ms);
    }

    g_index = (g_index + 1) % g_every;
    atomic_store_explicit(&g_skip_next,
                          g_index != 0 && !atomic_load_explicit(&g_hold, memory_order_relaxed),
                          memory_order_relaxed);
}

void frame_skip_hold_next(bool hold)
{
    atomic_store_explicit(&g_hold, hold, memory_order_relaxed);
}

unsigned int frame_skip_take_skipped(void)
{
    unsigned int skipped = g_skipped_total;

    g_skipped_total = 0;
    return skipped;
}
