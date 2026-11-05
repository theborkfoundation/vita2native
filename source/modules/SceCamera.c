/*
 * SceCamera: a synthetic camera.
 *
 * The host has no camera, so device 0 presents a generated colour-bar test
 * pattern with a moving marker. Controls are stored and read back (they do not
 * alter the pattern), which lets QR scanners, photo apps and camera-driven
 * menus exercise their normal code paths instead of failing at Open.
 *
 * Front/back device numbers are accepted; any other device does not exist.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <psp2/camera.h>

#include "module.h"

typedef enum {
    CAM_SATURATION,
    CAM_BRIGHTNESS,
    CAM_CONTRAST,
    CAM_SHARPNESS,
    CAM_REVERSE,
    CAM_EFFECT,
    CAM_EV,
    CAM_ZOOM,
    CAM_ANTIFLICKER,
    CAM_ISO,
    CAM_GAIN,
    CAM_WHITE_BALANCE,
    CAM_BACKLIGHT,
    CAM_NIGHTMODE,
    CAM_EXPOSURE_CEILING,
    CAM_AUTO_CONTROL_HOLD,
    CAM_IMAGE_QUALITY,
    CAM_NOISE_REDUCTION,
    CAM_SHARPNESS_OFF,
    CAM_PROP_COUNT,
} CameraProperty;

typedef struct {
    bool open;
    bool active;
    SceCameraInfo info;
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    int properties[CAM_PROP_COUNT];
} CameraState;

static CameraState g_camera;

static const int g_property_defaults[CAM_PROP_COUNT] = {
    [CAM_SATURATION] = 10,
    [CAM_BRIGHTNESS] = 10,
    [CAM_CONTRAST] = 10,
    [CAM_SHARPNESS] = 1,
    [CAM_ANTIFLICKER] = SCE_CAMERA_ANTIFLICKER_AUTO,
    [CAM_ISO] = SCE_CAMERA_ISO_AUTO,
    [CAM_IMAGE_QUALITY] = 10,
};

static bool camera_device(int devnum)
{
    return devnum == SCE_CAMERA_DEVICE_FRONT || devnum == SCE_CAMERA_DEVICE_BACK;
}

static void camera_resolution(uint16_t resolution, uint16_t *width, uint16_t *height)
{
    switch (resolution) {
    case SCE_CAMERA_RESOLUTION_640_480:
        *width = 640;
        *height = 480;
        break;
    case SCE_CAMERA_RESOLUTION_320_240:
        *width = 320;
        *height = 240;
        break;
    case SCE_CAMERA_RESOLUTION_160_120:
        *width = 160;
        *height = 120;
        break;
    case SCE_CAMERA_RESOLUTION_352_288:
        *width = 352;
        *height = 288;
        break;
    case SCE_CAMERA_RESOLUTION_176_144:
        *width = 176;
        *height = 144;
        break;
    case SCE_CAMERA_RESOLUTION_480_272:
        *width = 480;
        *height = 272;
        break;
    case SCE_CAMERA_RESOLUTION_640_360:
        *width = 640;
        *height = 360;
        break;
    default:
        *width = 640;
        *height = 480;
        break;
    }
}

/* SMPTE-style bars, plus a moving white marker column. */
static void camera_pixel(uint32_t x, uint32_t y, uint32_t frame, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 0, 0, 0 },
    };
    uint32_t band = x * 8 / (g_camera.width ? g_camera.width : 1);
    uint32_t marker = (frame * 8) % (g_camera.width ? g_camera.width : 1);
    int reverse = g_camera.properties[CAM_REVERSE];

    if (reverse & SCE_CAMERA_REVERSE_MIRROR)
        x = g_camera.width - 1 - x;
    if (reverse & SCE_CAMERA_REVERSE_FLIP)
        y = g_camera.height - 1 - y;
    (void)y;

    if (x == marker) {
        *r = *g = *b = 255;
        return;
    }

    if (band > 7)
        band = 7;
    *r = bars[band][0];
    *g = bars[band][1];
    *b = bars[band][2];
}

