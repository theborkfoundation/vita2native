#include <errno.h>
#include <psp2/display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "frame_profile.h"
#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "modules/SceGxm.h"
#include "modules/common_dialog_overlay.h"
#include "modules/fps_overlay.h"
#include "settings.h"
#include "util.h"

static SceDisplayFrameBuf g_vita_conf_fb;
static unsigned int g_frame_count;

/*
 * Vblank clock. Sleeping a fresh 16.67 ms per wait restarts from the wakeup
 * time, so scheduler overshoot accumulates and the game is always charged a
 * full period even when it is already late relative to the display. Keep an
 * absolute 60 Hz grid instead: a wait sleeps until the next boundary on that
 * grid, which both avoids drift and lets a late game wait less (or not at
 * all) instead of paying a full extra period every frame.
 */
#define VBLANK_PERIOD_NS 16666667ull

static uint64_t g_vblank_epoch_ns;
static HostVideo *g_video;
static bool g_video_checked;

static HostVideo *display_video(void)
{
    if (!g_video_checked) {
        g_video_checked = true;
        g_video = host_video_open();
        if (g_video)
            LOG("SceDisplay: video output %dx%d", host_video_width(g_video),
                host_video_height(g_video));
        else
            LOG("SceDisplay: no video output, frames stay headless");
    }

    return g_video;
}

#define MAX_CAPTURES 64
static long g_captures[MAX_CAPTURES];
static int g_capture_count;

/*
 * Headless frame capture. Set VITA2NATIVE_DUMP_FRAME to a path and optionally
 * VITA2NATIVE_DUMP_FRAME_AT to a comma-separated list of swap indices to
 * capture a BMP of the guest framebuffer. A '%u' in the path is replaced with
 * the frame index, so one run can capture a scripted sequence. This is how real
 * homebrew output is checked without a window.
 */
static void put_u16(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void dump_framebuffer(const SceDisplayFrameBuf *fb)
{
    static const char *path;
    static bool initialised;
    const uint8_t *base;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int row_size;
    unsigned char header[54];
    unsigned char *row;
    char file_path[1024];
    FILE *file;

    if (!initialised) {
        const char *at = vita2native_env_str("VITA2NATIVE_DUMP_FRAME_AT");

        initialised = true;
        path = vita2native_env_str("VITA2NATIVE_DUMP_FRAME");
        if (at) {
            char *copy = malloc(strlen(at) + 1);

            if (copy) {
                char *token;

                strcpy(copy, at);
                for (token = strtok(copy, ",");
                     token && g_capture_count < MAX_CAPTURES; token = strtok(NULL, ","))
                    g_captures[g_capture_count++] = strtol(token, NULL, 0);
                free(copy);
            }
        }
    }

    if (!path || !fb || !fb->base)
        return;
    if (g_capture_count > 0) {
        bool wanted = false;

        for (int i = 0; i < g_capture_count; i++)
            wanted = wanted || g_captures[i] == (long)g_frame_count;
        if (!wanted)
            return;
    }

    /* A '%u' in the path is replaced with the frame index, which lets one run
       capture a whole scripted sequence. */
    if (strchr(path, '%'))
        snprintf(file_path, sizeof(file_path), path, g_frame_count);
    else
        snprintf(file_path, sizeof(file_path), "%s", path);

    width  = fb->width;
    height = fb->height;
    pitch  = fb->pitch;
    if (width == 0 || height == 0 || pitch < width)
        return;

    file = fopen(file_path, "wb");
    if (!file)
        return;

    base = fb->base;
    row_size = (width * 3 + 3) & ~3u;
    row = calloc(1, row_size);
    if (!row) {
        fclose(file);
        return;
    }

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, 54u + (uint32_t)(row_size * height));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, width);
    put_u32(header + 22, height);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, (uint32_t)(row_size * height));
    fwrite(header, 1, sizeof(header), file);

    for (unsigned int y = height; y-- > 0;) {
        const uint32_t *src = (const uint32_t *)(base + (size_t)y * pitch * 4);

        for (unsigned int x = 0; x < width; x++) {
            uint32_t pixel = src[x];

            /* A8B8G8R8, the only format the guest can present. */
            row[x * 3 + 0] = (unsigned char)(pixel & 0xFF);
            row[x * 3 + 1] = (unsigned char)((pixel >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)((pixel >> 16) & 0xFF);
        }
        fwrite(row, 1, row_size, file);
    }

    free(row);
    fclose(file);
    LOG("SceDisplay: captured frame %u to %s", g_frame_count, file_path);
}

