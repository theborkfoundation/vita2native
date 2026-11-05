#include <psp2/display.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceDisplay.h"
#include "util.h"

static SceDisplayFrameBuf g_vita_conf_fb;
static int g_vcount;
static unsigned int g_frame_count;

/*
 * Headless frame capture. Set VITA2NATIVE_DUMP_FRAME to a path, optionally
 * VITA2NATIVE_DUMP_FRAME_AT to the (zero-based) swap index to capture, to get
 * a BMP of the guest framebuffer. This is how real homebrew output is checked
 * without a window.
 */
static void put_u16(unsigned char *p, unsigned int value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void dump_framebuffer(const SceDisplayFrameBuf *fb)
{
    static const char *path;
    static long capture_at = -1;
    static bool initialised;
    const uint8_t *base;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int row_size;
    unsigned char header[54];
    unsigned char *row;
    FILE *file;

    if (!initialised) {
        const char *at = getenv("VITA2NATIVE_DUMP_FRAME_AT");

        initialised = true;
        path = getenv("VITA2NATIVE_DUMP_FRAME");
        if (at)
            capture_at = strtol(at, NULL, 0);
    }

    if (!path || !fb || !fb->base)
        return;
    if (capture_at >= 0 && (long)g_frame_count != capture_at)
        return;

    width  = fb->width;
    height = fb->height;
    pitch  = fb->pitch;
    if (width == 0 || height == 0 || pitch < width)
        return;

    file = fopen(path, "wb");
    if (!file)
        return;

    base = fb->base;
    row_size = (width * 3 + 3) & ~3u;
    row = calloc(1, row_size);
    if (!row) {
        fclose(file);
        return;
    }

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, 54u + (uint32_t)(row_size * height));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, width);
    put_u32(header + 22, height);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, (uint32_t)(row_size * height));
    fwrite(header, 1, sizeof(header), file);

    for (unsigned int y = height; y-- > 0;) {
        const uint32_t *src = (const uint32_t *)(base + (size_t)y * pitch * 4);

        for (unsigned int x = 0; x < width; x++) {
            uint32_t pixel = src[x];

            /* A8B8G8R8, the only format the guest can present. */
            row[x * 3 + 0] = (unsigned char)(pixel & 0xFF);
            row[x * 3 + 1] = (unsigned char)((pixel >> 8) & 0xFF);
            row[x * 3 + 2] = (unsigned char)((pixel >> 16) & 0xFF);
        }
        fwrite(row, 1, row_size, file);
    }

    free(row);
    fclose(file);
    LOG("SceDisplay: captured frame %u to %s", g_frame_count, path);
}

EXPORT(SceDisplayUser, 0x7A410B64, int, sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    (void)sync;

    if (pParam)
        g_vita_conf_fb = *pParam;

    if (pParam)
        dump_framebuffer(pParam);
    g_frame_count++;

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
    host_sleep_ns(16666667ull);
    __sync_fetch_and_add(&g_vcount, 1);
    return 0;
}

EXPORT(SceDisplay, 0xF51523CB, int, _sceDisplaySetFrameBuf, const SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0xA753B0CA, int, _sceDisplayGetFrameBuf, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x7178FADA, int, sceDisplayGetPrimaryHead, void)
{
    return 0;
}

EXPORT(SceDisplay, 0xA08CA60D, int, sceDisplayGetRefreshRate, float *pFps)
{
    if (!pFps)
        return -1;
    *pFps = 60.0f;
    return 0;
}

EXPORT(SceDisplay, 0x2EBFC7CB, int, sceDisplayGetMaximumFrameBufResolution, int *width,
       int *height)
{
    if (width)
        *width = 960;
    if (height)
        *height = 544;
    return 0;
}

EXPORT(SceDisplay, 0xB6FDE0BA, int, sceDisplayGetVcount, void)
{
    return __sync_fetch_and_add(&g_vcount, 0);
}

static int display_wait_vblanks(unsigned int count)
{
    while (count--) {
        host_sleep_ns(16666667ull);
        __sync_fetch_and_add(&g_vcount, 1);
    }
    return 0;
}

EXPORT(SceDisplay, 0x78B41B92, int, sceDisplayWaitVblankStartCB, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0xDD0A13B8, int, sceDisplayWaitVblankStartMulti, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x05F27764, int, sceDisplayWaitVblankStartMultiCB, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x9423560C, int, sceDisplayWaitSetFrameBuf, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0x814C90AF, int, sceDisplayWaitSetFrameBufCB, void)
{
    return display_wait_vblanks(1);
}

EXPORT(SceDisplay, 0x7D9864A8, int, sceDisplayWaitSetFrameBufMulti, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x3E796EF5, int, sceDisplayWaitSetFrameBufMultiCB, unsigned int vcount)
{
    return display_wait_vblanks(vcount);
}

EXPORT(SceDisplay, 0x6BDF4C4D, int, sceDisplayRegisterVblankStartCallback, SceUID uid)
{
    (void)uid;
    return 0;
}

EXPORT(SceDisplay, 0x98436A80, int, sceDisplayUnregisterVblankStartCallback, SceUID uid)
{
    (void)uid;
    return 0;
}

EXPORT(SceDisplay, 0x9686859E, int, sceDisplayGetVcountInternal, int display)
{
    (void)display;

    return sceDisplayGetVcount();
}

/* Internal/compat aliases used by the shell and media applications. */
EXPORT(SceDisplayUser, 0x89F86A1E, int, sceDisplaySetFrameBufInternal, int buffer_index, int unk,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    (void)unk;
    if (buffer_index != 0)
        return 0;
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x7A8CB78E, int, _sceDisplaySetFrameBufInternal, int buffer_index, int unk,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    (void)unk;
    if (buffer_index != 0)
        return 0;
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplayUser, 0x8C36B628, int, sceDisplaySetFrameBufForCompat,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x45BCB941, int, _sceDisplaySetFrameBufForCompat,
       const SceDisplayFrameBuf *pParam, SceDisplaySetBufSync sync)
{
    return sceDisplaySetFrameBuf(pParam, sync);
}

EXPORT(SceDisplayUser, 0x5213F74B, int, sceDisplayGetFrameBufInternal, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

EXPORT(SceDisplay, 0x86A8E436, int, _sceDisplayGetFrameBufInternal, SceDisplayFrameBuf *pParam,
       SceDisplaySetBufSync sync)
{
    return sceDisplayGetFrameBuf(pParam, sync);
}

static int display_get_maximum_resolution(int *width, int *height)
{
    return sceDisplayGetMaximumFrameBufResolution(width, height);
}

EXPORT_FUNC(SceDisplayUser, 0x8848ADDD, display_get_maximum_resolution)

EXPORT(SceDisplayUser, 0x0BFB78C6, int, sceDisplayGetResolutionInfoInternal, void *info)
{
    (void)info;
    return 0;
}

EXPORT(SceDisplay, 0xFEFEB240, int, _sceDisplayGetResolutionInfoInternal, void *info)
{
    (void)info;
    return 0;
}

DECLARE_LIBRARY(SceDisplay, 0x5ed8f994);
DECLARE_LIBRARY(SceDisplayUser, 0x4faacd11);

void SceDisplay_get_window_size(int *width, int *height)
{
    if (width)
        *width = 960;
    if (height)
        *height = 544;
}

int SceDisplay_init(HostGpuDevice device)
{
    (void)device;
    return 0;
}

int SceDisplay_finish(void)
{
    return 0;
}
