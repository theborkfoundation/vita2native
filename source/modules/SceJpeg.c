/*
 * SceJpeg: MJPEG decoding and JPEG encoding.
 *
 * Decoding uses the vendored stb_image decoder; the encoder uses
 * stb_image_write. The guest-visible layout follows Vita3K: GetOutputInfo
 * reports the YUV plane pitches, DecodeMJpeg writes RGBA (or the raw planes for
 * the YCbCr variant) and returns the pitched width/height packed into one
 * integer, and the encoder exposes an opaque host context.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/jpeg.h>
#include <psp2/jpegarm.h>
#include <psp2/jpegenc.h>

#include "module.h"
#include "stb_image.h"
#include "stb_image_write.h"

#define SCE_JPEG_ERROR_DECODE_ERROR          0x80650009
#define SCE_JPEG_ERROR_INVALID_POINTER       0x80650010
#define SCE_JPEG_ERROR_UNSUPPORT_IMAGE_SIZE  0x80650020
#define SCE_JPEG_ERROR_INVALID_COLOR_FORMAT  0x80650050

#define SCE_JPEGENC_ERROR_IMAGE_SIZE           ((int)0x80650200)
#define SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER  ((int)0x80650201)
#define SCE_JPEGENC_ERROR_INVALID_POINTER      ((int)0x80650205)

#define SCE_JPEG_COLORSPACE_GRAYSCALE 0x10101
#define SCE_JPEG_COLORSPACE_YUV       0x20000
#define SCE_JPEG_COLORSPACE_YUV444    0x20101
#define SCE_JPEG_COLORSPACE_YUV440    0x20102
#define SCE_JPEG_COLORSPACE_YUV441    0x20104
#define SCE_JPEG_COLORSPACE_YUV422    0x20201
#define SCE_JPEG_COLORSPACE_YUV420    0x20202
#define SCE_JPEG_COLORSPACE_YUV411    0x20401

#define SCE_JPEG_PIXEL_RGBA8888 0
#define SCE_JPEG_PIXEL_BGRA8888 4
#define SCE_JPEG_NO_CSC_OUTPUT  (-1)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t components;
    uint32_t sampling[3];
    int color_space;
} JpegInfo;

typedef struct {
    uint32_t width;
    uint32_t height;
} JpegPlane;

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

/* Reads the SOF marker: dimensions, component count and sampling factors. */
static bool jpeg_parse_header(const uint8_t *data, size_t size, JpegInfo *info)
{
    size_t offset = 2;
    memset(info, 0, sizeof(*info));

    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    while (offset + 4 <= size) {
        uint8_t marker;
        uint16_t length;

        if (data[offset] != 0xFF) {
            offset++;
            continue;
        }
        marker = data[offset + 1];
        offset += 2;

        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;
        if (marker == 0xD9 || marker == 0xDA)
            break;
        if (offset + 2 > size)
            return false;

        length = (uint16_t)((data[offset] << 8) | data[offset + 1]);
        if (length < 2 || offset + length > size)
            return false;

        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
            const uint8_t *p = data + offset + 2;

            if (length < 8)
                return false;
            info->height = (uint32_t)((p[1] << 8) | p[2]);
            info->width = (uint32_t)((p[3] << 8) | p[4]);
            info->components = p[5];
            if (info->components < 1 || info->components > 3)
                return false;
            for (uint32_t c = 0; c < info->components; c++)
                info->sampling[c] = p[6 + c * 3 + 1];

            if (info->components == 1) {
                info->color_space = SCE_JPEG_COLORSPACE_GRAYSCALE;
            } else {
                uint32_t h = info->sampling[0] >> 4;
                uint32_t v = info->sampling[0] & 15;

                if (h == 4 && v == 1)
                    info->color_space = SCE_JPEG_COLORSPACE_YUV411;
                else if (h == 2 && v == 2)
                    info->color_space = SCE_JPEG_COLORSPACE_YUV420;
                else if (h == 2 && v == 1)
                    info->color_space = SCE_JPEG_COLORSPACE_YUV422;
                else if (h == 1 && v == 2)
                    info->color_space = SCE_JPEG_COLORSPACE_YUV440;
                else if (h == 1 && v == 4)
                    info->color_space = SCE_JPEG_COLORSPACE_YUV441;
                else
                    info->color_space = SCE_JPEG_COLORSPACE_YUV444;
            }

            return info->width && info->height;
        }

        offset += length;
    }

    return false;
}

