#ifndef VITA2NATIVE_NGS_RENDER_H
#define VITA2NATIVE_NGS_RENDER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Voice definitions returned by sceNgsVoiceDefGet*(). The value is the bus
 * type from the public NGS API; SceNgs.c encodes it in the opaque definition
 * pointer and the renderer uses it to pick the module stack.
 */
enum {
    NGS_BUSS_MASTER            = 0,
    NGS_BUSS_COMPRESSOR        = 1,
    NGS_BUSS_SIDE_CHAIN        = 2,
    NGS_BUSS_DELAY             = 3,
    NGS_BUSS_DISTORTION        = 4,
    NGS_BUSS_ENVELOPE          = 5,
    NGS_BUSS_EQUALIZATION      = 6,
    NGS_BUSS_MIXER             = 7,
    NGS_BUSS_PAUSER            = 8,
    NGS_BUSS_PITCH_SHIFT       = 9,
    NGS_BUSS_REVERB            = 10,
    NGS_BUSS_SAS_EMULATION     = 11,
    NGS_BUSS_SIMPLE            = 12,
    NGS_BUSS_ATRAC9            = 13,
    NGS_BUSS_SIMPLE_ATRAC9     = 14,
    NGS_BUSS_SCREAM            = 15,
    NGS_BUSS_SCREAM_ATRAC9     = 16,
    NGS_BUSS_NORMAL_PLAYER     = 17,
};

void ngs_render_reset(void);
void ngs_render_configure(int granularity, int sample_rate);
void ngs_render_voice_add(uint32_t voice, uint32_t rack, int buss, int channels);
void ngs_render_voice_remove(uint32_t voice);
void ngs_render_voice_params(uint32_t voice, uint32_t module, const void *data, uint32_t size);
void ngs_render_voice_play(uint32_t voice);
void ngs_render_voice_key_off(uint32_t voice);
void ngs_render_voice_kill(uint32_t voice);
void ngs_render_voice_pause(uint32_t voice);
void ngs_render_voice_resume(uint32_t voice);
bool ngs_render_voice_finished(uint32_t voice);
void ngs_render_voice_callbacks(uint32_t voice, void *module_callback, void *module_user,
                                void *finished_callback, void *finished_user);
void ngs_render_patch_add(uint32_t patch, uint32_t source, uint32_t dest, int output_index,
                          int sub_index, int input_index);
void ngs_render_patch_remove(uint32_t patch);
void ngs_render_patch_volume(uint32_t patch, const float matrix[2][2]);
void ngs_render_update(void);
void ngs_render_dispatch_callbacks(void);
bool ngs_render_get_state(uint32_t voice, uint32_t module, void *dest, uint32_t size);

/* Number of modules a buss type exposes (module 0 is the source/player). */
int ngs_render_module_count(int buss);
int ngs_render_output_count(int buss);

#endif
