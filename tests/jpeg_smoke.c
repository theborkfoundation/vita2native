/*
 * JPEG decoder/encoder test.
 *
 * Generates a JPEG with the vendored stb_image_write, then exercises the
 * SceJpeg output-info, RGBA/YCbCr decode, CSC and encoder entry points.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/jpeg.h>
#include <psp2/jpegarm.h>
#include <psp2/jpegenc.h>

#include "stb_image_write.h"

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);

#define SCE_JPEG_PIXEL_RGBA8888 0

int sceJpegDecodeMJpeg(const uint8_t *pJpeg, SceSize isize, uint8_t *pRGBA, SceSize osize,
                       int decodeMode, void *pTempBuffer, SceSize tempBufferSize,
                       void *pCoefBuffer, SceSize coefBufferSize);

static int failures;
static uint8_t g_jpeg[200000];
static size_t g_jpeg_size;

#define CHECK(condition, ...)                                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void write_memory(void *context, void *data, int size)
{
    (void)context;
    if (size <= 0 || g_jpeg_size + (size_t)size > sizeof(g_jpeg))
        return;
    memcpy(g_jpeg + g_jpeg_size, data, (size_t)size);
    g_jpeg_size += (size_t)size;
}

static int check_export(uint32_t lib_nid, uint32_t func_nid, const char *what)
{
    if (module_get_func_export(lib_nid, func_nid, NULL))
        return 0;
    fprintf(stderr, "%s (0x%08x/0x%08x) is not in the export table\n", what, lib_nid, func_nid);
    return 1;
}

static void make_test_jpeg(void)
{
    uint8_t rgb[64 * 64 * 3];

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t *p = rgb + ((size_t)y * 64 + x) * 3;

            p[0] = (uint8_t)(100 + x);
            p[1] = (uint8_t)(40 + y);
            p[2] = 30;
        }
    }

    g_jpeg_size = 0;
    stbi_write_jpg_to_func(write_memory, NULL, 64, 64, 3, rgb, 95);
}

int main(void)
{
    SceJpegOutputInfo info;
    uint8_t *rgba;
    uint8_t *yuv;
    uint8_t argb[64 * 64 * 4];
    uint8_t encoded[100000];
    int context_size;
    void *encoder;
    int result;

    failures += check_export(0x880BF710, 0x6215B095, "sceJpegDecodeMJpeg");
    failures += check_export(0x880BF710, 0x353BA9B0, "sceJpegGetOutputInfo");
    failures += check_export(0x3748BFD3, 0xC60DE94C, "sceJpegEncoderEncode");
    failures += check_export(0x19F04693, 0xA4ABFCE3, "sceJpegArmDecodeMJpeg");
    failures += check_export(0xCF9C23A2, 0x648FFAA3, "sceJpegArmEncoderEncode");

    make_test_jpeg();
    CHECK(g_jpeg_size > 0 && g_jpeg[0] == 0xFF && g_jpeg[1] == 0xD8, "could not create JPEG");

    CHECK(sceJpegInitMJpeg(1) == 0, "sceJpegInitMJpeg failed");

    memset(&info, 0, sizeof(info));
    CHECK(sceJpegGetOutputInfo(g_jpeg, g_jpeg_size, SCE_JPEG_PIXEL_RGBA8888, 0, &info) == 0,
          "GetOutputInfo failed");
    CHECK(info.width == 64 && info.height == 64, "GetOutputInfo returned %ux%u", info.width,
          info.height);
    CHECK(info.pitch[0].x == 64 * 4 && info.pitch[0].y == 64, "RGBA pitch is %ux%u",
          info.pitch[0].x, info.pitch[0].y);
    CHECK(info.outputSize >= 64 * 64 * 4, "output size is %u", info.outputSize);

    /* Arm's variant shares the same implementation. */
    {
        SceJpegOutputInfo arm_info;

        memset(&arm_info, 0, sizeof(arm_info));
        CHECK(sceJpegArmGetOutputInfo(g_jpeg, g_jpeg_size, 0, SCE_JPEG_PIXEL_RGBA8888, &arm_info) ==
                  0 &&
                  arm_info.width == 64 && arm_info.height == 64,
              "Arm GetOutputInfo failed");
    }

    rgba = malloc(64 * 64 * 4);
    CHECK(rgba != NULL, "out of memory");
    if (rgba) {
        memset(rgba, 0, 64 * 64 * 4);
        result = sceJpegDecodeMJpeg(g_jpeg, g_jpeg_size, rgba, 64 * 64 * 4, 0, NULL, 0, NULL, 0);
        CHECK(result == ((64 << 16) | 64), "DecodeMJpeg returned 0x%08x", result);
        CHECK(rgba[(32 * 64 + 32) * 4 + 0] > 100 && rgba[(32 * 64 + 32) * 4 + 2] < 80,
              "decoded pixel is %u,%u,%u", rgba[(32 * 64 + 32) * 4 + 0],
              rgba[(32 * 64 + 32) * 4 + 1], rgba[(32 * 64 + 32) * 4 + 2]);
        CHECK(rgba[(32 * 64 + 32) * 4 + 3] == 255, "decoded alpha is not 255");
        free(rgba);
    }

    yuv = calloc(64 * 64 * 4, 1);
    CHECK(yuv != NULL, "out of memory");
    if (yuv) {
        result = sceJpegDecodeMJpegYCbCr(g_jpeg, g_jpeg_size, 0, yuv, 64 * 64 * 4, NULL, 0);
        CHECK(result == ((64 << 16) | 64), "DecodeMJpegYCbCr returned 0x%08x", result);
        CHECK(yuv[32 * 64 + 32] != 0, "luma plane is empty");
        free(yuv);
    }

    /* YUV444 CSC: neutral chroma produces a grey pixel. */
    {
        uint8_t yuv444[4 * 4 * 3];
        uint8_t out[4 * 4 * 4];

        memset(yuv444, 128, sizeof(yuv444));
        memset(yuv444, 200, 4 * 4); /* Y plane */
        CHECK(sceJpegMJpegCsc(out, yuv444, sizeof(yuv444), 4, SCE_JPEG_PIXEL_RGBA8888, 0x101) == 0,
              "MJpegCsc failed");
        CHECK(out[(2 * 4 + 2) * 4 + 0] == 200 && out[(2 * 4 + 2) * 4 + 1] == 200 &&
                  out[(2 * 4 + 2) * 4 + 2] == 200,
              "CSC produced %u,%u,%u", out[(2 * 4 + 2) * 4 + 0], out[(2 * 4 + 2) * 4 + 1],
              out[(2 * 4 + 2) * 4 + 2]);
    }

    /* Encoder round trip. */
    context_size = sceJpegEncoderGetContextSize();
    CHECK(context_size > 0, "encoder context size is %d", context_size);
    encoder = calloc(1, (size_t)context_size);
    CHECK(encoder != NULL, "out of memory");
    if (encoder) {
        for (int i = 0; i < 64 * 64; i++) {
            argb[i * 4 + 0] = 30;  /* B */
            argb[i * 4 + 1] = 20;  /* G */
            argb[i * 4 + 2] = 10;  /* R */
            argb[i * 4 + 3] = 255; /* A */
        }

        memset(encoded, 0, sizeof(encoded));
        CHECK(sceJpegEncoderInit(encoder, 64, 64, SCE_JPEGENC_PIXELFORMAT_ARGB8888, encoded,
                                 sizeof(encoded)) == 0,
              "encoder init failed");
        CHECK(sceJpegEncoderSetCompressionRatio(encoder, 200) == 0, "encoder ratio failed");
        result = sceJpegEncoderEncode(encoder, argb);
        CHECK(result > 0, "encoder returned %d", result);
        CHECK(encoded[0] == 0xFF && encoded[1] == 0xD8, "encoder did not write a JPEG header");

        memset(&info, 0, sizeof(info));
        CHECK(sceJpegGetOutputInfo(encoded, (SceSize)result, SCE_JPEG_PIXEL_RGBA8888, 0, &info) == 0 &&
                  info.width == 64 && info.height == 64,
              "encoded JPEG did not report 64x64");
        CHECK(sceJpegEncoderEnd(encoder) == 0, "encoder end failed");
        free(encoder);
    }

    CHECK(sceJpegFinishMJpeg() == 0, "sceJpegFinishMJpeg failed");

    if (failures) {
        fprintf(stderr, "jpeg_smoke: %d failure(s)\n", failures);
        return 1;
    }

    printf("jpeg_smoke: ok\n");
    return 0;
}