static void camera_fill_argb(uint8_t *buffer, size_t size, bool abgr)
{
    uint32_t width = g_camera.width;
    uint32_t height = g_camera.height;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b;
            size_t offset = ((size_t)y * width + x) * 4;

            if (offset + 4 > size)
                return;
            camera_pixel(x, y, g_camera.frame, &r, &g, &b);
            if (abgr) {
                buffer[offset + 0] = r;
                buffer[offset + 1] = g;
                buffer[offset + 2] = b;
            } else {
                buffer[offset + 0] = b;
                buffer[offset + 1] = g;
                buffer[offset + 2] = r;
            }
            buffer[offset + 3] = 255;
        }
    }
}

static void camera_fill_planes(uint8_t *i_base, size_t i_size, uint8_t *u_base, size_t u_size,
                               uint8_t *v_base, size_t v_size, bool packed, bool yuv420)
{
    uint32_t width = g_camera.width;
    uint32_t height = g_camera.height;
    uint32_t chroma_width = width / 2;
    uint32_t chroma_height = yuv420 ? height / 2 : height;

    if (packed) {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x += 2) {
                uint8_t r0, g0, b0, r1, g1, b1;
                size_t offset = ((size_t)y * width + x) * 2;

                if (offset + 4 > i_size)
                    return;
                camera_pixel(x, y, g_camera.frame, &r0, &g0, &b0);
                camera_pixel(x + 1, y, g_camera.frame, &r1, &g1, &b1);
                i_base[offset + 0] = (uint8_t)((77 * r0 + 150 * g0 + 29 * b0) >> 8);
                i_base[offset + 1] = (uint8_t)(128 + ((128 * b0 - 107 * r0 - 21 * g0) >> 8));
                i_base[offset + 2] = (uint8_t)((77 * r1 + 150 * g1 + 29 * b1) >> 8);
                i_base[offset + 3] = (uint8_t)(128 + ((128 * r1 - 43 * g1 - 85 * b1) >> 8));
            }
        }
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b;
            size_t offset = (size_t)y * width + x;

            if (offset >= i_size)
                return;
            camera_pixel(x, y, g_camera.frame, &r, &g, &b);
            i_base[offset] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    for (uint32_t y = 0; y < chroma_height; y++) {
        for (uint32_t x = 0; x < chroma_width; x++) {
            uint8_t r, g, b;
            size_t offset = (size_t)y * chroma_width + x;

            if (offset >= u_size || offset >= v_size)
                return;
            camera_pixel(x * 2, y * (yuv420 ? 2 : 1), g_camera.frame, &r, &g, &b);
            u_base[offset] = (uint8_t)(128 + ((128 * b - 107 * r - 21 * g) >> 8));
            v_base[offset] = (uint8_t)(128 + ((128 * r - 43 * g - 85 * b) >> 8));
        }
    }
}

EXPORT(SceCamera, 0xA462F801, int, sceCameraOpen, int devnum, SceCameraInfo *info)
{
    uint16_t width, height;

    if (!camera_device(devnum) || !info)
        return SCE_CAMERA_ERROR_PARAM;
    if (info->size != sizeof(*info))
        return SCE_CAMERA_ERROR_PARAM;
    if (g_camera.open)
        return SCE_CAMERA_ERROR_ALREADY_OPEN;

    memset(&g_camera, 0, sizeof(g_camera));
    g_camera.info = *info;
    g_camera.width = info->width;
    g_camera.height = info->height;
    if (!g_camera.width || !g_camera.height) {
        camera_resolution(info->resolution, &width, &height);
        g_camera.width = width;
        g_camera.height = height;
        g_camera.info.width = width;
        g_camera.info.height = height;
    }
    memcpy(g_camera.properties, g_property_defaults, sizeof(g_camera.properties));
    g_camera.open = true;

    info->width = (uint16_t)g_camera.width;
    info->height = (uint16_t)g_camera.height;

    return 0;
}

EXPORT(SceCamera, 0xCD6E1CFC, int, sceCameraClose, int devnum)
{
    if (!camera_device(devnum))
        return SCE_CAMERA_ERROR_PARAM;
    if (!g_camera.open)
        return SCE_CAMERA_ERROR_NOT_OPEN;

    g_camera.open = false;
    g_camera.active = false;

    return 0;
}