static void jpeg_chroma_layout(int color_space, uint32_t *cw, uint32_t *ch, uint32_t *bw,
                               uint32_t *bh)
{
    *cw = *ch = 1;
    *bw = *bh = 8;

    switch (color_space) {
    case SCE_JPEG_COLORSPACE_YUV411:
        *cw = 4;
        *bw = 32;
        break;
    case SCE_JPEG_COLORSPACE_YUV420:
        *cw = *ch = 2;
        *bw = *bh = 16;
        break;
    case SCE_JPEG_COLORSPACE_YUV422:
        *cw = 2;
        *bw = 16;
        break;
    case SCE_JPEG_COLORSPACE_YUV440:
        *ch = 2;
        *bh = 16;
        break;
    case SCE_JPEG_COLORSPACE_YUV441:
        *ch = 4;
        *bh = 32;
        break;
    default:
        break;
    }
}

static void jpeg_planes(const JpegInfo *info, uint32_t ratio, JpegPlane planes[3])
{
    uint32_t cw, ch, bw, bh;

    jpeg_chroma_layout(info->color_space, &cw, &ch, &bw, &bh);

    planes[0].width = align_up(align_up(info->width, bw) / ratio, bw);
    planes[0].height = align_up(align_up(info->height, bh) / ratio, bh);
    planes[1] = planes[2] = (JpegPlane){ 0, 0 };

    if (info->components > 1) {
        planes[1].width = planes[2].width = planes[0].width / cw;
        planes[1].height = planes[2].height = planes[0].height / ch;
        if (planes[1].width == 0)
            planes[1].width = planes[2].width = 1;
        if (planes[1].height == 0)
            planes[1].height = planes[2].height = 1;
        if (info->color_space == SCE_JPEG_COLORSPACE_YUV420 ||
            info->color_space == SCE_JPEG_COLORSPACE_YUV422)
            planes[1].width = planes[2].width = align_up(planes[1].width, 16);
    }
}

static uint32_t jpeg_downscale_ratio(int mode)
{
    switch (mode & 0x70) {
    case 0x10:
        return 2;
    case 0x20:
        return 4;
    case 0x40:
        return 8;
    default:
        return 1;
    }
}

static uint32_t jpeg_yuv_size(const JpegPlane planes[3])
{
    return planes[0].width * planes[0].height + planes[1].width * planes[1].height +
           planes[2].width * planes[2].height;
}

/*
 * Decodes to tightly packed RGB(A). The caller owns the returned buffer.
 * width/height are the pitched dimensions used by the Vita API.
 */
