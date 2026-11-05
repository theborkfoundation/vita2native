/*
 * End-to-end display presentation test.
 *
 * Builds the production GLES device in window mode, lets SceDisplay create its
 * SDL/X11 window, attaches it to EGL, presents guest framebuffers through
 * sceDisplaySetFrameBuf and reads the window back with XGetImage.
 *
 * Usage: display_egl_smoke   (skips without DISPLAY / X11 / a GLES backend)
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/display.h>

#include "gfx.h"
#include "modules/SceDisplay.h"

#define WIDTH  960
#define HEIGHT 544

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
    void *display;
    void *(*open_display)(const char *name);
    int (*close_display)(void *display);
    int (*default_screen)(void *display);
    unsigned long (*root_window)(void *display, int screen);
    int (*query_tree)(void *display, unsigned long window, unsigned long *root, unsigned long *parent,
                      unsigned long **children, unsigned int *count);
    int (*fetch_name)(void *display, unsigned long window, char **name);
    int (*sync)(void *display, int discard);
    XImagePrefix *(*get_image)(void *display, unsigned long drawable, int x, int y,
                               unsigned int width, unsigned int height, unsigned long plane_mask,
                               int format);
    int (*free_memory)(void *data);
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
    LOAD_X11(query_tree, QueryTree);
    LOAD_X11(fetch_name, FetchName);
    LOAD_X11(sync, Sync);
    LOAD_X11(get_image, GetImage);
    LOAD_X11(free_memory, Free);
#undef LOAD_X11

    if (!x11->open_display || !x11->default_screen || !x11->root_window || !x11->query_tree ||
        !x11->fetch_name || !x11->sync || !x11->get_image)
        return false;

    x11->display = x11->open_display(NULL);
    return x11->display != NULL;
}

static unsigned long find_window_named(X11Api *x11, unsigned long parent, const char *name, int depth)
{
    unsigned long root = 0, parent_return = 0;
    unsigned long *children = NULL;
    unsigned int count = 0;
    unsigned long found = 0;

    if (depth > 2 || !x11->query_tree(x11->display, parent, &root, &parent_return, &children, &count))
        return 0;

    for (unsigned int i = 0; i < count && !found; i++) {
        char *window_name = NULL;

        if (x11->fetch_name(x11->display, children[i], &window_name) && window_name) {
            if (strcmp(window_name, name) == 0)
                found = children[i];
            if (x11->free_memory)
                x11->free_memory(window_name);
        }

        if (!found)
            found = find_window_named(x11, children[i], name, depth + 1);
    }

    if (children && x11->free_memory)
        x11->free_memory(children);

    return found;
}

static unsigned extract_component(unsigned long pixel, unsigned long mask)
{
    unsigned shift = 0;

    if (!mask)
        return 0;

    while (!(mask & 1)) {
        mask >>= 1;
        shift++;
    }

    return (unsigned)(((pixel >> shift) & mask) * 255 / mask);
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

static bool window_center_is(X11Api *x11, unsigned long window, unsigned r, unsigned g, unsigned b)
{
    XImagePrefix *image;
    unsigned unsigned_r, unsigned_g, unsigned_b;
    bool match;

    x11->sync(x11->display, 0);
    image = x11->get_image(x11->display, window, WIDTH / 2, HEIGHT / 2, 1, 1, ~0UL, 2);
    if (!image)
        return false;

    unsigned_r = extract_component(x11_pixel(image, 0, 0), image->red_mask);
    unsigned_g = extract_component(x11_pixel(image, 0, 0), image->green_mask);
    unsigned_b = extract_component(x11_pixel(image, 0, 0), image->blue_mask);
    x11->free_memory(image);

    printf("  centre pixel: %u,%u,%u\n", unsigned_r, unsigned_g, unsigned_b);
    match = unsigned_r == r && unsigned_g == g && unsigned_b == b;

    return match;
}

int main(void)
{
    X11Api x11;
    HostGpuConfig config = { WIDTH, HEIGHT, false };
    HostGpuDevice device;
    SceDisplayFrameBuf frame;
    uint32_t *pixels;
    unsigned long window;
    int result = 1;

    if (!getenv("DISPLAY")) {
        printf("display_egl_smoke: skipped (no DISPLAY)\n");
        return 0;
    }

    device = host_gpu_create(&config);
    if (!device) {
        fprintf(stderr, "display_egl_smoke: no GPU device\n");
        return 1;
    }

    printf("GPU: %s\n", host_gpu_description(device));
    if (host_gpu_backend(device) != HOST_GPU_BACKEND_GLES) {
        printf("display_egl_smoke: skipped (no GLES backend)\n");
        host_gpu_destroy(device);
        return 0;
    }

    if (SceDisplay_init(device) != 0) {
        fprintf(stderr, "display_egl_smoke: SceDisplay_init failed\n");
        host_gpu_destroy(device);
        return 1;
    }

    if (!x11_load(&x11)) {
        printf("display_egl_smoke: skipped (no X11)\n");
        SceDisplay_finish();
        host_gpu_destroy(device);
        return 0;
    }

    window = find_window_named(&x11, x11.root_window(x11.display, x11.default_screen(x11.display)),
                               "vita2native", 0);
    if (!window) {
        fprintf(stderr, "display_egl_smoke: could not find the vita2native window\n");
        goto done;
    }

    pixels = malloc((size_t)WIDTH * HEIGHT * 4);
    if (!pixels) {
        fprintf(stderr, "display_egl_smoke: out of memory\n");
        goto done;
    }

    memset(&frame, 0, sizeof(frame));
    frame.size = sizeof(frame);
    frame.base = pixels;
    frame.pitch = WIDTH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = WIDTH;
    frame.height = HEIGHT;

    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++)
        pixels[i] = 0xFF0000FFu; /* opaque red, bytes R=255 G=0 B=0 A=255 */

    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_IMMEDIATE);
    sceDisplayWaitVblankStartMulti(2);
    if (!window_center_is(&x11, window, 255, 0, 0)) {
        fprintf(stderr, "display_egl_smoke: red frame was not presented\n");
        free(pixels);
        goto done;
    }

    for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; i++)
        pixels[i] = 0xFFFF0000u; /* opaque blue */

    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_IMMEDIATE);
    sceDisplayWaitVblankStartMulti(2);
    if (!window_center_is(&x11, window, 0, 0, 255)) {
        fprintf(stderr, "display_egl_smoke: blue frame was not presented\n");
        free(pixels);
        goto done;
    }

    free(pixels);
    printf("display_egl_smoke: ok\n");
    result = 0;

done:
    SceDisplay_finish();
    host_gpu_destroy(device);
    if (x11.close_display)
        x11.close_display(x11.display);
    if (x11.lib)
        dlclose(x11.lib);
    return result;
}
