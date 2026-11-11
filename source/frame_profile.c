#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "frame_profile.h"
#include "settings.h"

static int g_enabled = -1;

static uint64_t g_last_present_ns;
static uint64_t g_last_log_ns;
static uint64_t g_interval_ns;
static uint64_t g_present_ns;
static uint64_t g_flip_ns;
static uint64_t g_vblank_ns;
static uint64_t g_draw_ns;
static uint64_t g_scene_ns;
static unsigned int g_frames;
static unsigned int g_vblank_calls;
static unsigned int g_scenes;

bool frame_profile_enabled(void)
{
    if (g_enabled < 0)
        g_enabled = vita2native_env_flag("VITA2NATIVE_PROFILE_FRAME");
    return g_enabled != 0;
}

uint64_t frame_profile_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Draws and scenes may be recorded from different guest threads than the one
   presenting, so use the 64-bit atomic helpers even though this is only a
   diagnostic. */
static void add_u64(uint64_t *value, uint64_t amount)
{
    __sync_fetch_and_add(value, amount);
}

void frame_profile_add_draw(uint64_t ns)
{
    if (frame_profile_enabled())
        add_u64(&g_draw_ns, ns);
}

void frame_profile_add_scene(uint64_t ns)
{
    if (!frame_profile_enabled())
        return;
    add_u64(&g_scene_ns, ns);
    __sync_fetch_and_add(&g_scenes, 1);
}

void frame_profile_add_vblank_wait(uint64_t ns)
{
    if (!frame_profile_enabled())
        return;
    add_u64(&g_vblank_ns, ns);
    __sync_fetch_and_add(&g_vblank_calls, 1);
}

void frame_profile_add_flip_wait(uint64_t ns)
{
    if (frame_profile_enabled())
        add_u64(&g_flip_ns, ns);
}

void frame_profile_present(uint64_t present_ns, uint64_t now_ns)
{
    uint64_t interval;
    double frames;

    if (!frame_profile_enabled())
        return;

    if (!g_last_present_ns) {
        g_last_present_ns = now_ns;
        g_last_log_ns = now_ns;
        return;
    }

    interval = now_ns - g_last_present_ns;
    g_last_present_ns = now_ns;
    add_u64(&g_interval_ns, interval);
    add_u64(&g_present_ns, present_ns);
    __sync_fetch_and_add(&g_frames, 1);

    if (now_ns - g_last_log_ns < 2000000000ull)
        return;

    frames = (double)g_frames;
    if (frames > 0.0) {
        double average = (double)g_interval_ns / frames;
        double present = (double)g_present_ns / frames;
        double flip = (double)g_flip_ns / frames;
        double vblank = (double)g_vblank_ns / frames;
        double draw = (double)g_draw_ns / frames;
        double scene = (double)g_scene_ns / frames;
        double guest = average - present - vblank - draw - scene;

        if (guest < 0.0)
            guest = 0.0;

        fprintf(stderr,
                "SceDisplay: frame %.1f ms (%.1f fps): present %.1f (flip %.1f), "
                "vblank %.1f ms (%.1f waits), gxm draw %.1f + end %.1f (%.1f scenes), "
                "guest %.1f ms\n",
                average / 1e6, average > 0.0 ? 1e9 / average : 0.0, present / 1e6, flip / 1e6,
                vblank / 1e6, frames > 0.0 ? (double)g_vblank_calls / frames : 0.0, draw / 1e6,
                scene / 1e6, frames > 0.0 ? (double)g_scenes / frames : 0.0, guest / 1e6);
    }

    g_interval_ns = 0;
    g_present_ns = 0;
    g_flip_ns = 0;
    g_vblank_ns = 0;
    g_draw_ns = 0;
    g_scene_ns = 0;
    g_frames = 0;
    g_vblank_calls = 0;
    g_scenes = 0;
    g_last_log_ns = now_ns;
}