static uint8_t *jpeg_decode_rgba(const JpegInfo *info, const uint8_t *jpeg, size_t size,
                                 uint32_t ratio, uint32_t *width, uint32_t *height)
{
    int decoded_w = 0, decoded_h = 0;
    uint8_t *decoded = stbi_load_from_memory(jpeg, (int)size, &decoded_w, &decoded_h, NULL, 4);
    JpegPlane planes[3];
    uint32_t out_w, out_h, source_w, source_h;
    uint8_t *output;

    if (!decoded)
        return NULL;

    jpeg_planes(info, ratio, planes);
    out_w = planes[0].width;
    out_h = planes[0].height;
    source_w = (uint32_t)decoded_w;
    source_h = (uint32_t)decoded_h;

    output = calloc((size_t)out_w * out_h, 4);
    if (!output) {
        stbi_image_free(decoded);
        return NULL;
    }

    for (uint32_t y = 0; y < out_h; y++) {
        for (uint32_t x = 0; x < out_w; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            uint32_t sx = x * ratio;
            uint32_t sy = y * ratio;

            for (uint32_t dy = 0; dy < ratio; dy++) {
                for (uint32_t dx = 0; dx < ratio; dx++) {
                    uint32_t px = sx + dx;
                    uint32_t py = sy + dy;
                    const uint8_t *p;

                    if (px >= source_w || py >= source_h)
                        continue;
                    p = decoded + ((size_t)py * source_w + px) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    a += p[3];
                }
            }

            output[((size_t)y * out_w + x) * 4 + 0] = (uint8_t)(r / (ratio * ratio));
            output[((size_t)y * out_w + x) * 4 + 1] = (uint8_t)(g / (ratio * ratio));
            output[((size_t)y * out_w + x) * 4 + 2] = (uint8_t)(b / (ratio * ratio));
            output[((size_t)y * out_w + x) * 4 + 3] = (uint8_t)(a / (ratio * ratio));
        }
    }

    stbi_image_free(decoded);
    *width = out_w;
    *height = out_h;
    return output;
}

static size_t jpeg_write_rgba(const JpegPlane planes[3], const uint8_t *rgba, uint8_t *dest,
                              size_t dest_size)
{
    uint32_t stride = planes[0].width;
    uint32_t height = planes[0].height;
    size_t needed = (size_t)stride * height * 4;

    if (!dest || dest_size < needed)
        return 0;
    memset(dest, 0, needed);
    memcpy(dest, rgba, needed);

    return needed;
}

static uint8_t clamp_u8(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b, uint8_t *y, uint8_t *cb, uint8_t *cr)
{
    *y = clamp_u8((int)(0.299f * r + 0.587f * g + 0.114f * b + 0.5f));
    *cb = clamp_u8((int)(-0.168736f * r - 0.331264f * g + 0.5f * b + 128.5f));
    *cr = clamp_u8((int)(0.5f * r - 0.418688f * g - 0.081312f * b + 128.5f));
}

static void ycbcr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y;
    int d = (int)cb - 128;
    int e = (int)cr - 128;

    *r = clamp_u8((int)(c + 1.402f * e + 0.5f));
    *g = clamp_u8((int)(c - 0.344136f * d - 0.714136f * e + 0.5f));
    *b = clamp_u8((int)(c + 1.772f * d + 0.5f));
}

/* ------------------------------------------------------------- decode API */

EXPORT(SceJpegUser, 0xB030773B, int, sceJpegInitMJpeg, int decoderCount)
{
    (void)decoderCount;
    return 0;
}

EXPORT(SceJpegUser, 0x518163A7, int, sceJpegInitMJpegWithParam,
       const SceJpegMJpegInitParam *params)
{
    if (!params)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    return 0;
}

EXPORT(SceJpegUser, 0x62842598, int, sceJpegFinishMJpeg, void)
{
    return 0;
}

