/*
 * SceTouch: front-panel touch input.
 *
 * The front panel is driven by the pointer (mouse or touchscreen) over the
 * presentation window. The SDL video thread samples the pointer, maps it through
 * the same letterbox rectangle it uses for the guest frame, and latches a press
 * until the guest reads it. The back panel has no host source.
 *
 * Read() polls for at most ~128 ms so a title that calls it in a loop still
 * gets a chance to run its render path when nothing is being touched.
 */

#include <pthread.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/touch.h>

#include "host.h"
#include "module.h"
#include "settings.h"
#include "util.h"

/* Weak so the touch smoke test can link without a display backend; without a
   display the counter stays at zero, which only affects scripted runs. */
extern unsigned int SceDisplay_get_frame_count(void) __attribute__((weak));

#define TOUCH_MAX_BUFFERS 8
#define TOUCH_READ_POLLS  16
#define TOUCH_READ_NS     8000000ll

static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static SceTouchSamplingState g_sampling[SCE_TOUCH_PORT_MAX_NUM] = {
    SCE_TOUCH_SAMPLING_STATE_START,
    SCE_TOUCH_SAMPLING_STATE_STOP,
};
static bool g_debug;
static bool g_logged_down;

/*
 * VITA2NATIVE_TOUCH_SCRIPT drives the front panel from a timetable keyed to
 * the display frame counter, mirroring VITA2NATIVE_CTRL_SCRIPT:
 *
 *   VITA2NATIVE_TOUCH_SCRIPT="40:960/544,60:"
 *
 * Each entry is `<frame>:<x>/<y>` to press or `<frame>:` to release, with x/y
 * in front-panel coordinates (0..1919, 0..1087). Comma separates entries, so the
 * coordinates use a slash. Titles that only react to the mouse/touchscreen
 * cannot be driven with the pad script.
 */
typedef struct {
    unsigned int frame;
    bool down;
    uint16_t x, y;
} TouchScriptEntry;

static TouchScriptEntry *g_script;
static unsigned int g_script_count;
static unsigned int g_script_index;
static bool g_script_loaded;
static bool g_script_down;
static uint16_t g_script_x, g_script_y;

static void touch_script_load(void)
{
    const char *env = vita2native_env_str("VITA2NATIVE_TOUCH_SCRIPT");
    char *copy;
    char *token;

    g_script_loaded = true;
    if (!env || !*env)
        return;

    copy = malloc(strlen(env) + 1);
    if (!copy)
        return;
    strcpy(copy, env);

    for (token = strtok(copy, ","); token; token = strtok(NULL, ",")) {
        char *colon = strchr(token, ':');
        TouchScriptEntry entry;
        char *coords;

        if (!colon)
            continue;
        *colon = '\0';
        memset(&entry, 0, sizeof(entry));
        entry.frame = (unsigned int)strtoul(token, NULL, 0);
        coords = colon + 1;

        while (*coords && isspace((unsigned char)*coords))
            coords++;
        if (*coords)
            entry.down = sscanf(coords, "%hu/%hu", &entry.x, &entry.y) == 2;

        {
            TouchScriptEntry *grown =
                realloc(g_script, (g_script_count + 1) * sizeof(*g_script));

            if (!grown)
                break;
            g_script = grown;
            g_script[g_script_count++] = entry;
        }
    }

    free(copy);
}

static void touch_script_poll(void)
{
    unsigned int frame;
    unsigned int before = g_script_index;

    if (!g_script_loaded)
        touch_script_load();
    if (!g_script_count)
        return;

    frame = SceDisplay_get_frame_count ? SceDisplay_get_frame_count() : 0;
    while (g_script_index < g_script_count && g_script[g_script_index].frame <= frame) {
        g_script_down = g_script[g_script_index].down;
        g_script_x = g_script[g_script_index].x;
        g_script_y = g_script[g_script_index].y;
        g_script_index++;
    }

    if (vita2native_env_flag("VITA2NATIVE_TOUCH_SCRIPT_DEBUG") && g_script_index != before) {
        fprintf(stderr, "vita2native: SceTouch script frame %u -> %s %u,%u\n", frame,
                g_script_down ? "down" : "up", (unsigned)g_script_x, (unsigned)g_script_y);
    }
}

