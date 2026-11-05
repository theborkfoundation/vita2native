#ifndef VITA2NATIVE_COMMON_DIALOG_OVERLAY_H
#define VITA2NATIVE_COMMON_DIALOG_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A snapshot of the active system dialog, drawn by the display backend on top
 * of the guest framebuffer at present time. The guest never sees these
 * structures; the dialog module owns the state.
 */
typedef struct {
    char title[64];
    char message[512];
    char hint[64];
    int progress; /* 0..100, or -1 when there is no progress bar */
} CommonDialogOverlay;

/* Fills `overlay` and returns true while a dialog should be visible. */
bool SceCommonDialog_getOverlay(CommonDialogOverlay *overlay);

/* Called by the display backend after drawing the overlay. */
void SceCommonDialog_overlayPresented(void);

/*
 * Draws `overlay` into an A8B8G8R8 framebuffer. `pitch` is in pixels. The
 * renderer clips to the framebuffer and only touches the panel area.
 */
void CommonDialogOverlay_render(const CommonDialogOverlay *overlay, uint32_t *pixels, int width,
                                int height, int pitch);

#endif /* VITA2NATIVE_COMMON_DIALOG_OVERLAY_H */
