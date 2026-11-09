#ifndef VITA2NATIVE_FPS_OVERLAY_H
#define VITA2NATIVE_FPS_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Enabled with VITA2NATIVE_FPS_OVERLAY. */
bool fps_overlay_enabled(void);

/* Adds one sample; the display backend calls this once per second. */
void fps_overlay_add_sample(double fps);

/*
 * Draws the debug pane into an A8B8G8R8 framebuffer. `pitch` is in pixels.
 * The renderer clips to the framebuffer and only touches the pane area.
 */
void fps_overlay_render(uint32_t *pixels, int width, int height, int pitch);

/*
 * Returns the pane rendered into its own RGBA image (top-down, opaque), for
 * backends that present the game through the GPU and cannot compose into the
 * guest framebuffer. Returns NULL when the overlay is disabled.
 */
const uint32_t *fps_overlay_image(int *width, int *height);

#endif