EXPORT(SceCamera, 0xA8FEAE35, int, sceCameraStart, int devnum)
{
    if (!camera_device(devnum))
        return SCE_CAMERA_ERROR_PARAM;
    if (!g_camera.open)
        return SCE_CAMERA_ERROR_NOT_OPEN;
    if (g_camera.active)
        return SCE_CAMERA_ERROR_ALREADY_START;

    g_camera.active = true;
    g_camera.frame = 0;

    return 0;
}

EXPORT(SceCamera, 0x1DD9C9CE, int, sceCameraStop, int devnum)
{
    if (!camera_device(devnum))
        return SCE_CAMERA_ERROR_PARAM;
    if (!g_camera.active)
        return SCE_CAMERA_ERROR_NOT_START;

    g_camera.active = false;

    return 0;
}

EXPORT(SceCamera, 0x103A75B8, int, sceCameraIsActive, int devnum)
{
    if (!camera_device(devnum))
        return 0;

    return g_camera.open && g_camera.active ? 1 : 0;
}

EXPORT(SceCamera, 0x79B5C2DE, int, sceCameraRead, int devnum, SceCameraRead *read)
{
    uint8_t *i_base;
    uint8_t *u_base;
    uint8_t *v_base;
    size_t i_size;
    size_t u_size;
    size_t v_size;

    if (!camera_device(devnum) || !read)
        return SCE_CAMERA_ERROR_PARAM;
    if (!g_camera.active)
        return SCE_CAMERA_ERROR_NOT_START;
    if (read->size != sizeof(*read))
        return SCE_CAMERA_ERROR_PARAM;

    i_base = read->pIBase ? read->pIBase : g_camera.info.pIBase;
    u_base = read->pUBase ? read->pUBase : g_camera.info.pUBase;
    v_base = read->pVBase ? read->pVBase : g_camera.info.pVBase;
    i_size = read->sizeIBase ? read->sizeIBase : g_camera.info.sizeIBase;
    u_size = read->sizeUBase ? read->sizeUBase : g_camera.info.sizeUBase;
    v_size = read->sizeVBase ? read->sizeVBase : g_camera.info.sizeVBase;

    if (!i_base || !i_size)
        return SCE_CAMERA_ERROR_PARAM;

    switch (g_camera.info.format) {
    case SCE_CAMERA_FORMAT_YUV422_PACKED:
        camera_fill_planes(i_base, i_size, NULL, 0, NULL, 0, true, false);
        break;
    case SCE_CAMERA_FORMAT_YUV422_PLANE:
        if (!u_base || !v_base)
            return SCE_CAMERA_ERROR_PARAM;
        camera_fill_planes(i_base, i_size, u_base, u_size, v_base, v_size, false, false);
        break;
    case SCE_CAMERA_FORMAT_YUV420_PLANE:
        if (!u_base || !v_base)
            return SCE_CAMERA_ERROR_PARAM;
        camera_fill_planes(i_base, i_size, u_base, u_size, v_base, v_size, false, true);
        break;
    case SCE_CAMERA_FORMAT_ARGB:
        camera_fill_argb(i_base, i_size, false);
        break;
    case SCE_CAMERA_FORMAT_ABGR:
        camera_fill_argb(i_base, i_size, true);
        break;
    case SCE_CAMERA_FORMAT_RAW8:
        for (uint32_t y = 0; y < g_camera.height; y++) {
            for (uint32_t x = 0; x < g_camera.width; x++) {
                uint8_t r, g, b;
                size_t offset = (size_t)y * g_camera.width + x;

                if (offset >= i_size)
                    break;
                camera_pixel(x, y, g_camera.frame, &r, &g, &b);
                i_base[offset] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
        break;
    default:
        return SCE_CAMERA_ERROR_FORMAT_UNKNOWN;
    }

    read->frame = g_camera.frame++;
    read->timestamp = (uint64_t)g_camera.frame * 1000000ull /
                      (g_camera.info.framerate ? g_camera.info.framerate : 30);
    read->status = 0;

    return 0;
}

EXPORT(SceCamera, 0x274EF751, int, sceCameraGetDeviceLocation, int devnum, SceFVector3 *location)
{
    if (!camera_device(devnum) || !location)
        return SCE_CAMERA_ERROR_PARAM;

    memset(location, 0, sizeof(*location));

    return 0;
}

static int camera_check(int devnum)
{
    if (!camera_device(devnum))
        return SCE_CAMERA_ERROR_PARAM;
    if (!g_camera.open)
        return SCE_CAMERA_ERROR_NOT_OPEN;
    return 0;
}

#define CAMERA_PROPERTY(name, get_nid, set_nid, field)                                             \
    EXPORT(SceCamera, get_nid, int, sceCameraGet##name, int devnum, int *value)                    \
    {                                                                                              \
        int result = camera_check(devnum);                                                         \
                                                                                                   \
        if (result < 0)                                                                            \
            return result;                                                                         \
        if (!value)                                                                                \
            return SCE_CAMERA_ERROR_PARAM;                                                         \
        *value = g_camera.properties[field];                                                       \
        return 0;                                                                                  \
    }                                                                                              \
                                                                                                   \
    EXPORT(SceCamera, set_nid, int, sceCameraSet##name, int devnum, int value)                     \
    {                                                                                              \
        int result = camera_check(devnum);                                                         \
                                                                                                   \
        if (result < 0)                                                                            \
            return result;                                                                         \
        g_camera.properties[field] = value;                                                        \
        return 0;                                                                                  \
    }

CAMERA_PROPERTY(Saturation, 0x624F7653, 0xF9F7CA3D, CAM_SATURATION)
CAMERA_PROPERTY(Brightness, 0x85D5951D, 0x98D71588, CAM_BRIGHTNESS)
CAMERA_PROPERTY(Contrast, 0x8FBE84BE, 0x06FB2900, CAM_CONTRAST)
CAMERA_PROPERTY(Sharpness, 0xAA72C3DC, 0xD1A5BB0B, CAM_SHARPNESS)
CAMERA_PROPERTY(Reverse, 0x44F6043F, 0x1175F477, CAM_REVERSE)
CAMERA_PROPERTY(Effect, 0x7E8EF3B2, 0xE9D2CFB1, CAM_EFFECT)
CAMERA_PROPERTY(EV, 0x8B5E6147, 0x62AFF0B8, CAM_EV)
CAMERA_PROPERTY(Zoom, 0x06D3816C, 0xF7464216, CAM_ZOOM)
CAMERA_PROPERTY(AntiFlicker, 0x9FDACB99, 0xE312958A, CAM_ANTIFLICKER)
CAMERA_PROPERTY(ISO, 0x4EBD5C68, 0x3CF630A1, CAM_ISO)
CAMERA_PROPERTY(Gain, 0x2C36D6F3, 0xE65CFE86, CAM_GAIN)
CAMERA_PROPERTY(WhiteBalance, 0xDBFFA1DA, 0x4D4514AC, CAM_WHITE_BALANCE)
CAMERA_PROPERTY(Backlight, 0x8DD1292B, 0xAE071044, CAM_BACKLIGHT)
CAMERA_PROPERTY(Nightmode, 0x12B6FF26, 0x3F26233E, CAM_NIGHTMODE)
CAMERA_PROPERTY(ExposureCeiling, 0x5FA5B1BB, 0x04F34BEE, CAM_EXPOSURE_CEILING)
CAMERA_PROPERTY(AutoControlHold, 0x06A21BBB, 0x3A0DABBD, CAM_AUTO_CONTROL_HOLD)
CAMERA_PROPERTY(ImageQuality, 0xE2AC7BCE, 0x75C4300B, CAM_IMAGE_QUALITY)
CAMERA_PROPERTY(NoiseReduction, 0xFEB99ACC, 0xF9B79556, CAM_NOISE_REDUCTION)
CAMERA_PROPERTY(SharpnessOff, 0x34CCAF85, 0x4B5405C8, CAM_SHARPNESS_OFF)

DECLARE_LIBRARY(SceCamera, 0xDA91B3ED);