static void build_touch_data(SceUInt32 port, SceTouchData *data)
{
    HostTouchState touch;
    bool down;

    memset(data, 0, sizeof(*data));
    data->timeStamp = host_monotonic_ns() / 1000ull;

    if (port != SCE_TOUCH_PORT_FRONT)
        return;

    touch_script_poll();
    if (g_script_count) {
        if (!g_script_down)
            return;
        touch.active = true;
        touch.down = true;
        touch.x = g_script_x;
        touch.y = g_script_y;
    } else if (!host_video_touch_state(&touch) || !touch.active) {
        return;
    }

    down = touch.down;
    if (g_debug && down != g_logged_down) {
        fprintf(stderr, "vita2native: SceTouch %s x=%u y=%u\n", down ? "down" : "up",
                (unsigned)touch.x, (unsigned)touch.y);
        g_logged_down = down;
    }

    if (!down)
        return;

    data->reportNum = 1;
    data->report[0].id = 0;
    data->report[0].force = 64;
    data->report[0].x = (SceInt16)touch.x;
    data->report[0].y = (SceInt16)touch.y;
    data->report[0].info = 0;
}

static int touch_get(SceUInt32 port, SceTouchData *data, SceUInt32 n_bufs, bool block)
{
    SceTouchData sample;

    if (g_debug) {
        static unsigned int calls;

        calls++;
        if (calls <= 20 || (calls % 600) == 0) {
            fprintf(stderr, "vita2native: SceTouch call #%u port %u nbufs %u block %d\n", calls,
                    (unsigned)port, (unsigned)n_bufs, (int)block);
            fflush(stderr);
        }
    }

    if (port >= SCE_TOUCH_PORT_MAX_NUM || !data || n_bufs == 0 || n_bufs > TOUCH_MAX_BUFFERS)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    if (g_debug) {
        static unsigned int accepted;

        accepted++;
        if ((accepted % 600) == 0) {
            fprintf(stderr, "vita2native: SceTouch read #%u port %u\n", accepted, (unsigned)port);
            fflush(stderr);
        }
    }

    if (block) {
        for (int i = 0; i < TOUCH_READ_POLLS; i++) {
            pthread_mutex_lock(&g_touch_lock);
            build_touch_data(port, &sample);
            pthread_mutex_unlock(&g_touch_lock);

            if (sample.reportNum > 0)
                break;
            host_sleep_ns(TOUCH_READ_NS);
        }
    } else {
        pthread_mutex_lock(&g_touch_lock);
        build_touch_data(port, &sample);
        pthread_mutex_unlock(&g_touch_lock);
    }

    for (SceUInt32 i = 0; i < n_bufs; i++)
        data[i] = sample;

    return (int)n_bufs;
}

EXPORT(SceTouch, 0xFF082DF0, int, sceTouchPeek, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs)
{
    return touch_get(port, pData, nBufs, false);
}

EXPORT(SceTouch, 0x10A2CA25, int, sceTouchGetPanelInfo, SceUInt32 port,
       SceTouchPanelInfo *pPanelInfo)
{
    if (port >= SCE_TOUCH_PORT_MAX_NUM || !pPanelInfo)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    memset(pPanelInfo, 0, sizeof(*pPanelInfo));

    switch (port) {
    case SCE_TOUCH_PORT_FRONT:
        pPanelInfo->minAaX = 0;
        pPanelInfo->minAaY = 0;
        pPanelInfo->maxAaX = 1919;
        pPanelInfo->maxAaY = 1087;
        pPanelInfo->minDispX = 0;
        pPanelInfo->minDispY = 0;
        pPanelInfo->maxDispX = 1919;
        pPanelInfo->maxDispY = 1087;
        pPanelInfo->minForce = 1;
        pPanelInfo->maxForce = 128;
        break;
    case SCE_TOUCH_PORT_BACK:
        pPanelInfo->minAaX = 0;
        pPanelInfo->minAaY = 108;
        pPanelInfo->maxAaX = 1919;
        pPanelInfo->maxAaY = 889;
        pPanelInfo->minDispX = 0;
        pPanelInfo->minDispY = 0;
        pPanelInfo->maxDispX = 1919;
        pPanelInfo->maxDispY = 1087;
        pPanelInfo->minForce = 1;
        pPanelInfo->maxForce = 128;
        break;
    }

    return 0;
}

EXPORT(SceTouch, 0x1B9C5D14, int, sceTouchSetSamplingState, SceUInt32 port,
       SceTouchSamplingState state)
{
    if (port >= SCE_TOUCH_PORT_MAX_NUM)
        return SCE_TOUCH_ERROR_INVALID_ARG;
    if (state != SCE_TOUCH_SAMPLING_STATE_STOP && state != SCE_TOUCH_SAMPLING_STATE_START)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    g_sampling[port] = state;
    if (g_debug)
        fprintf(stderr, "vita2native: SceTouch sampling port %u -> %d\n", (unsigned)port,
                (int)state);
    return 0;
}

/* --------------------------------------------------------------- read/peek */

EXPORT(SceTouch, 0x169A1D58, int, sceTouchRead, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs)
{
    return touch_get(port, pData, nBufs, true);
}

EXPORT(SceTouch, 0x39401BEA, int, sceTouchRead2, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs)
{
    return sceTouchRead(port, pData, nBufs);
}