unsigned int SceDisplay_get_frame_count(void)
{
    return g_frame_count;
}

static void display_log_fps(void)
{
    static uint64_t last_ns;
    static unsigned int frames;
    static bool ready;
    static bool enabled;
    static bool overlay_enabled;
    static const char *metrics_path;
    struct timespec ts;
    uint64_t now;

    if (!ready) {
        enabled = vita2native_env_flag("VITA2NATIVE_FPS");
        overlay_enabled = fps_overlay_enabled();
        metrics_path = vita2native_env_str("VITA2NATIVE_METRICS_FILE");
        if (metrics_path && !metrics_path[0])
            metrics_path = NULL;
        ready = true;
    }
    if (!enabled && !overlay_enabled && !metrics_path)
        return;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;

    if (!last_ns) {
        last_ns = now;
        return;
    }

    frames++;
    if (now - last_ns >= 1000000000ull) {
        const double elapsed = (double)(now - last_ns) / 1e9;
        const double fps = (double)frames / elapsed;

        if (enabled)
            LOG("SceDisplay: %u frames in %.1f s (%.1f fps)", frames, elapsed, fps);

        fps_overlay_add_sample(fps);

        if (metrics_path) {
            FILE *file = fopen(metrics_path, "a");

            if (file) {
                fprintf(file, "%llu,%.3f,%u\n", (unsigned long long)(now / 1000000ull), fps,
                        g_frame_count);
                fclose(file);
            }
        }

        last_ns = now;
        frames = 0;
    }
}

EXPORT(SceDisplayUser, 0x7A410B64, int, sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    uint64_t present_started = frame_profile_enabled() ? frame_profile_now_ns() : 0;

    (void)sync;

    if (pParam)
        g_vita_conf_fb = *pParam;

    if (pParam) {
        unsigned int present_texture;
        int present_width, present_height;
        HostVideo *video = display_video();
        bool presented = false;

        if (video &&
            SceGxm_get_present_frame(pParam->base, &present_texture, &present_width,
                                     &present_height)) {
            int overlay_width = 0, overlay_height = 0;
            const uint32_t *fps_image = fps_overlay_image(&overlay_width, &overlay_height);

            host_video_set_overlay(video, fps_image, overlay_width, overlay_height);
            presented = host_video_present_texture(video, present_texture, present_width,
                                                   present_height);
        }

        if (!presented) {
            CommonDialogOverlay overlay;

            if (SceCommonDialog_getOverlay(&overlay)) {
                CommonDialogOverlay_render(&overlay, (uint32_t *)pParam->base, (int)pParam->width,
                                           (int)pParam->height, (int)pParam->pitch);
                SceCommonDialog_overlayPresented();
            }
            fps_overlay_render((uint32_t *)pParam->base, (int)pParam->width, (int)pParam->height,
                               (int)pParam->pitch);
            dump_framebuffer(pParam);

            if (video && pParam->base && pParam->width && pParam->height)
                host_video_present(video, pParam->base, (int)pParam->width, (int)pParam->height,
                                   (int)pParam->pitch);
        }
    }
    if (present_started) {
        uint64_t now = frame_profile_now_ns();

        frame_profile_present(now - present_started, now);
    }

    g_frame_count++;
    display_log_fps();

    return 0;
}

EXPORT(SceDisplayUser, 0x42AE6BBC, int, sceDisplayGetFrameBuf, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    (void)sync;

    if (pParam)
        *pParam = g_vita_conf_fb;

    return 0;
}

static void vblank_wait(unsigned int periods)
{
    uint64_t started;
    uint64_t now;
    uint64_t deadline;
    struct timespec ts;
    int ret;

    if (!periods)
        return;

    started = frame_profile_enabled() ? frame_profile_now_ns() : 0;
    now = host_monotonic_ns();

    /* First boundary strictly after now, then `periods` boundaries in all. */
    deadline = g_vblank_epoch_ns +
               ((now - g_vblank_epoch_ns) / VBLANK_PERIOD_NS + periods) * VBLANK_PERIOD_NS;

    ts.tv_sec = (time_t)(deadline / 1000000000ull);
    ts.tv_nsec = (long)(deadline % 1000000000ull);
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    } while (ret == EINTR);

    if (started)
        frame_profile_add_vblank_wait(frame_profile_now_ns() - started);
}