EXPORT(SceJpegUser, 0x353BA9B0, int, sceJpegGetOutputInfo, const uint8_t *jpegData,
       SceSize jpegSize, int format, int mode, SceJpegOutputInfo *output)
{
    JpegInfo info;
    JpegPlane planes[3];
    uint32_t ratio;
    uint32_t total_yuv;

    if (!jpegData || !jpegSize || !output)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    if (format != SCE_JPEG_NO_CSC_OUTPUT && format != SCE_JPEG_PIXEL_RGBA8888 &&
        format != SCE_JPEG_PIXEL_BGRA8888)
        return SCE_JPEG_ERROR_INVALID_COLOR_FORMAT;
    if (!jpeg_parse_header(jpegData, jpegSize, &info))
        return SCE_JPEG_ERROR_DECODE_ERROR;

    ratio = jpeg_downscale_ratio(mode);
    if (info.width < 64 * ratio || info.height < 64 * ratio || info.width > 2032 * ratio ||
        info.height > 1088 * ratio)
        return SCE_JPEG_ERROR_UNSUPPORT_IMAGE_SIZE;

    jpeg_planes(&info, ratio, planes);
    total_yuv = jpeg_yuv_size(planes);

    memset(output, 0, sizeof(*output));
    output->colorSpace = info.color_space;
    output->width = (SceUInt16)planes[0].width;
    output->height = (SceUInt16)planes[0].height;
    output->unk_0xc = 0;
    output->unk_0x10 = 0;

    for (int i = 0; i < 4; i++) {
        output->pitch[i].x = i < 3 ? planes[i].width : 0;
        output->pitch[i].y = i < 3 ? planes[i].height : 0;
    }

    if (format != SCE_JPEG_NO_CSC_OUTPUT) {
        output->pitch[0].x *= 4;
        output->pitch[1] = output->pitch[2] = output->pitch[3] = (SceJpegPitch){ 0, 0 };
        output->unk_0xc = (total_yuv + 255u) & ~255u;
        output->outputSize = (planes[0].width * planes[0].height * 4u + 255u) & ~255u;
    } else {
        output->outputSize = (total_yuv + 255u) & ~255u;
    }

    return 0;
}

EXPORT(SceJpegUser, 0x6215B095, int, sceJpegDecodeMJpeg, const uint8_t *pJpeg, SceSize isize,
       uint8_t *pRGBA, SceSize osize, int decodeMode, void *pTempBuffer, SceSize tempBufferSize,
       void *pCoefBuffer, SceSize coefBufferSize)
{
    JpegInfo info;
    JpegPlane planes[3];
    uint32_t ratio;
    uint32_t width, height;
    uint8_t *rgba;
    size_t written;

    (void)pTempBuffer;
    (void)tempBufferSize;
    (void)pCoefBuffer;
    (void)coefBufferSize;

    if (!pJpeg || !pRGBA || !isize)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    if (!jpeg_parse_header(pJpeg, isize, &info))
        return SCE_JPEG_ERROR_DECODE_ERROR;

    ratio = jpeg_downscale_ratio(decodeMode);
    rgba = jpeg_decode_rgba(&info, pJpeg, isize, ratio, &width, &height);
    if (!rgba)
        return SCE_JPEG_ERROR_DECODE_ERROR;

    jpeg_planes(&info, ratio, planes);
    written = jpeg_write_rgba(planes, rgba, pRGBA, osize);
    free(rgba);

    if (!written)
        return SCE_JPEG_ERROR_DECODE_ERROR;

    return (int)((planes[0].width << 16) | planes[0].height);
}

