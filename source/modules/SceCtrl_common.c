/*
 * Controller entry points shared by both pad backends.
 *
 * Titles pick whichever variant their engine was written against
 * (Positive/PositiveExt/ReadBufferPositive...); all of them describe the same
 * pad state, so they all forward to sceCtrlPeekBufferPositive.
 */

#include <psp2/ctrl.h>
#include <string.h>

#include "host.h"
#include "module.h"

int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad_data, int count);

static SceCtrlPadInputMode g_sampling_mode = SCE_CTRL_MODE_DIGITAL;
static SceCtrlPadInputMode g_sampling_mode_ext = SCE_CTRL_MODE_DIGITAL;

EXPORT(SceCtrl, 0xA497B150, int, sceCtrlSetSamplingMode, SceCtrlPadInputMode mode)
{
    g_sampling_mode = mode;

    return 0;
}

EXPORT(SceCtrl, 0xB1DC4378, int, sceCtrlSetSamplingModeExt, SceCtrlPadInputMode mode)
{
    g_sampling_mode_ext = mode;

    return 0;
}

EXPORT(SceCtrl, 0xEC752AAF, int, sceCtrlGetSamplingMode, SceCtrlPadInputMode *mode)
{
    if (!mode)
        return -1;

    *mode = g_sampling_mode;

    return 0;
}

EXPORT(SceCtrl, 0xBD27F830, int, sceCtrlGetSamplingModeExt, SceCtrlPadInputMode *mode)
{
    if (!mode)
        return -1;

    *mode = g_sampling_mode_ext;

    return 0;
}

EXPORT(SceCtrl, 0x7E110E90, int, sceCtrlResetLightBar, void)
{
    return 0;
}

EXPORT(SceCtrl, 0xDA11D433, int, sceCtrlSetLightBar, int port, SceUInt8 r, SceUInt8 g,
       SceUInt8 b)
{
    (void)port;
    (void)r;
    (void)g;
    (void)b;

    return 0;
}

EXPORT(SceCtrl, 0x1FFFD965, int, sceCtrlIsMultiControllerSupported, void)
{
    return 0;
}

EXPORT(SceCtrl, 0x16D26DC7, int, sceCtrlDisconnect, void)
{
    return 0;
}

EXPORT(SceCtrl, 0x8F9B1CE5, int, sceCtrlGetBatteryInfo, int port, SceUInt8 *battery)
{
    (void)port;

    if (!battery)
        return -1;
    *battery = 100;

    return 0;
}

EXPORT(SceCtrl, 0x6FAFAD05, int, sceCtrlGetButtonIntercept, int *intercept)
{
    if (!intercept)
        return -1;
    *intercept = 0;

    return 0;
}

EXPORT(SceCtrl, 0x433D71F4, int, sceCtrlSetButtonIntercept, int intercept)
{
    (void)intercept;

    return 0;
}

/* SceCtrlData2 has the same size and layout as SceCtrlData. */
EXPORT(SceCtrl, 0x15F81E8C, int, sceCtrlPeekBufferPositive2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0xC4226A3E, int, sceCtrlReadBufferPositive2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlReadBufferPositive(port, pad_data, count);
}

EXPORT(SceCtrl, 0x860BF292, int, sceCtrlPeekBufferPositiveExt2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferPositiveExt(port, pad_data, count);
}

EXPORT(SceCtrl, 0xA7178860, int, sceCtrlReadBufferPositiveExt2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlReadBufferPositiveExt(port, pad_data, count);
}

EXPORT(SceCtrl, 0x81A89660, int, sceCtrlPeekBufferNegative2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlPeekBufferNegative(port, pad_data, count);
}

EXPORT(SceCtrl, 0x27A0C5FB, int, sceCtrlReadBufferNegative2, int port, SceCtrlData *pad_data,
       int count)
{
    return sceCtrlReadBufferNegative(port, pad_data, count);
}

EXPORT(SceCtrl, 0xB8844141, int, sceCtrlGetWirelessControllerInfo, void *info)
{
    if (info)
        memset(info, 0, 16);

    return 0;
}

EXPORT(SceCtrl, 0x0A06B7E4, int, sceCtrlRegisterBdRMCCallback, void)
{
    return 0;
}

EXPORT(SceCtrl, 0x103EB2B0, int, sceCtrlUnregisterBdRMCCallback, void)
{
    return 0;
}

EXPORT(SceCtrl, 0x031BE396, int, sceCtrlSingleControllerMode, int enable)
{
    (void)enable;

    return 0;
}

EXPORT(SceCtrl, 0x8A187F98, int, sceCtrlSetButtonRemappingInfo, int port, const void *info)
{
    (void)port;
    (void)info;

    return 0;
}

EXPORT(SceCtrl, 0xE9CB69C8, int, sceCtrlSetRapidFire, int port, int index,
       const SceCtrlRapidFireRule *rule)
{
    (void)port;
    (void)index;
    (void)rule;

    return 0;
}

EXPORT(SceCtrl, 0xD8294C9C, int, sceCtrlClearRapidFire, int port, int index)
{
    (void)port;
    (void)index;

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

    if (state)
        host_video_rumble(state->small, state->large);

    return 0;
}

/*
 * Analog-stick calibration queries. These only affect diagnostic tools; the
 * host pad backend applies no dead zone of its own.
 */
static int g_analog_stick_check_mode;

EXPORT(SceCtrl, 0xB2C98244, int, sceCtrlSetAnalogStickCheckMode, int port, int mode)
{
    (void)port;
    g_analog_stick_check_mode = mode;
    return 0;
}

EXPORT(SceCtrl, 0x35BCD3BB, int, sceCtrlGetAnalogStickCheckMode, int port, int *mode)
{
    (void)port;
    if (mode)
        *mode = g_analog_stick_check_mode;
    return 0;
}

EXPORT(SceCtrl, 0xFB12A0A7, int, sceCtrlSetAnalogStickCheckTarget, int port, int x, int y)
{
    (void)port;
    (void)x;
    (void)y;
    return 0;
}

EXPORT(SceCtrl, 0xD01DE63A, int, sceCtrlGetAnalogStickCheckTarget, int port, int *x, int *y)
{
    (void)port;
    if (x)
        *x = 0;
    if (y)
        *y = 0;
    return 0;
}

EXPORT(SceCtrl, 0xA2A56685, int, sceCtrlGetProcessStatus, int *status)
{
    if (status)
        *status = 0;
    return 0;
}

