#ifndef SCE_DISPLAY_H
#define SCE_DISPLAY_H

#include "gfx.h"

int SceDisplay_init(HostGpuDevice device);
int SceDisplay_finish(void);

/* Current logical window size, used to map pointer input onto the touch panel. */
void SceDisplay_get_window_size(int *width, int *height);

#endif
