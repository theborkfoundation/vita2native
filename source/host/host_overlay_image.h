#pragma once

/* Decode the build-time PNG to RGBA, or return NULL when none is embedded. */
unsigned char *host_overlay_image_load(int *width, int *height);
void host_overlay_image_free(unsigned char *pixels);
