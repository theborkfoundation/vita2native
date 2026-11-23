/*
 * EGL window presentation test.
 *
 * Creates an X11 window through libX11 (loaded with dlopen so the test links
 * without X11 development headers), builds the production GLES device with a
 * window-capable platform display, attaches the window and presents a
 * checkerboard framebuffer. The window contents are then read back with
 * XGetImage to prove that pixels actually reached the X server.
 *
 * Usage: egl_window_smoke   (skips when DISPLAY is unset or X11 is missing)
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gfx.h"
#include <GLES3/gl3.h>

#define WINDOW_WIDTH  640
#define WINDOW_HEIGHT 480
#define FRAME_WIDTH   120
#define FRAME_HEIGHT  68

static HostWindowInfo test_window;

bool host_video_window_info(HostWindowInfo *info)
{
    *info = test_window;
    return true;
}

typedef struct {
    int width, height;
    int xoffset;
    int format;
    char *data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask, green_mask, blue_mask;
} XImagePrefix;

typedef struct {
    void *lib;
    void *(*open_display)(const char *name);
    int (*close_display)(void *display);
    int (*default_screen)(void *display);
    unsigned long (*root_window)(void *display, int screen);
    unsigned long (*create_simple_window)(void *display, unsigned long parent, int x, int y,
                                          unsigned int width, unsigned int height,
                                          unsigned int border_width, unsigned long border,
                                          unsigned long background);
    int (*map_window)(void *display, unsigned long window);
    int (*flush)(void *display);
    int (*sync)(void *display, int discard);
    XImagePrefix *(*get_image)(void *display, unsigned long drawable, int x, int y,
                               unsigned int width, unsigned int height, unsigned long plane_mask,
                               int format);
    int (*free_memory)(void *data);
    int (*destroy_window)(void *display, unsigned long window);
} X11Api;

static bool x11_load(X11Api *x11)
{
    memset(x11, 0, sizeof(*x11));
    x11->lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!x11->lib)
        return false;

#define LOAD_X11(field, symbol) (*(void **)(&x11->field) = dlsym(x11->lib, "X" #symbol))
    LOAD_X11(open_display, OpenDisplay);
    LOAD_X11(close_display, CloseDisplay);
    LOAD_X11(default_screen, DefaultScreen);
    LOAD_X11(root_window, RootWindow);
    LOAD_X11(create_simple_window, CreateSimpleWindow);
    LOAD_X11(map_window, MapWindow);
    LOAD_X11(flush, Flush);
    LOAD_X11(sync, Sync);
    LOAD_X11(get_image, GetImage);
    LOAD_X11(free_memory, Free);
    LOAD_X11(destroy_window, DestroyWindow);
#undef LOAD_X11

    return x11->open_display && x11->close_display && x11->default_screen &&
           x11->root_window && x11->create_simple_window && x11->map_window && x11->flush &&
           x11->sync && x11->get_image;
}

static unsigned extract_component(unsigned long pixel, unsigned long mask)
{
    unsigned shift = 0;
    unsigned long value;

    if (!mask)
        return 0;

    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }

    value = (pixel >> shift) & mask;
    return (unsigned)(value * 255 / mask);
}

static uint32_t x11_pixel(const XImagePrefix *image, int x, int y)
{
    const unsigned char *row = (const unsigned char *)image->data + (size_t)y * image->bytes_per_line;

    if (image->bits_per_pixel == 32)
        return *(const uint32_t *)(const void *)(row + (size_t)x * 4);
    if (image->bits_per_pixel == 24)
        return (uint32_t)row[x * 3] | ((uint32_t)row[x * 3 + 1] << 8) |
               ((uint32_t)row[x * 3 + 2] << 16);
    return 0;
}

static void put_u16(unsigned char *p, unsigned value)
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

/* Dumps the X11 window contents as a 24-bit BMP for visual inspection. */
static bool write_bmp(const char *path, const XImagePrefix *image, int width, int height)
{
    unsigned char header[54];
    const int row_size = (width * 3 + 3) & ~3;
    unsigned char *row;
    FILE *file;

    file = fopen(path, "wb");
    if (!file)
        return false;

    memset(header, 0, sizeof(header));
    header[0] = 'B';
    header[1] = 'M';
    put_u32(header + 2, 54u + (uint32_t)(row_size * height));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, (uint32_t)width);
    put_u32(header + 22, (uint32_t)height);
    put_u16(header + 26, 1);
    put_u16(header + 28, 24);
    put_u32(header + 34, (uint32_t)(row_size * height));
    fwrite(header, 1, sizeof(header), file);

    row = calloc(1, (size_t)row_size);
    if (!row) {
        fclose(file);
        return false;
    }

    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            uint32_t pixel = x11_pixel(image, x, y);

            row[x * 3 + 0] = (unsigned char)extract_component(pixel, image->blue_mask);
            row[x * 3 + 1] = (unsigned char)extract_component(pixel, image->green_mask);
            row[x * 3 + 2] = (unsigned char)extract_component(pixel, image->red_mask);
        }
        fwrite(row, 1, (size_t)row_size, file);
    }

    free(row);
    fclose(file);
    return true;
}

