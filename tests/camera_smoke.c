/*
 * Synthetic camera test: open/start/read/stop/close, controls and the export
 * table entries.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/camera.h>
#include "test.h"

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);

static int check_export(uint32_t lib_nid, uint32_t func_nid, const char *what)
{
    if (module_get_func_export(lib_nid, func_nid, NULL))
        return 0;
    fprintf(stderr, "%s (0x%08x/0x%08x) is not in the export table\n", what, lib_nid, func_nid);
    return 1;
}

int main(void)
{
    SceCameraInfo info;
    SceCameraRead read;
    SceFVector3 location;
    uint8_t luma[64 * 64];
    uint8_t chroma_u[32 * 32];
    uint8_t chroma_v[32 * 32];
    uint8_t argb[32 * 32 * 4];
    int value;
    uint32_t non_black = 0;

    failures += check_export(0xDA91B3ED, 0xA462F801, "sceCameraOpen");
    failures += check_export(0xDA91B3ED, 0x79B5C2DE, "sceCameraRead");
    failures += check_export(0xDA91B3ED, 0x85D5951D, "sceCameraGetBrightness");

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution = SCE_CAMERA_RESOLUTION_320_240;
    info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.width = 64;
    info.height = 64;
    info.sizeIBase = sizeof(luma);
    info.sizeUBase = sizeof(chroma_u);
    info.sizeVBase = sizeof(chroma_v);
    info.pIBase = luma;
    info.pUBase = chroma_u;
    info.pVBase = chroma_v;

    CHECK(sceCameraOpen(SCE_CAMERA_DEVICE_FRONT, &info) == 0, "open failed");
    CHECK(info.width == 64 && info.height == 64, "open returned %ux%u", info.width, info.height);
    CHECK(sceCameraIsActive(SCE_CAMERA_DEVICE_FRONT) == 0, "camera active before start");

    memset(&read, 0, sizeof(read));
    read.size = sizeof(read);
    read.pIBase = luma;
    read.pUBase = chroma_u;
    read.pVBase = chroma_v;
    read.sizeIBase = sizeof(luma);
    read.sizeUBase = sizeof(chroma_u);
    read.sizeVBase = sizeof(chroma_v);
    CHECK(sceCameraRead(SCE_CAMERA_DEVICE_FRONT, &read) == SCE_CAMERA_ERROR_NOT_START,
          "read before start was accepted");

    CHECK(sceCameraStart(SCE_CAMERA_DEVICE_FRONT) == 0, "start failed");
    CHECK(sceCameraIsActive(SCE_CAMERA_DEVICE_FRONT) == 1, "camera not active after start");

    memset(luma, 0, sizeof(luma));
    memset(chroma_u, 0, sizeof(chroma_u));
    memset(chroma_v, 0, sizeof(chroma_v));
    CHECK(sceCameraRead(SCE_CAMERA_DEVICE_FRONT, &read) == 0, "read failed");
    CHECK(read.frame == 0, "first frame number is %llu", (unsigned long long)read.frame);
    for (uint32_t i = 0; i < sizeof(luma); i++)
        non_black += luma[i] != 0;
    CHECK(non_black > sizeof(luma) / 2, "luma plane is mostly empty (%u pixels)", non_black);
    CHECK(chroma_u[0] != 0 || chroma_v[0] != 0, "chroma planes are empty");

    CHECK(sceCameraRead(SCE_CAMERA_DEVICE_FRONT, &read) == 0 && read.frame == 1,
          "frame counter did not advance");

    CHECK(sceCameraGetBrightness(SCE_CAMERA_DEVICE_FRONT, &value) == 0 && value == 10,
          "default brightness is %d", value);
    CHECK(sceCameraSetBrightness(SCE_CAMERA_DEVICE_FRONT, 25) == 0, "set brightness failed");
    CHECK(sceCameraGetBrightness(SCE_CAMERA_DEVICE_FRONT, &value) == 0 && value == 25,
          "brightness did not stick");
    CHECK(sceCameraGetSaturation(SCE_CAMERA_DEVICE_FRONT, &value) == 0 && value == 10,
          "default saturation is %d", value);

    CHECK(sceCameraGetDeviceLocation(SCE_CAMERA_DEVICE_FRONT, &location) == 0,
          "device location failed");

    CHECK(sceCameraStop(SCE_CAMERA_DEVICE_FRONT) == 0, "stop failed");
    CHECK(sceCameraIsActive(SCE_CAMERA_DEVICE_FRONT) == 0, "camera still active after stop");
    CHECK(sceCameraClose(SCE_CAMERA_DEVICE_FRONT) == 0, "close failed");
    CHECK(sceCameraClose(SCE_CAMERA_DEVICE_FRONT) == SCE_CAMERA_ERROR_NOT_OPEN,
          "double close was accepted");

    /* ARGB output path. */
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.format = SCE_CAMERA_FORMAT_ARGB;
    info.resolution = SCE_CAMERA_RESOLUTION_320_240;
    info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.width = 32;
    info.height = 32;
    info.sizeIBase = sizeof(argb);
    info.pIBase = argb;
    CHECK(sceCameraOpen(SCE_CAMERA_DEVICE_BACK, &info) == 0, "ARGB open failed");
    CHECK(sceCameraStart(SCE_CAMERA_DEVICE_BACK) == 0, "ARGB start failed");

    memset(&read, 0, sizeof(read));
    read.size = sizeof(read);
    read.pIBase = argb;
    read.sizeIBase = sizeof(argb);
    CHECK(sceCameraRead(SCE_CAMERA_DEVICE_BACK, &read) == 0, "ARGB read failed");
    CHECK(argb[3] == 255 && (argb[0] || argb[1] || argb[2]), "ARGB pixel is empty");

    CHECK(sceCameraStop(SCE_CAMERA_DEVICE_BACK) == 0, "ARGB stop failed");
    CHECK(sceCameraClose(SCE_CAMERA_DEVICE_BACK) == 0, "ARGB close failed");

    return test_status("camera_smoke");
}
