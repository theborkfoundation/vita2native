/*
 * Controller entry points shared by both pad backends.
 *
 * Titles pick whichever variant their engine was written against
 * (Positive/PositiveExt/ReadBufferPositive...); all of them describe the same
 * pad state, so they all forward to sceCtrlPeekBufferPositive.
 */

#include <psp2/ctrl.h>
#include <string.h>

#include "module.h"

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);

EXPORT(SceCtrl, 0xA497B150, int, sceCtrlSetSamplingMode, SceCtrlPadInputMode mode)
{
    (void)mode;

    return 0;
}

EXPORT(SceCtrl, 0xB1DC4378, int, sceCtrlSetSamplingModeExt, SceCtrlPadInputMode mode)
{
    (void)mode;

    return 0;
}

EXPORT(SceCtrl, 0xA59454D3, int, sceCtrlPeekBufferPositiveExt, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0x67E7AB83, int, sceCtrlReadBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0xE2D99296, int, sceCtrlReadBufferPositiveExt, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0x104ED1A7, int, sceCtrlPeekBufferNegative, int port, SceCtrlData *pad_data,
       int count)
{
    /* Negative buffers report released inputs; the pad backend has none. */
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0x15F96FB0, int, sceCtrlReadBufferNegative, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferNegative(port, pad_data, count);
}

EXPORT(SceCtrl, 0x324F1B66, int, sceCtrlGetControllerPortInfo, SceCtrlPortInfo *info)
{
    if (!info)
        return -1;

    memset(info, 0, sizeof(*info));
    info->port[0] = SCE_CTRL_TYPE_PHY;

    return 0;
}

EXPORT(SceCtrl, 0xDBCAA0C9, int, sceCtrlSetActuator, int port, const SceCtrlActuator *state)
{
    (void)port;
    (void)state;

    return 0;
}

