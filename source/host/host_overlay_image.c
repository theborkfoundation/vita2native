#include "host_overlay_image.h"
#include "v2n_overlay_png.h"

#if V2N_HAS_OVERLAY_PNG
#include <limits.h>
#include <stdio.h>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

unsigned char *host_overlay_image_load(int *width, int *height)
{
    unsigned char *pixels;
    if (sizeof(v2n_overlay_png) > INT_MAX) {
        fprintf(stderr, "vita2native: embedded overlay PNG is too large\n");
        return NULL;
    }
    pixels = stbi_load_from_memory(v2n_overlay_png, (int)sizeof(v2n_overlay_png), width, height,
                                   NULL, 4);
    if (!pixels)
        fprintf(stderr, "vita2native: cannot decode embedded overlay PNG: %s\n",
                stbi_failure_reason());
    return pixels;
}

void host_overlay_image_free(unsigned char *pixels)
{
    stbi_image_free(pixels);
}
#else
unsigned char *host_overlay_image_load(int *width, int *height)
{
    (void)width;
    (void)height;
    return 0;
}

void host_overlay_image_free(unsigned char *pixels)
{
    (void)pixels;
}
#endif
