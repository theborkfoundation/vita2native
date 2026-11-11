#ifndef VITA2NATIVE_DISPLAY_H
#define VITA2NATIVE_DISPLAY_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

/*
 * True when the raw KMS/DRM present path is wanted.
 *
 * It is opt-in. On several vendor kernels (the R36S VOP, for one)
 * drmModePageFlip reports success while the controller keeps scanning the
 * buffer from drmModeSetCrtc, so the panel shows the first frame forever.
 * SDL's own KMSDRM driver drives that panel correctly, so presentation goes
 * through SDL even when SDL selected the KMSDRM video driver.
 */
static inline bool host_display_uses_kms(void)
{
    static int cached = -1;

    if (cached >= 0)
        return cached != 0;

    cached = vita2native_env_flag("VITA2NATIVE_KMS_PRESENT") ? 1 : 0;

    return cached != 0;
}

#endif /* VITA2NATIVE_DISPLAY_H */