EXPORT(SceTouch, 0x3AD3D0A1, int, sceTouchPeek2, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs)
{
    return sceTouchPeek(port, pData, nBufs);
}

EXPORT(SceTouch, 0x93B5635B, int, sceTouchReadRegion, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs, int region)
{
    (void)region;
    return sceTouchRead(port, pData, nBufs);
}

EXPORT(SceTouch, 0x9F0ACAF9, int, sceTouchReadRegionExt, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs, int region)
{
    (void)region;
    return sceTouchRead(port, pData, nBufs);
}

EXPORT(SceTouch, 0x04440622, int, sceTouchPeekRegion, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs, int region)
{
    (void)region;
    return sceTouchPeek(port, pData, nBufs);
}

EXPORT(SceTouch, 0x2CF6D7E2, int, sceTouchPeekRegionExt, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs, int region)
{
    (void)region;
    return sceTouchPeek(port, pData, nBufs);
}

/* ------------------------------------------------------------ panel state */

EXPORT(SceTouch, 0x26531526, int, sceTouchGetSamplingState, SceUInt32 port,
       SceTouchSamplingState *pState)
{
    if (port >= SCE_TOUCH_PORT_MAX_NUM || !pState)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    *pState = g_sampling[port];
    return 0;
}

EXPORT(SceTouch, 0xDC8671EA, int, sceTouchGetSamplingStateExt, SceUInt32 port,
       SceTouchSamplingState *pState)
{
    return sceTouchGetSamplingState(port, pState);
}

EXPORT(SceTouch, 0x13CDFC43, int, sceTouchSetSamplingStateExt, SceUInt32 port,
       SceTouchSamplingState state)
{
    return sceTouchSetSamplingState(port, state);
}

EXPORT(SceTouch, 0xB18370C2, int, sceTouchEnableTouchForce, SceUInt32 port)
{
    return port < SCE_TOUCH_PORT_MAX_NUM ? 0 : SCE_TOUCH_ERROR_INVALID_ARG;
}

EXPORT(SceTouch, 0x41194411, int, sceTouchDisableTouchForce, SceUInt32 port)
{
    return port < SCE_TOUCH_PORT_MAX_NUM ? 0 : SCE_TOUCH_ERROR_INVALID_ARG;
}

EXPORT(SceTouch, 0xA3FE877B, int, sceTouchEnableTouchForceExt, SceUInt32 port)
{
    return sceTouchEnableTouchForce(port);
}

EXPORT(SceTouch, 0xED25DFDC, int, sceTouchDisableTouchForceExt, SceUInt32 port)
{
    return sceTouchDisableTouchForce(port);
}

EXPORT(SceTouch, 0x012EAE3A, int, sceTouchEnableIdleTimerCancelSetting, int enable)
{
    (void)enable;
    return 0;
}

EXPORT(SceTouch, 0x609EA489, int, sceTouchSetProcessPrivilege, int privilege)
{
    (void)privilege;
    return 0;
}

/* --------------------------------------------------------------- regions */

EXPORT(SceTouch, 0xBD59D652, int, sceTouchActivateRegion, int region)
{
    (void)region;
    return 0;
}

EXPORT(SceTouch, 0x2D94EAEA, int, sceTouchClearRegion, int region)
{
    (void)region;
    return 0;
}

EXPORT(SceTouch, 0x6134DFBE, int, sceTouchSetRegion, int region, const void *param)
{
    (void)region;
    (void)param;
    return 0;
}

EXPORT(SceTouch, 0x08DD4C7C, int, sceTouchSetRegionAttr, int region, const void *attr)
{
    (void)region;
    (void)attr;
    return 0;
}

/* ----------------------------------------------------------- device info */

EXPORT(SceTouch, 0xD7889B91, int, sceTouchGetDeviceInfo, void *info)
{
    (void)info;
    return 0;
}

EXPORT(SceTouch, 0xAB364C23, int, sceTouchGetProcessInfo, void *info)
{
    (void)info;
    return 0;
}

EXPORT(SceTouch, 0xF0704CF3, int, sceTouchGetPixelDensity, float *p1, float *p2)
{
    if (!p1 || !p2)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    *p1 = 22.0f;
    *p2 = 22.0f;
    return 0;
}

EXPORT(SceTouch, 0xE0C1DB92, int, sceTouchGetPixelDensity2, float *p1, float *p2, float *p3,
       float *p4)
{
    if (!p1 || !p2 || !p3 || !p4)
        return SCE_TOUCH_ERROR_INVALID_ARG;

    *p1 = 22.0f;
    *p2 = 22.0f;
    *p3 = 22.0f;
    *p4 = 22.0f;
    return 0;
}

DECLARE_LIBRARY(SceTouch, 0x3e4f4a81);

int SceTouch_init(void)
{
    g_debug = vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG");
    return 0;
}