EXPORT(SceJpegUser, 0x2A769BD8, int, sceJpegDecodeMJpegYCbCr, const SceUInt8 *jpegData,
       SceSize jpegSize, SceInt32 mode, SceUInt8 *output, SceSize outputSize, void *buffer,
       SceSize bufferSize)
{
    JpegInfo info;
    JpegPlane planes[3];
    uint32_t ratio;
    uint32_t width, height;
    uint32_t cw, ch, bw, bh;
    uint8_t *rgba;
    size_t total;

    (void)buffer;
    (void)bufferSize;

    if (!jpegData || !output || !jpegSize)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    if (!jpeg_parse_header(jpegData, jpegSize, &info))
        return SCE_JPEG_ERROR_DECODE_ERROR;

    ratio = jpeg_downscale_ratio(mode);
    rgba = jpeg_decode_rgba(&info, jpegData, jpegSize, ratio, &width, &height);
    if (!rgba)
        return SCE_JPEG_ERROR_DECODE_ERROR;

    jpeg_planes(&info, ratio, planes);
    jpeg_chroma_layout(info.color_space, &cw, &ch, &bw, &bh);
    total = jpeg_yuv_size(planes);
    if (outputSize < total) {
        free(rgba);
        return SCE_JPEG_ERROR_DECODE_ERROR;
    }
    memset(output, 0, total);

    if (info.components == 1) {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *p = rgba + ((size_t)y * width + x) * 4;

                if (x < planes[0].width && y < planes[0].height)
                    output[(size_t)y * planes[0].width + x] = p[1];
            }
        }
    } else {
        uint8_t *y_plane = output;
        uint8_t *cb_plane = output + (size_t)planes[0].width * planes[0].height;
        uint8_t *cr_plane = cb_plane + (size_t)planes[1].width * planes[1].height;

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
                uint8_t yy, cb, cr;

                rgb_to_ycbcr(p[0], p[1], p[2], &yy, &cb, &cr);
                if (x < planes[0].width && y < planes[0].height)
                    y_plane[(size_t)y * planes[0].width + x] = yy;
            }
        }

        for (uint32_t cy = 0; cy < planes[1].height; cy++) {
            uint32_t sy = cy * ch * ratio;

            for (uint32_t cx = 0; cx < planes[1].width; cx++) {
                uint32_t sx = cx * cw * ratio;
                uint32_t rs = 0, gs = 0, bs = 0, count = 0;

                for (uint32_t dy = 0; dy < ch * ratio; dy++) {
                    for (uint32_t dx = 0; dx < cw * ratio; dx++) {
                        uint32_t px = sx + dx;
                        uint32_t py = sy + dy;

                        if (px >= width || py >= height)
                            continue;
                        {
                            const uint8_t *p = rgba + ((size_t)py * width + px) * 4;

                            rs += p[0];
                            gs += p[1];
                            bs += p[2];
                            count++;
                        }
                    }
                }

                if (count) {
                    uint8_t yy, cb, cr;

                    rgb_to_ycbcr((uint8_t)(rs / count), (uint8_t)(gs / count),
                                 (uint8_t)(bs / count), &yy, &cb, &cr);
                    cb_plane[(size_t)cy * planes[1].width + cx] = cb;
                    cr_plane[(size_t)cy * planes[2].width + cx] = cr;
                }
            }
        }
    }

    free(rgba);
    return (int)((planes[0].width << 16) | planes[0].height);
}

static int jpeg_csc_common(uint8_t *pRGBA, const uint8_t *pYCbCr, uint32_t width, uint32_t height,
                           int out_pitch, int format, int sampling)
{
    uint32_t cw, ch, bw, bh;
    int color_space = SCE_JPEG_COLORSPACE_YUV | (sampling & 0xFFFF);
    const uint8_t *y_plane = pYCbCr;
    const uint8_t *cb_plane;
    const uint8_t *cr_plane;
    bool bgra = format == SCE_JPEG_PIXEL_BGRA8888;

    if (!pRGBA || !pYCbCr)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    if (format != SCE_JPEG_PIXEL_RGBA8888 && format != SCE_JPEG_PIXEL_BGRA8888)
        return SCE_JPEG_ERROR_INVALID_COLOR_FORMAT;

    if (color_space == SCE_JPEG_COLORSPACE_GRAYSCALE || color_space == SCE_JPEG_COLORSPACE_YUV444 ||
        color_space == SCE_JPEG_COLORSPACE_YUV) {
        cw = ch = 1;
        bw = bh = 1;
        color_space = SCE_JPEG_COLORSPACE_YUV444;
    } else {
        jpeg_chroma_layout(color_space, &cw, &ch, &bw, &bh);
        (void)bw;
        (void)bh;
    }

    cb_plane = y_plane + (size_t)width * height;
    cr_plane = cb_plane + (size_t)((width + cw - 1) / cw) * ((height + ch - 1) / ch);

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst = pRGBA + (size_t)y * (out_pitch > 0 ? (uint32_t)out_pitch : width) * 4;

        for (uint32_t x = 0; x < width; x++) {
            uint32_t cx = x / cw;
            uint32_t cy = y / ch;
            uint8_t r, g, b;

            ycbcr_to_rgb(y_plane[(size_t)y * width + x],
                         cb_plane[(size_t)cy * ((width + cw - 1) / cw) + cx],
                         cr_plane[(size_t)cy * ((width + cw - 1) / cw) + cx], &r, &g, &b);
            dst[x * 4 + 0] = bgra ? b : r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = bgra ? r : b;
            dst[x * 4 + 3] = 255;
        }
    }

    return 0;
}

