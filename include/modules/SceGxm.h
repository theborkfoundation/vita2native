#ifndef SCE_GXM_H
#define SCE_GXM_H

#include "gfx.h"

int SceGxm_init(HostGpuDevice device);
int SceGxm_finish(void);

/*
 * Direct-present handoff. When the last sceGxmEndScene rendered `data` into a
 * GL texture that can be presented without a readback, returns true and fills
 * the texture id and its size.
 */
bool SceGxm_get_present_frame(const void *data, unsigned int *texture, int *width, int *height);

#endif