int main(int argc, char **argv)
{
    const char *dump_path = argc > 1 ? argv[1] : NULL;
    X11Api x11;
    void *display;
    int screen;
    unsigned long window;
    HostGpuConfig config = { WINDOW_WIDTH, WINDOW_HEIGHT, false };
    HostGpuDevice device;
    uint32_t frame[FRAME_WIDTH * FRAME_HEIGHT];
    XImagePrefix *image;
    unsigned red, green, blue;
    int result = 1;

    if (!getenv("DISPLAY")) {
        printf("egl_window_smoke: skipped (no DISPLAY)\n");
        return 0;
    }

    if (!x11_load(&x11) || !(display = x11.open_display(NULL))) {
        printf("egl_window_smoke: skipped (no X11)\n");
        return 0;
    }

    screen = x11.default_screen(display);
    window = x11.create_simple_window(display, x11.root_window(display, screen), 0, 0,
                                      WINDOW_WIDTH, WINDOW_HEIGHT, 0, 0, 0);
    if (!window) {
        fprintf(stderr, "egl_window_smoke: XCreateSimpleWindow failed\n");
        goto done;
    }

    x11.map_window(display, window);
    x11.flush(display);
    x11.sync(display, 0);

    test_window = (HostWindowInfo){.type = HOST_WINDOW_X11, .display = display,
                                   .window = (void *)(uintptr_t)window,
                                   .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT};
    device = host_gpu_create(&config);
    if (!device) {
        fprintf(stderr, "egl_window_smoke: no GPU device\n");
        goto done;
    }

    printf("GPU: %s\n", host_gpu_description(device));
    if (host_gpu_backend(device) != HOST_GPU_BACKEND_GLES) {
        printf("egl_window_smoke: skipped (no GLES backend)\n");
        host_gpu_destroy(device);
        result = 0;
        goto done;
    }

    if (host_gpu_attach_window(device, &test_window) != 0) {
        fprintf(stderr, "egl_window_smoke: host_gpu_attach_window failed\n");
        host_gpu_destroy(device);
        goto done;
    }

    /* Red/blue checkerboard: any letterbox-scaled sample is one of the two. */
    for (int y = 0; y < FRAME_HEIGHT; y++) {
        for (int x = 0; x < FRAME_WIDTH; x++) {
            bool red_square = ((x / 16) + (y / 16)) % 2 == 0;

            frame[y * FRAME_WIDTH + x] = red_square ? 0xFF0000FFu : 0xFFFF0000u;
        }
    }

    host_gpu_present_framebuffer(device, frame, FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH);
    x11.sync(display, 0);

    image = x11.get_image(display, window, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, ~0UL, 2);
    if (!image) {
        fprintf(stderr, "egl_window_smoke: XGetImage failed\n");
        host_gpu_destroy(device);
        goto done;
    }

    /* The Vita aspect ratio fits at y=0..362. Samples stay inside its squares. */
    red = extract_component(x11_pixel(image, 40, 42), image->red_mask);
    green = extract_component(x11_pixel(image, 40, 42), image->green_mask);
    blue = extract_component(x11_pixel(image, 40, 42), image->blue_mask);
    if (!(red > 200 && green < 60 && blue < 60)) {
        fprintf(stderr, "egl_window_smoke: red square is %u,%u,%u\n", red, green, blue);
        x11.free_memory(image);
        host_gpu_destroy(device);
        goto done;
    }
    red = extract_component(x11_pixel(image, 125, 42), image->red_mask);
    green = extract_component(x11_pixel(image, 125, 42), image->green_mask);
    blue = extract_component(x11_pixel(image, 125, 42), image->blue_mask);
    if (!(blue > 200 && red < 60 && green < 60)) {
        fprintf(stderr, "egl_window_smoke: blue square is %u,%u,%u\n", red, green, blue);
        x11.free_memory(image);
        host_gpu_destroy(device);
        goto done;
    }

    if (dump_path && write_bmp(dump_path, image, WINDOW_WIDTH, WINDOW_HEIGHT))
        printf("wrote %s\n", dump_path);

    /* The frame touches the top edge and the last row above the border. */
    for (int y = 0; y <= 362; y += 362) {
        red = extract_component(x11_pixel(image, 40, y), image->red_mask);
        blue = extract_component(x11_pixel(image, 40, y), image->blue_mask);
        if (red < 100 && blue < 100) {
            fprintf(stderr, "egl_window_smoke: guest frame missing at y=%d\n", y);
            x11.free_memory(image);
            host_gpu_destroy(device);
            goto done;
        }
    }

    /* The unused space begins immediately below the game and is textured. */
    for (int y = 363; y <= 460; y += 97) {
        red = extract_component(x11_pixel(image, 320, y), image->red_mask);
        green = extract_component(x11_pixel(image, 320, y), image->green_mask);
        blue = extract_component(x11_pixel(image, 320, y), image->blue_mask);
        if (!(red > 0 && green > red && blue > green)) {
            fprintf(stderr, "egl_window_smoke: border at y=%d is %u,%u,%u\n", y, red, green, blue);
            x11.free_memory(image);
            host_gpu_destroy(device);
            goto done;
        }
    }
    x11.free_memory(image);

    /* The GPU-direct path must preserve the same borders and the guest image. */
    {
        void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
        PFNGLGENTEXTURESPROC gen = gles ? (PFNGLGENTEXTURESPROC)dlsym(gles, "glGenTextures") : NULL;
        PFNGLBINDTEXTUREPROC bind = gles ? (PFNGLBINDTEXTUREPROC)dlsym(gles, "glBindTexture") : NULL;
        PFNGLTEXIMAGE2DPROC upload = gles ? (PFNGLTEXIMAGE2DPROC)dlsym(gles, "glTexImage2D") : NULL;
        PFNGLTEXPARAMETERIPROC param = gles ? (PFNGLTEXPARAMETERIPROC)dlsym(gles, "glTexParameteri") : NULL;
        PFNGLDELETETEXTURESPROC del = gles ? (PFNGLDELETETEXTURESPROC)dlsym(gles, "glDeleteTextures") : NULL;
        GLuint texture = 0;
        uint32_t debug_overlay[16];
        if (!gen || !bind || !upload || !param || !del) {
            fprintf(stderr, "egl_window_smoke: GLES texture functions unavailable\n");
            if (gles) dlclose(gles);
            host_gpu_destroy(device);
            goto done;
        }
        for (size_t i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; ++i)
            frame[i] = 0xFF00FF00u;
        gen(1, &texture);
        bind(GL_TEXTURE_2D, texture);
        param(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        param(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        upload(GL_TEXTURE_2D, 0, GL_RGBA8, FRAME_WIDTH, FRAME_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame);
        for (size_t i = 0; i < 16; ++i)
            debug_overlay[i] = 0xFFFFFFFFu;
        host_gpu_set_overlay(device, debug_overlay, 4, 4);
        host_gpu_present_texture(device, texture, FRAME_WIDTH, FRAME_HEIGHT);
        x11.sync(display, 0);
        image = x11.get_image(display, window, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, ~0UL, 2);
        if (!image) {
            del(1, &texture);
            dlclose(gles);
            host_gpu_destroy(device);
            goto done;
        }
        red = extract_component(x11_pixel(image, 320, 240), image->red_mask);
        green = extract_component(x11_pixel(image, 320, 240), image->green_mask);
        blue = extract_component(x11_pixel(image, 320, 240), image->blue_mask);
        if (red != 0 || green != 255 || blue != 0) {
            fprintf(stderr, "egl_window_smoke: direct frame is %u,%u,%u\n", red, green, blue);
            x11.free_memory(image);
            del(1, &texture);
            dlclose(gles);
            host_gpu_destroy(device);
            goto done;
        }
        red = extract_component(x11_pixel(image, 586, 54), image->red_mask);
        green = extract_component(x11_pixel(image, 586, 54), image->green_mask);
        blue = extract_component(x11_pixel(image, 586, 54), image->blue_mask);
        if (red != 255 || green != 255 || blue != 255) {
            fprintf(stderr, "egl_window_smoke: debug overlay is %u,%u,%u\n", red, green, blue);
            x11.free_memory(image);
            del(1, &texture);
            dlclose(gles);
            host_gpu_destroy(device);
            goto done;
        }
        for (int y = 0; y <= 362; y += 362) {
            red = extract_component(x11_pixel(image, 320, y), image->red_mask);
            green = extract_component(x11_pixel(image, 320, y), image->green_mask);
            blue = extract_component(x11_pixel(image, 320, y), image->blue_mask);
            if (red != 0 || green != 255 || blue != 0) {
                fprintf(stderr, "egl_window_smoke: direct frame missing at y=%d\n", y);
                x11.free_memory(image);
                del(1, &texture);
                dlclose(gles);
                host_gpu_destroy(device);
                goto done;
            }
        }
        for (int y = 363; y <= 460; y += 97) {
            red = extract_component(x11_pixel(image, 320, y), image->red_mask);
            green = extract_component(x11_pixel(image, 320, y), image->green_mask);
            blue = extract_component(x11_pixel(image, 320, y), image->blue_mask);
            if (!(red > 0 && green > red && blue > green)) {
                fprintf(stderr, "egl_window_smoke: direct border at y=%d is %u,%u,%u\n", y, red, green, blue);
                x11.free_memory(image);
                del(1, &texture);
                dlclose(gles);
                host_gpu_destroy(device);
                goto done;
            }
        }
        x11.free_memory(image);
        del(1, &texture);
        dlclose(gles);
    }

    /* Presenting NULL clears the window to black. */
    host_gpu_present_framebuffer(device, NULL, 0, 0, 0);
    x11.sync(display, 0);
    image = x11.get_image(display, window, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, 1, 1, ~0UL, 2);
    if (image) {
        red = extract_component(x11_pixel(image, 0, 0), image->red_mask);
        green = extract_component(x11_pixel(image, 0, 0), image->green_mask);
        blue = extract_component(x11_pixel(image, 0, 0), image->blue_mask);
        x11.free_memory(image);
        if (red || green || blue) {
            fprintf(stderr, "egl_window_smoke: black frame is %u,%u,%u\n", red, green, blue);
            host_gpu_destroy(device);
            goto done;
        }
    }

    printf("egl_window_smoke: ok\n");
    result = 0;

    host_gpu_detach_window(device);
    host_gpu_destroy(device);

done:
    if (x11.destroy_window)
        x11.destroy_window(display, window);
    x11.close_display(display);
    if (x11.lib)
        dlclose(x11.lib);
    return result;
}
