#ifndef VITA2NATIVE_FRAME_SKIP_H
#define VITA2NATIVE_FRAME_SKIP_H

#include <stdbool.h>

/*
 * Opt-in frame skipping for titles whose clock is their frame counter. When a
 * frame costs more than 1/60 s the guest loop runs slow, and so does the game;
 * dropping the render work of some frames keeps the loop at 60 Hz while fewer
 * frames reach the display.
 *
 *   VITA2NATIVE_FRAME_SKIP=auto   skip only while the loop is behind 60 Hz
 *                        =N       draw one frame in N (2 shows every other)
 *   unset, empty, 0 or 1          off
 *
 * frame_skip_frame_current() answers for the frame in progress: ask it in the
 * render path and again at present, then call frame_skip_frame_end() once per
 * guest frame, at the present, to account for the time that frame took.
 */

/* Force the next frame to draw and present, for a frame that is being captured. */
void frame_skip_hold_next(bool hold);

bool frame_skip_frame_current(void);
void frame_skip_frame_end(bool skipped);
unsigned int frame_skip_take_skipped(void);

#endif
