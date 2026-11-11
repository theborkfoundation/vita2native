/*
 * Exercises the SceTouch surface. This is a headless build, so no pointer
 * input exists: Peek must report one empty buffer and Read must return after
 * its bounded poll interval.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/touch.h>
#include "test.h"

int sceTouchPeek(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs);
int sceTouchPeek2(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs);
int sceTouchRead(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs);
int sceTouchRead2(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs);
int sceTouchPeekRegion(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region);
int sceTouchReadRegion(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region);
int sceTouchGetPanelInfo(SceUInt32 port, SceTouchPanelInfo *info);
int sceTouchSetSamplingState(SceUInt32 port, SceTouchSamplingState state);
int sceTouchGetSamplingState(SceUInt32 port, SceTouchSamplingState *state);
int sceTouchGetPixelDensity(float *p1, float *p2);
int sceTouchGetPixelDensity2(float *p1, float *p2, float *p3, float *p4);
int sceTouchEnableTouchForce(SceUInt32 port);
int sceTouchDisableTouchForce(SceUInt32 port);
int sceTouchActivateRegion(int region);
int sceTouchClearRegion(int region);
int sceTouchSetRegion(int region, const void *param);
int sceTouchSetRegionAttr(int region, const void *attr);
int sceTouchGetDeviceInfo(void *info);
int sceTouchGetProcessInfo(void *info);
int sceTouchSetProcessPrivilege(int privilege);

int main(void)
{
    SceTouchPanelInfo panel;
    SceTouchSamplingState state;
    SceTouchData data[2];
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;

    REQUIRE(sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) == 0, "front panel info failed");
    REQUIRE(panel.maxAaX == 1919 && panel.maxAaY == 1087, "front panel dimensions are wrong");
    REQUIRE(sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &panel) == 0, "back panel info failed");
    REQUIRE(panel.maxAaX == 1919 && panel.maxAaY == 889, "back panel dimensions are wrong");
    REQUIRE(sceTouchGetPanelInfo(2, &panel) == SCE_TOUCH_ERROR_INVALID_ARG,
            "an invalid port was accepted");
    REQUIRE(sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, NULL) == SCE_TOUCH_ERROR_INVALID_ARG,
            "a null panel pointer was accepted");

    REQUIRE(sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP) == 0,
            "SetSamplingState failed");
    state = SCE_TOUCH_SAMPLING_STATE_START;
    REQUIRE(sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &state) == 0 &&
                state == SCE_TOUCH_SAMPLING_STATE_STOP,
            "the sampling state did not round-trip");
    REQUIRE(sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, NULL) == SCE_TOUCH_ERROR_INVALID_ARG,
            "a null sampling state was accepted");

    memset(data, 0xa5, sizeof(data));
    REQUIRE(sceTouchPeek(SCE_TOUCH_PORT_FRONT, data, 1) == 1, "Peek failed");
    REQUIRE(data[0].reportNum == 0, "headless Peek reported touch input");
    REQUIRE(data[0].timeStamp > 0, "Peek did not timestamp the data");

    memset(data, 0, sizeof(data));
    REQUIRE(sceTouchPeek2(SCE_TOUCH_PORT_FRONT, data, 2) == 2, "Peek2 failed");
    REQUIRE(data[1].reportNum == 0, "Peek2 wrote unexpected touch data");

    memset(data, 0, sizeof(data));
    REQUIRE(sceTouchRead(SCE_TOUCH_PORT_BACK, data, 1) == 1, "Read failed");
    REQUIRE(data[0].reportNum == 0, "the back panel reported touch input");

    REQUIRE(sceTouchPeekRegion(SCE_TOUCH_PORT_FRONT, data, 1, 0) == 1, "PeekRegion failed");
    REQUIRE(sceTouchReadRegion(SCE_TOUCH_PORT_FRONT, data, 1, 1) == 1, "ReadRegion failed");
    REQUIRE(sceTouchRead2(SCE_TOUCH_PORT_FRONT, data, 1) == 1, "Read2 failed");

    REQUIRE(sceTouchPeek(SCE_TOUCH_PORT_FRONT, NULL, 1) == SCE_TOUCH_ERROR_INVALID_ARG,
            "a null data pointer was accepted");
    REQUIRE(sceTouchPeek(SCE_TOUCH_PORT_FRONT, data, 0) == SCE_TOUCH_ERROR_INVALID_ARG,
            "a zero buffer count was accepted");

    REQUIRE(sceTouchGetPixelDensity(&d1, &d2) == 0 && d1 == 22.0f && d2 == 22.0f,
            "GetPixelDensity is wrong");
    REQUIRE(sceTouchGetPixelDensity2(&d1, &d2, &d3, &d4) == 0 && d3 == 22.0f && d4 == 22.0f,
            "GetPixelDensity2 is wrong");
    REQUIRE(sceTouchGetPixelDensity(NULL, &d2) == SCE_TOUCH_ERROR_INVALID_ARG,
            "a null pixel density pointer was accepted");

    REQUIRE(sceTouchEnableTouchForce(SCE_TOUCH_PORT_FRONT) == 0, "EnableTouchForce failed");
    REQUIRE(sceTouchDisableTouchForce(SCE_TOUCH_PORT_FRONT) == 0, "DisableTouchForce failed");
    REQUIRE(sceTouchEnableTouchForce(2) == SCE_TOUCH_ERROR_INVALID_ARG,
            "EnableTouchForce accepted an invalid port");

    REQUIRE(sceTouchActivateRegion(0) == 0, "ActivateRegion failed");
    REQUIRE(sceTouchSetRegion(0, NULL) == 0, "SetRegion failed");
    REQUIRE(sceTouchSetRegionAttr(0, NULL) == 0, "SetRegionAttr failed");
    REQUIRE(sceTouchClearRegion(0) == 0, "ClearRegion failed");

    REQUIRE(sceTouchGetDeviceInfo(NULL) == 0, "GetDeviceInfo failed");
    REQUIRE(sceTouchGetProcessInfo(NULL) == 0, "GetProcessInfo failed");
    REQUIRE(sceTouchSetProcessPrivilege(0) == 0, "SetProcessPrivilege failed");

    puts("touch_smoke: ok");
    return 0;
}