EXPORT(SceJpegUser, 0xC2380E3A, int, sceJpegMJpegCsc, uint8_t *pRGBA, const uint8_t *pYCbCr,
       SceSize yuvSize, int imageWidth, int format, int sampling)
{
    uint32_t width = (uint32_t)imageWidth;
    uint32_t height;
    int color_space = SCE_JPEG_COLORSPACE_YUV | (sampling & 0xFFFF);

    if (width == 0)
        return SCE_JPEG_ERROR_INVALID_POINTER;
    if (color_space == SCE_JPEG_COLORSPACE_YUV420 || color_space == SCE_JPEG_COLORSPACE_YUV422 ||
        color_space == SCE_JPEG_COLORSPACE_YUV411)
        height = (uint32_t)yuvSize / (width + width / 2);
    else
        height = (uint32_t)yuvSize / (width * 3);
    if (height == 0)
        return SCE_JPEG_ERROR_INVALID_POINTER;

    return jpeg_csc_common(pRGBA, pYCbCr, width, height, imageWidth, format, sampling);
}

EXPORT(SceJpegUser, 0x6263AEC2, int, sceJpegCsc, uint8_t *pRGBA, const uint8_t *pYCbCr,
       uint32_t xysize, int inFrameWidth, int colorOption, int sampling)
{
    uint32_t width = xysize >> 16;
    uint32_t height = xysize & 0xFFFF;

    (void)inFrameWidth;
    colorOption &= ~0x10; /* BT.601 flag */

    return jpeg_csc_common(pRGBA, pYCbCr, width, height, 0, colorOption, sampling);
}

/* ------------------------------------------------------------- SceJpegArm */

EXPORT(SceJpegArm, 0xA4ABFCE3, int, sceJpegArmDecodeMJpeg, const uint8_t *pJpeg, SceSize isize,
       int decodeMode, void *pRGBA, SceSize osize, void *pCoefBuffer, SceSize coefBufferSize)
{
    return sceJpegDecodeMJpeg(pJpeg, isize, pRGBA, osize, decodeMode, NULL, 0, pCoefBuffer,
                              coefBufferSize);
}

EXPORT(SceJpegArm, 0xE9B1B86F, int, sceJpegArmDecodeMJpegYCbCr, const uint8_t *pJpeg,
       SceSize isize, int decodeMode, uint8_t *pYCbCr, SceSize osize, void *pCoefBuffer,
       SceSize coefBufferSize)
{
    return sceJpegDecodeMJpegYCbCr(pJpeg, isize, decodeMode, pYCbCr, osize, pCoefBuffer,
                                   coefBufferSize);
}

EXPORT(SceJpegArm, 0x23AE3BEA, int, sceJpegArmGetOutputInfo, const uint8_t *pJpeg, SceSize isize,
       int decodeMode, int outputFormat, SceJpegOutputInfo *pOutputInfo)
{
    return sceJpegGetOutputInfo(pJpeg, isize, outputFormat, decodeMode, pOutputInfo);
}

/* --------------------------------------------------------------- encoding */

typedef struct {
    uint32_t magic;
    int width;
    int height;
    int valid_width;
    int valid_height;
    int pixel_format;
    uint8_t *out;
    SceSize out_size;
    int quality;
    int header_mode;
} JpegEncoder;

#define JPEG_ENCODER_MAGIC 0x4A504547u

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t used;
    bool overflow;
} JpegMemoryWriter;

