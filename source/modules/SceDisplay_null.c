#include <psp2/display.h>

#include "host.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "util.h"

static SceDisplayFrameBuf g_vita_conf_fb;

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
    svcSleepThread(16666667ull);
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
