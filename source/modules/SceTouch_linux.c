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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/touch.h>

#include "host.h"
#include "module.h"
#include "util.h"

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

static void build_touch_data(SceUInt32 port, SceTouchData *data)
{
    HostTouchState touch;
    bool down;

    memset(data, 0, sizeof(*data));
    data->timeStamp = host_monotonic_ns() / 1000ull;

    if (port != SCE_TOUCH_PORT_FRONT)
        return;

    if (!host_video_touch_state(&touch) || !touch.active)
        return;

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

    if (port >= SCE_TOUCH_PORT_MAX_NUM || !data || n_bufs == 0 || n_bufs > TOUCH_MAX_BUFFERS)
        return SCE_TOUCH_ERROR_INVALID_ARG;

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
    g_debug = getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;
    return 0;
}