static void jpeg_memory_write(void *context, void *data, int size)
{
    JpegMemoryWriter *writer = context;

    if (size <= 0 || writer->overflow)
        return;
    if (writer->used + (size_t)size > writer->capacity) {
        writer->overflow = true;
        return;
    }
    memcpy(writer->data + writer->used, data, (size_t)size);
    writer->used += (size_t)size;
}

EXPORT(SceJpegEncUser, 0x2B55844D, int, sceJpegEncoderGetContextSize, void)
{
    return (int)sizeof(JpegEncoder);
}

EXPORT(SceJpegEncUser, 0x88DA92B4, int, sceJpegEncoderInit, SceJpegEncoderContext context,
       int inWidth, int inHeight, SceJpegEncoderPixelFormat pixelformat, void *outBuffer,
       SceSize outSize)
{
    JpegEncoder *encoder = context;

    if (!encoder || !outBuffer)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;
    if (inWidth <= 0 || inHeight <= 0)
        return SCE_JPEGENC_ERROR_IMAGE_SIZE;

    memset(encoder, 0, sizeof(*encoder));
    encoder->magic = JPEG_ENCODER_MAGIC;
    encoder->width = inWidth;
    encoder->height = inHeight;
    encoder->valid_width = inWidth;
    encoder->valid_height = inHeight;
    encoder->pixel_format = pixelformat;
    encoder->out = outBuffer;
    encoder->out_size = outSize;
    encoder->quality = 75;
    encoder->header_mode = SCE_JPEGENC_HEADER_MODE_JPEG;

    return 0;
}

EXPORT(SceJpegEncUser, 0x2E8FE45D, int, sceJpegEncoderInitWithParam, SceJpegEncoderContext context,
       const SceJpegEncoderInitParam *initParam)
{
    if (!initParam)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;

    return sceJpegEncoderInit(context, initParam->inWidth, initParam->inHeight,
                              (SceJpegEncoderPixelFormat)initParam->pixelFormat,
                              initParam->outBuffer, initParam->outSize);
}

EXPORT(SceJpegEncUser, 0xC87AA849, int, sceJpegEncoderEnd, SceJpegEncoderContext context)
{
    JpegEncoder *encoder = context;

    if (!encoder)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;
    encoder->magic = 0;

    return 0;
}

EXPORT(SceJpegEncUser, 0xC60DE94C, int, sceJpegEncoderEncode, SceJpegEncoderContext context,
       const void *inBuffer)
{
    JpegEncoder *encoder = context;
    JpegMemoryWriter writer;
    uint8_t *rgb;
    uint32_t w, h;
    const uint8_t *src;

    if (!encoder || encoder->magic != JPEG_ENCODER_MAGIC || !inBuffer)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;

    w = (uint32_t)(encoder->valid_width > 0 ? encoder->valid_width : encoder->width);
    h = (uint32_t)(encoder->valid_height > 0 ? encoder->valid_height : encoder->height);
    if (w > (uint32_t)encoder->width)
        w = (uint32_t)encoder->width;
    if (h > (uint32_t)encoder->height)
        h = (uint32_t)encoder->height;

    rgb = malloc((size_t)w * h * 3);
    if (!rgb)
        return SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER;
    src = inBuffer;

    if (encoder->pixel_format == SCE_JPEGENC_PIXELFORMAT_ARGB8888) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                /* Little-endian ARGB8888 words are stored B, G, R, A. */
                const uint8_t *p = src + ((size_t)y * encoder->width + x) * 4;
                uint8_t *d = rgb + ((size_t)y * w + x) * 3;

                d[0] = p[2];
                d[1] = p[1];
                d[2] = p[0];
            }
        }
    } else {
        /* YCbCr420/422 input: convert to RGB for the encoder. */
        uint32_t cw = encoder->pixel_format == SCE_JPEGENC_PIXELFORMAT_YCBCR420 ? 2 : 1;
        uint32_t ch = encoder->pixel_format == SCE_JPEGENC_PIXELFORMAT_YCBCR420 ? 2 : 1;
        uint32_t cstride = (encoder->width + cw - 1) / cw;
        const uint8_t *y_plane = src;
        const uint8_t *cb_plane = y_plane + (size_t)encoder->width * encoder->height;
        const uint8_t *cr_plane = cb_plane + (size_t)cstride * ((encoder->height + ch - 1) / ch);

        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t r, g, b;

                ycbcr_to_rgb(y_plane[(size_t)y * encoder->width + x],
                             cb_plane[(size_t)(y / ch) * cstride + x / cw],
                             cr_plane[(size_t)(y / ch) * cstride + x / cw], &r, &g, &b);
                rgb[((size_t)y * w + x) * 3 + 0] = r;
                rgb[((size_t)y * w + x) * 3 + 1] = g;
                rgb[((size_t)y * w + x) * 3 + 2] = b;
            }
        }
    }

    writer.data = encoder->out;
    writer.capacity = encoder->out_size;
    writer.used = 0;
    writer.overflow = false;

    stbi_write_jpg_to_func(jpeg_memory_write, &writer, (int)w, (int)h, 3, rgb, encoder->quality);
    free(rgb);

    if (writer.overflow || writer.used == 0)
        return SCE_JPEGENC_ERROR_INSUFFICIENT_BUFFER;

    return (int)writer.used;
}

