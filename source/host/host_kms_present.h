#ifndef VITA2NATIVE_HOST_KMS_PRESENT_H
#define VITA2NATIVE_HOST_KMS_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * KMS/DRM scanout for the EGL presenter.
 *
 * SDL owns the DRM node: it opens the card, takes master and leaves the CRTC
 * configured. What SDL cannot give us is a gbm_surface - SDL_SysWMinfo reports
 * only the gbm_device - and an EGL window surface needs one. So EGL owns the
 * surface and this module owns the flip: it creates a gbm_surface on SDL's
 * device, hands it to EGL as the native window, and after each swap turns the
 * front buffer into a framebuffer and page-flips it.
 *
 * It never calls drmSetMaster, never searches for connectors, encoders or
 * CRTCs, and never changes the mode: SDL already did all of that, and taking
 * master here would fight it.
 */

typedef struct HostKmsPresent HostKmsPresent;

/*
 * Builds the scanout surface on `gbm_device` at `width`x`height`. Returns NULL
 * when libdrm/libgbm are missing, the device cannot make a surface, or the
 * kernel rejects the flip setup - in which case the caller keeps rendering
 * offscreen and presenting through the CPU path.
 */
HostKmsPresent *host_kms_present_create(void *gbm_device, int drm_fd, int width, int height);

void host_kms_present_destroy(HostKmsPresent *kms);

/* The gbm_surface, for eglCreateWindowSurface(). NULL once create() failed. */
void *host_kms_present_surface(const HostKmsPresent *kms);

/* True when this object can actually flip. */
bool host_kms_present_active(const HostKmsPresent *kms);

/*
 * Shows the buffer EGL just finished drawing. Waits for the previous flip to
 * retire so a buffer is never recycled while the display still scans it.
 */
int host_kms_present_flip(HostKmsPresent *kms);

/*
 * Waits for an in-flight flip to land, so the buffer it holds can be released.
 * Safe to call when nothing is pending.
 */
void host_kms_present_wait_idle(HostKmsPresent *kms);

#endif
