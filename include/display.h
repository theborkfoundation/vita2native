#ifndef VITA2NATIVE_DISPLAY_H
#define VITA2NATIVE_DISPLAY_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * True when the active SDL video driver targets KMS/DRM. On a handheld that
 * means presentation can attach EGL to the scanout plane directly instead of
 * copying the frame through SDL. VITA2NATIVE_KMS_PRESENT forces the KMS path
 * for debugging when SDL is driven through another video driver.
 */
static inline bool host_display_uses_kms(void)
{
    static int cached = -1;
    const char *driver;

    if (cached >= 0)
        return cached != 0;

    cached = 0;
    if (getenv("VITA2NATIVE_KMS_PRESENT")) {
        cached = 1;
    } else if ((driver = getenv("SDL_VIDEODRIVER")) && strstr(driver, "kmsdrm")) {
        cached = 1;
    }

    return cached != 0;
}

#endif /* VITA2NATIVE_DISPLAY_H */
