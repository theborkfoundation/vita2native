/*
 * Exercises the SceTouch surface. This is a headless build, so no pointer
 * input exists: Peek must report one empty buffer and Read must return after
 * its bounded poll interval.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/touch.h>

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

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("touch_smoke: %s (line %d)\n", message, __LINE__);                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    SceTouchPanelInfo panel;
    SceTouchSamplingState state;
    SceTouchData data[2];
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;

    CHECK(sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panel) == 0, "front panel info failed");
    CHECK(panel.maxAaX == 1919 && panel.maxAaY == 1087, "front panel dimensions are wrong");
    CHECK(sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &panel) == 0, "back panel info failed");
    CHECK(panel.maxAaX == 1919 && panel.maxAaY == 889, "back panel dimensions are wrong");
    CHECK(sceTouchGetPanelInfo(2, &panel) == SCE_TOUCH_ERROR_INVALID_ARG,
          "an invalid port was accepted");
    CHECK(sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, NULL) == SCE_TOUCH_ERROR_INVALID_ARG,
          "a null panel pointer was accepted");

    CHECK(sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP) == 0,
          "SetSamplingState failed");
    state = SCE_TOUCH_SAMPLING_STATE_START;
    CHECK(sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &state) == 0 &&
              state == SCE_TOUCH_SAMPLING_STATE_STOP,
          "the sampling state did not round-trip");
    CHECK(sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, NULL) == SCE_TOUCH_ERROR_INVALID_ARG,
          "a null sampling state was accepted");

    memset(data, 0xa5, sizeof(data));
    CHECK(sceTouchPeek(SCE_TOUCH_PORT_FRONT, data, 1) == 1, "Peek failed");
    CHECK(data[0].reportNum == 0, "headless Peek reported touch input");
    CHECK(data[0].timeStamp > 0, "Peek did not timestamp the data");

    memset(data, 0, sizeof(data));
    CHECK(sceTouchPeek2(SCE_TOUCH_PORT_FRONT, data, 2) == 2, "Peek2 failed");
    CHECK(data[1].reportNum == 0, "Peek2 wrote unexpected touch data");

    memset(data, 0, sizeof(data));
    CHECK(sceTouchRead(SCE_TOUCH_PORT_BACK, data, 1) == 1, "Read failed");
    CHECK(data[0].reportNum == 0, "the back panel reported touch input");

    CHECK(sceTouchPeekRegion(SCE_TOUCH_PORT_FRONT, data, 1, 0) == 1, "PeekRegion failed");
    CHECK(sceTouchReadRegion(SCE_TOUCH_PORT_FRONT, data, 1, 1) == 1, "ReadRegion failed");
    CHECK(sceTouchRead2(SCE_TOUCH_PORT_FRONT, data, 1) == 1, "Read2 failed");

    CHECK(sceTouchPeek(SCE_TOUCH_PORT_FRONT, NULL, 1) == SCE_TOUCH_ERROR_INVALID_ARG,
          "a null data pointer was accepted");
    CHECK(sceTouchPeek(SCE_TOUCH_PORT_FRONT, data, 0) == SCE_TOUCH_ERROR_INVALID_ARG,
          "a zero buffer count was accepted");

    CHECK(sceTouchGetPixelDensity(&d1, &d2) == 0 && d1 == 22.0f && d2 == 22.0f,
          "GetPixelDensity is wrong");
    CHECK(sceTouchGetPixelDensity2(&d1, &d2, &d3, &d4) == 0 && d3 == 22.0f && d4 == 22.0f,
          "GetPixelDensity2 is wrong");
    CHECK(sceTouchGetPixelDensity(NULL, &d2) == SCE_TOUCH_ERROR_INVALID_ARG,
          "a null pixel density pointer was accepted");

    CHECK(sceTouchEnableTouchForce(SCE_TOUCH_PORT_FRONT) == 0, "EnableTouchForce failed");
    CHECK(sceTouchDisableTouchForce(SCE_TOUCH_PORT_FRONT) == 0, "DisableTouchForce failed");
    CHECK(sceTouchEnableTouchForce(2) == SCE_TOUCH_ERROR_INVALID_ARG,
          "EnableTouchForce accepted an invalid port");

    CHECK(sceTouchActivateRegion(0) == 0, "ActivateRegion failed");
    CHECK(sceTouchSetRegion(0, NULL) == 0, "SetRegion failed");
    CHECK(sceTouchSetRegionAttr(0, NULL) == 0, "SetRegionAttr failed");
    CHECK(sceTouchClearRegion(0) == 0, "ClearRegion failed");

    CHECK(sceTouchGetDeviceInfo(NULL) == 0, "GetDeviceInfo failed");
    CHECK(sceTouchGetProcessInfo(NULL) == 0, "GetProcessInfo failed");
    CHECK(sceTouchSetProcessPrivilege(0) == 0, "SetProcessPrivilege failed");

    puts("touch_smoke: ok");
    return 0;
}
