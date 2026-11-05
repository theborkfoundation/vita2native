#ifndef SCE_DISPLAY_H
#define SCE_DISPLAY_H

#include "gfx.h"

int SceDisplay_init(HostGpuDevice device);
int SceDisplay_finish(void);

/*
 * Number of frames presented so far. Headless scripted input is keyed to this
 * counter, so input timing does not depend on host wall-clock speed (a single
 * software-rendered frame can take seconds under qemu).
 */
unsigned int SceDisplay_get_frame_count(void);

/* Current logical window size, used to map pointer input onto the touch panel. */
void SceDisplay_get_window_size(int *width, int *height);

#endif
