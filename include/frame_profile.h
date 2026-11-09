#ifndef VITA2NATIVE_FRAME_PROFILE_H
#define VITA2NATIVE_FRAME_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Whole-frame timing, enabled with VITA2NATIVE_PROFILE_FRAME=1.
 *
 * VITA2NATIVE_PROFILE covers GXM internals only, which is not enough to tell
 * where a slow title spends its frame: on the handhelds most of it is guest
 * code and vblank waits. These counters split the interval between presented
 * frames into the GXM draw/end work, the presentation, vblank sleeps, and the
 * remainder (guest execution).
 *
 * All calls are cheap no-ops while the environment variable is unset.
 */
bool frame_profile_enabled(void);

uint64_t frame_profile_now_ns(void);

void frame_profile_add_draw(uint64_t ns);
void frame_profile_add_scene(uint64_t ns);
void frame_profile_add_vblank_wait(uint64_t ns);
void frame_profile_add_flip_wait(uint64_t ns);

/* Call once per presentation; logs an averaged summary every two seconds. */
void frame_profile_present(uint64_t present_ns, uint64_t now_ns);

#endif