EXPORT(SceDisplay, 0x5795E898, int, sceDisplayWaitVblankStart, void)
{
    vblank_wait(1);
    return 0;
}

EXPORT(SceDisplay, 0xF51523CB, int, _sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0xA753B0CA, int, _sceDisplayGetFrameBuf, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x7178FADA, int, sceDisplayGetPrimaryHead, void)
{
    return 0;
}

EXPORT(SceDisplay, 0xA08CA60D, int, sceDisplayGetRefreshRate, float *pFps)
{
    if (!pFps)
        return -1;
    *pFps = 60.0f;
    return 0;
}

EXPORT(SceDisplay, 0x2EBFC7CB, int, sceDisplayGetMaximumFrameBufResolution, int *width,
       int *height)
{
    if (width)
        *width = 960;
    if (height)
        *height = 544;
    return 0;
}

EXPORT(SceDisplay, 0xB6FDE0BA, int, sceDisplayGetVcount, void)
{
    uint64_t now = host_monotonic_ns();
    uint64_t index =
        now > g_vblank_epoch_ns ? (now - g_vblank_epoch_ns) / VBLANK_PERIOD_NS : 0;

    return (int)(index & 0x7fffffffu);
}

static int display_wait_vblanks(unsigned int count)
{
    vblank_wait(count);
    return 0;
}

EXPORT(SceDisplay, 0x78B41B92, int, sceDisplayWaitVblankStartCB, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0xDD0A13B8, int, sceDisplayWaitVblankStartMulti, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x05F27764, int, sceDisplayWaitVblankStartMultiCB, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x9423560C, int, sceDisplayWaitSetFrameBuf, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0x814C90AF, int, sceDisplayWaitSetFrameBufCB, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0x7D9864A8, int, sceDisplayWaitSetFrameBufMulti, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x3E796EF5, int, sceDisplayWaitSetFrameBufMultiCB, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x6BDF4C4D, int, sceDisplayRegisterVblankStartCallback, SceUID uid)
{
    (void)uid;
    return 0;
}

EXPORT(SceDisplay, 0x98436A80, int, sceDisplayUnregisterVblankStartCallback, SceUID uid)
{
    (void)uid;
    return 0;
}

EXPORT(SceDisplay, 0x9686859E, int, sceDisplayGetVcountInternal, int display)
{
    (void)display;

    return sceDisplayGetVcount();
}

/* Internal/compat aliases used by the shell and media applications. */
EXPORT(SceDisplayUser, 0x89F86A1E, int, sceDisplaySetFrameBufInternal, int buffer_index, int unk,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    (void)unk;
    if (buffer_index != 0)
        return 0;
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x7A8CB78E, int, _sceDisplaySetFrameBufInternal, int buffer_index, int unk,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    (void)unk;
    if (buffer_index != 0)
        return 0;
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplayUser, 0x8C36B628, int, sceDisplaySetFrameBufForCompat,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x45BCB941, int, _sceDisplaySetFrameBufForCompat,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplayUser, 0x5213F74B, int, sceDisplayGetFrameBufInternal, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x86A8E436, int, _sceDisplayGetFrameBufInternal, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

static int display_get_maximum_resolution(int *width, int *height)
{
    return sceDisplayGetMaximumFrameBufResolution(width, height);
}

EXPORT_FUNC(SceDisplayUser, 0x8848ADDD, display_get_maximum_resolution)

EXPORT(SceDisplayUser, 0x0BFB78C6, int, sceDisplayGetResolutionInfoInternal, void *info)
{
    (void)info;
    return 0;
}

EXPORT(SceDisplay, 0xFEFEB240, int, _sceDisplayGetResolutionInfoInternal, void *info)
{
    (void)info;
    return 0;
}

DECLARE_LIBRARY(SceDisplay, 0x5ed8f994);
DECLARE_LIBRARY(SceDisplayUser, 0x4faacd11);

void SceDisplay_get_window_size(int *width, int *height)
{
    if (width)
        *width = 960;
    if (height)
        *height = 544;
}

int SceDisplay_init(HostGpuDevice device)
{
    (void)device;

    /* The vblank grid starts before any guest thread can wait on it. */
    g_vblank_epoch_ns = host_monotonic_ns();

    /* Bring SDL up before guest threads start rendering. SDL's X11/GLX
       initialisation and the guest's llvmpipe shader JIT both touch LLVM, and
       letting them run concurrently crashes in llvmpipe's MCJIT. */
    display_video();
    return 0;
}

int SceDisplay_finish(void)
{
    return 0;
}
