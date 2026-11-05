#include <psp2/touch.h>

#include "module.h"
#include "util.h"

EXPORT(SceTouch, 0xFF082DF0, int, sceTouchPeek, SceUInt32 port, SceTouchData *pData,
       SceUInt32 nBufs)
{
    (void)port;
    (void)pData;
    (void)nBufs;

    /* Touch input is not wired up on the Linux backend yet. */
    return 0;
}

EXPORT(SceTouch, 0x10A2CA25, int, sceTouchGetPanelInfo, SceUInt32 port,
       SceTouchPanelInfo *pPanelInfo)
{
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
        return 0;
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
        return 0;
    default:
        return SCE_TOUCH_ERROR_INVALID_ARG;
    }
}

EXPORT(SceTouch, 0x1B9C5D14, int, sceTouchSetSamplingState, SceUInt32 port,
       SceTouchSamplingState state)
{
    (void)port;
    (void)state;

    /* Sampling only affects the speed at which the panel is polled; there is
       no panel here. */
    return 0;
}

DECLARE_LIBRARY(SceTouch, 0x3e4f4a81);

int SceTouch_init(void)
{
    return 0;
}
