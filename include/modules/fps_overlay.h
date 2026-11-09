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

#endif
