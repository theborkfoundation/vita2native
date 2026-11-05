#include <psp2/display.h>

#include "host.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "util.h"

static SceDisplayFrameBuf g_vita_conf_fb;
static int g_vcount;

EXPORT(SceDisplayUser, 0x7A410B64, int, sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    (void)sync;

    if (pParam)
        g_vita_conf_fb = *pParam;

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

EXPORT(SceDisplay, 0x5795E898, int, sceDisplayWaitVblankStart, void)
{
    host_sleep_ns(16666667ull);
    __sync_fetch_and_add(&g_vcount, 1);
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
    return __sync_fetch_and_add(&g_vcount, 0);
}

static int display_wait_vblanks(unsigned int count)
{
    while (count--) {
        host_sleep_ns(16666667ull);
        __sync_fetch_and_add(&g_vcount, 1);
    }
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

DECLARE_LIBRARY(SceDisplay, 0x5ed8f994);
DECLARE_LIBRARY(SceDisplayUser, 0x4faacd11);

int SceDisplay_init(HostGpuDevice device)
{
    (void)device;
    return 0;
}

int SceDisplay_finish(void)
{
    return 0;
}