EXPORT(SceJpegEncUser, 0xB2B828EC, int, sceJpegEncoderSetCompressionRatio,
       SceJpegEncoderContext context, int ratio)
{
    JpegEncoder *encoder = context;

    if (!encoder || encoder->magic != JPEG_ENCODER_MAGIC)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;
    if (ratio < 0 || ratio > 255)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;

    /* Higher Vita ratio means stronger compression, i.e. lower quality. */
    encoder->quality = 100 - ratio * 99 / 255;
    if (encoder->quality < 1)
        encoder->quality = 1;

    return 0;
}

EXPORT(SceJpegEncUser, 0x25D52D97, int, sceJpegEncoderSetOutputAddr,
       SceJpegEncoderContext context, void *outBuffer, SceSize outSize)
{
    JpegEncoder *encoder = context;

    if (!encoder || !outBuffer)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;
    encoder->out = outBuffer;
    encoder->out_size = outSize;

    return 0;
}

EXPORT(SceJpegEncUser, 0x9511F3BC, int, sceJpegEncoderSetValidRegion,
       SceJpegEncoderContext context, int inWidth, int inHeight)
{
    JpegEncoder *encoder = context;

    if (!encoder)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;
    if (inWidth <= 0 || inHeight <= 0 || inWidth > encoder->width || inHeight > encoder->height)
        return SCE_JPEGENC_ERROR_IMAGE_SIZE;

    encoder->valid_width = inWidth;
    encoder->valid_height = inHeight;

    return 0;
}

EXPORT(SceJpegEncUser, 0x2F58B12C, int, sceJpegEncoderSetHeaderMode,
       SceJpegEncoderContext context, int mode)
{
    JpegEncoder *encoder = context;

    if (!encoder)
        return SCE_JPEGENC_ERROR_INVALID_POINTER;

    encoder->header_mode = mode;

    return 0;
}

EXPORT(SceJpegEncUser, 0x824A7D4F, int, sceJpegEncoderCsc, SceJpegEncoderContext context,
       void *outBuffer, const void *inBuffer, int inPitch, SceJpegEncoderPixelFormat inPixelFormat)
{
    (void)context;
    (void)outBuffer;
    (void)inBuffer;
    (void)inPitch;
    (void)inPixelFormat;
    return -1;
}

DECLARE_LIBRARY(SceJpegUser, 0x880BF710);
DECLARE_LIBRARY(SceJpegEncUser, 0x3748BFD3);
DECLARE_LIBRARY(SceJpegArm, 0x19F04693);
