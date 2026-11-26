#ifndef SCE_GXM_H
#define SCE_GXM_H

#include <stdint.h>

#include "gfx.h"

int SceGxm_init(HostGpuDevice device);
int SceGxm_finish(void);

/*
 * Direct-present handoff. When the last sceGxmEndScene rendered `data` into a
 * GL texture that can be presented without a readback, returns true and fills
 * the texture id and its size.
 */
bool SceGxm_get_present_frame(const void *data, unsigned int *texture, int *width, int *height);

/*
 * Reads the render target named by the last direct present back into `rgba`
 * (R, G, B, A bytes; row 0 is the bottom of the image, which is the order
 * glReadPixels returns). Diagnostics only: it synchronises the pipeline and
 * copies the whole frame, so callers gate it on the frames they want.
 */
bool SceGxm_read_present_frame(uint8_t *rgba, int width, int height);

#endif
