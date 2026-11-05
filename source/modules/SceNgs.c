/*
 * SceNgs: the Next Generation Sound library.
 *
 * NGS is a full synthesis graph (systems, racks, voices, modules, patches).
 * This host has no shell audio stack and the public graph layouts are not in
 * vita-headers, so the graph is bookkeeping: handles are opaque encoded
 * values, the object table tracks lifetimes and routing, every lifecycle call
 * succeeds, and rendering is silent. Titles that only need NGS to initialise
 * and drive their mixer keep running; SceAudioOut still plays whatever the
 * title mixes itself.
 *
 * Handle encoding: 0x4e470000 | slot << 4 | kind. The guest treats handles as
 * opaque pointers and never dereferences them.
 */

#include "ngs_types.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"

#define NGS_MAX_OBJECTS      1024
#define NGS_HANDLE_BASE      0x4e470000u
#define NGS_HANDLE_KIND_MASK 0x0000000fu
#define NGS_HANDLE_SLOT_MASK 0x000003ffu
#define NGS_LOCK_SIZE        0x1000

enum {
    NGS_KIND_NONE = 0,
    NGS_KIND_SYSTEM = 1,
    NGS_KIND_RACK = 2,
    NGS_KIND_VOICE = 3,
    NGS_KIND_PATCH = 4,
};

typedef struct {
    uint8_t kind;
    uint8_t used;
    uint8_t initialized;
    uint8_t playing;
    uint8_t paused;
    uint8_t keyed_off;
    uint8_t bypassed;
    uint8_t channels;

    uint32_t parent; /* system for a rack, rack for a voice */
    uint32_t index;  /* voice index inside its rack */
    uint32_t voice_count;
    int32_t patches_per_output;

    const void *definition;
    void *user_data;
    void *callback; /* rack-release, finished or module callback */
    void *callback_user_data;

    void *locked_params;
    uint32_t locked_module;

    uint32_t source; /* patch endpoints */
    uint32_t dest;
    int32_t output_index;
    int32_t sub_index;
    int32_t input_index;
    float matrix[SCE_NGS_MAX_SYSTEM_CHANNELS][SCE_NGS_MAX_SYSTEM_CHANNELS];
} NgsObject;

static NgsObject g_objects[NGS_MAX_OBJECTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define NGS_ENTER() pthread_mutex_lock(&g_lock)

#define NGS_LEAVE(value)                                                                           \
    do {                                                                                           \
        pthread_mutex_unlock(&g_lock);                                                             \
        return (value);                                                                            \
    } while (0)

static uint32_t ngs_handle(uint32_t slot, int kind)
{
    return NGS_HANDLE_BASE | (slot << 4) | (uint32_t)kind;
}

static NgsObject *ngs_lookup(uint32_t handle, int kind)
{
    uint32_t slot;

    if ((handle & 0xffff0000u) != NGS_HANDLE_BASE)
        return NULL;
    if ((handle & NGS_HANDLE_KIND_MASK) != (uint32_t)kind)
        return NULL;

    slot = (handle >> 4) & NGS_HANDLE_SLOT_MASK;
    if (!g_objects[slot].used || g_objects[slot].kind != kind)
        return NULL;

    return &g_objects[slot];
}


static uint32_t ngs_alloc(int kind)
{
    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        if (!g_objects[i].used) {
            memset(&g_objects[i], 0, sizeof(g_objects[i]));
            g_objects[i].used = 1;
            g_objects[i].kind = (uint8_t)kind;
            return ngs_handle(i, kind);
        }
    }

    return 0;
}

static void ngs_free(uint32_t handle, int kind)
{
    NgsObject *object = ngs_lookup(handle, kind);

    if (!object)
        return;

    free(object->locked_params);
    memset(object, 0, sizeof(*object));
}

static uint32_t ngs_voice_system(uint32_t voice_handle)
{
    NgsObject *voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    NgsObject *rack;

    if (!voice)
        return 0;

    rack = ngs_lookup(voice->parent, NGS_KIND_RACK);
    return rack ? rack->parent : 0;
}

static int ngs_voice_in_rack(uint32_t voice_handle, uint32_t rack_handle)
{
    NgsObject *voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);

    return voice && voice->parent == rack_handle;
}

static void ngs_release_rack_locked(uint32_t rack_handle)
{
    /* Patches go first: their endpoints are about to disappear. */
    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        NgsObject *object = &g_objects[i];

        if (!object->used || object->kind != NGS_KIND_PATCH)
            continue;
        if (ngs_voice_in_rack(object->source, rack_handle) ||
            ngs_voice_in_rack(object->dest, rack_handle))
            ngs_free(ngs_handle(i, NGS_KIND_PATCH), NGS_KIND_PATCH);
    }

    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        NgsObject *object = &g_objects[i];

        if (object->used && object->kind == NGS_KIND_VOICE && object->parent == rack_handle)
            ngs_free(ngs_handle(i, NGS_KIND_VOICE), NGS_KIND_VOICE);
    }

    ngs_free(rack_handle, NGS_KIND_RACK);
}

static void ngs_release_system_locked(uint32_t system_handle)
{
    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        NgsObject *object = &g_objects[i];

        if (object->used && object->kind == NGS_KIND_RACK && object->parent == system_handle)
            ngs_release_rack_locked(ngs_handle(i, NGS_KIND_RACK));
    }

    ngs_free(system_handle, NGS_KIND_SYSTEM);
}

/* ------------------------------------------------------------------------- */
/* ATRAC9 helpers                                                            */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x2A9FA501, int32_t, sceNgsAT9GetSectionDetails, int32_t samples_start,
       int32_t num_samples, int32_t config_data, SceNgsAT9SkipBufferInfo *info)
{
    int32_t sample_rate_index;
    int32_t frame_bytes;
    int32_t samples_per_frame;
    uint32_t start;
    int32_t super_frame;

    if (!info)
        return SCE_NGS_ERROR_INVALID_PARAM;

    memset(info, 0, sizeof(*info));

    if ((config_data & 0xff) != 0xfe || num_samples < 0)
        return SCE_NGS_ERROR;

    sample_rate_index = (config_data >> 12) & 0xf;
    if (sample_rate_index == 1)
        samples_per_frame = 64;
    else if (sample_rate_index == 4)
        samples_per_frame = 128;
    else if (sample_rate_index == 7)
        samples_per_frame = 256;
    else
        return SCE_NGS_ERROR;

    frame_bytes = (int32_t)(((((uint32_t)config_data & 0xff0000u) >> 16) << 3) |
                             (((uint32_t)config_data & (7u << 29)) >> 29)) +
                  1;
    super_frame = (config_data & (3 << 27)) != 0;

    start = (uint32_t)samples_start;
    if (start & 0x80000000u) {
        /* Decoder preroll requested by SDK 3.36 and newer. */
        start &= ~0x80000000u;
        start = start > (uint32_t)samples_per_frame ? start - (uint32_t)samples_per_frame : 0;
    }

    info->is_super_packet = super_frame;

    if (super_frame) {
        int64_t superframe_bytes = (int64_t)frame_bytes * 4;
        int64_t superframe_samples = (int64_t)samples_per_frame * 4;
        int64_t sample_offset;
        int64_t sample_skip;
        int64_t sf_offset;
        int64_t total_sf;

        start += (uint32_t)samples_per_frame;
        sf_offset = start / superframe_samples;
        sample_offset = sf_offset * superframe_samples;
        sample_skip = (int64_t)start - sample_offset;

        info->start_byte_offset = (int32_t)(sf_offset * superframe_bytes);
        info->start_skip = (int16_t)sample_skip;

        total_sf = ((int64_t)start + num_samples + superframe_samples - 1) / superframe_samples;
        info->num_bytes = (int32_t)((total_sf - sf_offset) * superframe_bytes);
        info->end_skip = (int16_t)(total_sf * superframe_samples - ((int64_t)start + num_samples));

        if (sample_skip < samples_per_frame && sf_offset > 0) {
            /* Include the previous superframe so the decoder can preroll. */
            info->start_byte_offset -= (int32_t)superframe_bytes;
            info->start_skip = (int16_t)(sample_skip + superframe_samples);
            info->num_bytes += (int32_t)superframe_bytes;
        }
    } else {
        int64_t frame_offset = start / samples_per_frame;
        int64_t sample_offset = frame_offset * samples_per_frame;
        int64_t start_skip = ((int64_t)start + samples_per_frame) - sample_offset;
        int64_t total_frames;

        info->start_byte_offset = (int32_t)(frame_offset * frame_bytes);
        info->start_skip = (int16_t)start_skip;

        total_frames = (start_skip + num_samples + samples_per_frame - 1) / samples_per_frame;
        info->num_bytes = (int32_t)(total_frames * frame_bytes);
        info->end_skip = (int16_t)(total_frames * samples_per_frame - (start_skip + num_samples));
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Module presets                                                            */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x5C71FE09, int32_t, sceNgsModuleGetNumPresets, SceNgsHSynSystem system,
       SceNgsModuleID module_id, uint32_t *num_presets)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!num_presets)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module_id;
    *num_presets = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xC58298A7, int32_t, sceNgsModuleGetPreset, SceNgsHSynSystem system,
       SceNgsModuleID module_id, uint32_t preset_index, SceNgsBufferInfo *params_buffer)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!params_buffer)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module_id;
    (void)preset_index;
    (void)params_buffer;

    /* No built-in presets are advertised, so there is nothing to copy. */
    NGS_LEAVE(0);
}

/* ------------------------------------------------------------------------- */
/* Patches                                                                   */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0xD668B49C, int32_t, sceNgsPatchCreateRouting,
       const SceNgsPatchSetupInfo *patch_info, SceNgsHPatch *patch_handle)
{
    NgsObject *source;
    NgsObject *dest;
    uint32_t source_system;
    uint32_t dest_system;
    uint32_t handle;
    int32_t sub_index;

    NGS_ENTER();

    if (!patch_info || !patch_handle)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    source = ngs_lookup(patch_info->source_voice, NGS_KIND_VOICE);
    dest = ngs_lookup(patch_info->destination_voice, NGS_KIND_VOICE);
    if (!source || !dest)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    source_system = ngs_voice_system(patch_info->source_voice);
    dest_system = ngs_voice_system(patch_info->destination_voice);
    if (!source_system || source_system != dest_system)
        NGS_LEAVE(SCE_NGS_ERROR_SYSTEM_MISMATCH);

    if (patch_info->source_output_index < 0 || patch_info->destination_input_index < 0)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    sub_index = patch_info->source_output_sub_index;
    if (sub_index < 0) {
        for (sub_index = 0; sub_index < 64; sub_index++) {
            int used = 0;

            for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
                NgsObject *object = &g_objects[i];

                if (object->used && object->kind == NGS_KIND_PATCH &&
                    object->source == patch_info->source_voice &&
                    object->output_index == patch_info->source_output_index &&
                    object->sub_index == sub_index) {
                    used = 1;
                    break;
                }
            }
            if (!used)
                break;
        }
    }

    if (sub_index < 0 || sub_index >= 64)
        NGS_LEAVE(SCE_NGS_ERROR_PATCH_NOT_AVAIL);

    handle = ngs_alloc(NGS_KIND_PATCH);
    if (!handle)
        NGS_LEAVE(SCE_NGS_ERROR_OUT_OF_ASSETS);

    {
        NgsObject *patch = ngs_lookup(handle, NGS_KIND_PATCH);

        patch->source = patch_info->source_voice;
        patch->dest = patch_info->destination_voice;
        patch->output_index = patch_info->source_output_index;
        patch->sub_index = sub_index;
        patch->input_index = patch_info->destination_input_index;
    }

    *patch_handle = handle;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x98703DBC, int32_t, sceNgsPatchGetInfo, SceNgsHPatch patch_handle,
       SceNgsPatchRouteInfo *route_info, SceNgsPatchSetupInfo *setup)
{
    NgsObject *patch;
    NgsObject *source;
    NgsObject *dest;

    NGS_ENTER();

    patch = ngs_lookup(patch_handle, NGS_KIND_PATCH);
    if (!patch)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    source = ngs_lookup(patch->source, NGS_KIND_VOICE);
    dest = ngs_lookup(patch->dest, NGS_KIND_VOICE);

    if (route_info) {
        memset(route_info, 0, sizeof(*route_info));
        route_info->output_channels = source ? source->channels : 2;
        route_info->input_channels = dest ? dest->channels : 2;
        memcpy(route_info->volume_matrix.matrix, patch->matrix, sizeof(patch->matrix));
    }

    if (setup) {
        memset(setup, 0, sizeof(*setup));
        setup->source_voice = patch->source;
        setup->source_output_index = patch->output_index;
        setup->source_output_sub_index = patch->sub_index;
        setup->destination_voice = patch->dest;
        setup->destination_input_index = patch->input_index;
    }

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xD0C9AE5A, int32_t, sceNgsPatchRemoveRouting, SceNgsHPatch patch_handle)
{
    NGS_ENTER();

    if (!ngs_lookup(patch_handle, NGS_KIND_PATCH))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    ngs_free(patch_handle, NGS_KIND_PATCH);
    NGS_LEAVE(0);
}

/* ------------------------------------------------------------------------- */
/* Racks                                                                     */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x477318C0, int32_t, sceNgsRackGetRequiredMemorySize, SceNgsHSynSystem system,
       const SceNgsRackDescription *description, uint32_t *size)
{
    int32_t voices;

    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!description || !size || description->voice_count <= 0)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    voices = description->voice_count;
    if (voices > 256)
        voices = 256;

    *size = (uint32_t)(0x400 + voices * 0x200 + 0xf) & ~0xfu;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x0A92E4EC, int32_t, sceNgsRackInit, SceNgsHSynSystem system,
       const SceNgsBufferInfo *info, const SceNgsRackDescription *description,
       SceNgsHRack *rack_handle)
{
    uint32_t rack;
    uint32_t voices;
    NgsObject *object;

    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!description || !rack_handle || description->voice_count <= 0)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);
    if (description->channels_per_voice != 1 && description->channels_per_voice != 2)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)info;

    voices = (uint32_t)description->voice_count;
    if (voices > 256)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    rack = ngs_alloc(NGS_KIND_RACK);
    if (!rack)
        NGS_LEAVE(SCE_NGS_ERROR_OUT_OF_ASSETS);

    object = ngs_lookup(rack, NGS_KIND_RACK);
    object->parent = system;
    object->voice_count = voices;
    object->channels = (uint8_t)description->channels_per_voice;
    object->patches_per_output = description->patches_per_output;
    object->definition = description->voice_definition;
    object->user_data = description->user_data;

    for (uint32_t i = 0; i < voices; i++) {
        uint32_t voice = ngs_alloc(NGS_KIND_VOICE);
        NgsObject *voice_object;

        if (!voice) {
            ngs_release_rack_locked(rack);
            NGS_LEAVE(SCE_NGS_ERROR_OUT_OF_ASSETS);
        }

        voice_object = ngs_lookup(voice, NGS_KIND_VOICE);
        voice_object->parent = rack;
        voice_object->index = i;
        voice_object->channels = (uint8_t)description->channels_per_voice;
        voice_object->patches_per_output = description->patches_per_output;
        voice_object->definition = description->voice_definition;
    }

    *rack_handle = rack;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xFE1A98E9, int32_t, sceNgsRackGetVoiceHandle, SceNgsHRack rack_handle,
       uint32_t index, SceNgsHVoice *voice_handle)
{
    NgsObject *rack;

    NGS_ENTER();

    rack = ngs_lookup(rack_handle, NGS_KIND_RACK);
    if (!rack)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!voice_handle)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);
    if (index >= rack->voice_count)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        NgsObject *object = &g_objects[i];

        if (object->used && object->kind == NGS_KIND_VOICE && object->parent == rack_handle &&
            object->index == index) {
            *voice_handle = ngs_handle(i, NGS_KIND_VOICE);
            NGS_LEAVE(0);
        }
    }

    NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
}

EXPORT(SceNgs, 0xDD5CA10B, int32_t, sceNgsRackRelease, SceNgsHRack rack_handle,
       SceNgsRackReleaseCallbackFunc callback)
{
    NGS_ENTER();

    if (!ngs_lookup(rack_handle, NGS_KIND_RACK))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)callback;

    ngs_release_rack_locked(rack_handle);
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x534B6E3F, int32_t, sceNgsRackSetParamErrorCallback, SceNgsHRack rack_handle,
       SceNgsParamsErrorCallbackFunc callback)
{
    NgsObject *rack;

    NGS_ENTER();

    rack = ngs_lookup(rack_handle, NGS_KIND_RACK);
    if (!rack)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    rack->callback = (void *)callback;
    NGS_LEAVE(0);
}

/* ------------------------------------------------------------------------- */
/* Systems                                                                   */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x6CE8B36F, int32_t, sceNgsSystemGetRequiredMemorySize,
       const SceNgsSystemInitParams *params, uint32_t *size)
{
    uint32_t racks;
    uint32_t voices;

    if (!params || !size)
        return SCE_NGS_ERROR_INVALID_PARAM;

    racks = (uint32_t)(params->max_racks > 0 ? params->max_racks : 1);
    voices = (uint32_t)(params->max_voices > 0 ? params->max_voices : 1);
    if (racks > 64)
        racks = 64;
    if (voices > 256)
        voices = 256;

    *size = (0x1000 + racks * 0x200 + voices * 0x100 + 0xf) & ~0xfu;
    return 0;
}

EXPORT(SceNgs, 0xED14CF4A, int32_t, sceNgsSystemInit, void *memspace, uint32_t memspace_size,
       const SceNgsSystemInitParams *params, SceNgsHSynSystem *system)
{
    uint32_t handle;

    NGS_ENTER();

    if (!memspace || !system || !params)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);
    if (params->max_racks <= 0 || params->max_voices <= 0)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)memspace_size;

    handle = ngs_alloc(NGS_KIND_SYSTEM);
    if (!handle)
        NGS_LEAVE(SCE_NGS_ERROR_OUT_OF_ASSETS);

    *system = handle;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xB9D971F2, int32_t, sceNgsSystemLock, SceNgsHSynSystem system)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x4A25BEBC, int32_t, sceNgsSystemRelease, SceNgsHSynSystem system)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    ngs_release_system_locked(system);
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x64D80013, int32_t, sceNgsSystemSetFlags, SceNgsHSynSystem system,
       uint32_t system_flags)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)system_flags;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x5ADD22DC, int32_t, sceNgsSystemSetParamErrorCallback, SceNgsHSynSystem system,
       SceNgsParamsErrorCallbackFunc callback)
{
    NgsObject *object;

    NGS_ENTER();

    object = ngs_lookup(system, NGS_KIND_SYSTEM);
    if (!object)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    object->callback = (void *)callback;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x0A93EA96, int32_t, sceNgsSystemUnlock, SceNgsHSynSystem system)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x684F080C, int32_t, sceNgsSystemUpdate, SceNgsHSynSystem system)
{
    NGS_ENTER();

    if (!ngs_lookup(system, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    /* Callbacks are queued by the real mixer; this host renders silence. */
    NGS_LEAVE(0);
}

/* ------------------------------------------------------------------------- */
/* Voices                                                                    */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x9AB87E71, int32_t, sceNgsVoiceBypassModule, SceNgsHVoice voice_handle,
       uint32_t module, uint32_t bypass_flag)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)module;
    voice->bypassed = bypass_flag ? 1 : 0;
    NGS_LEAVE(0);
}

#define VOICE_DEF(nid, name, slot)                                                                 \
    EXPORT(SceNgs, nid, const SceNgsVoiceDefinition *, name, void)                                 \
    {                                                                                              \
        return (const SceNgsVoiceDefinition *)(uintptr_t)(VOICE_DEF_BASE + (slot));                \
    }

#define VOICE_DEF_BASE 0x4e471000u

VOICE_DEF(0x14EF65A0, sceNgsVoiceDefGetAtrac9Voice, 0x01)
VOICE_DEF(0x0E0ACB68, sceNgsVoiceDefGetCompressorBuss, 0x02)
VOICE_DEF(0x1AF83512, sceNgsVoiceDefGetCompressorSideChainBuss, 0x03)
VOICE_DEF(0x4D705E3E, sceNgsVoiceDefGetDelayBuss, 0x04)
VOICE_DEF(0xAAD90DEB, sceNgsVoiceDefGetDistortionBuss, 0x05)
VOICE_DEF(0xF6B68C31, sceNgsVoiceDefGetEnvelopeBuss, 0x06)
VOICE_DEF(0xF964120E, sceNgsVoiceDefGetEqBuss, 0x07)
VOICE_DEF(0x79A121D1, sceNgsVoiceDefGetMasterBuss, 0x08)
VOICE_DEF(0xE0AC8776, sceNgsVoiceDefGetMixerBuss, 0x09)
VOICE_DEF(0x214485D6, sceNgsVoiceDefGetPauserBuss, 0x0a)
VOICE_DEF(0xFBE515D4, sceNgsVoiceDefGetPitchShiftBuss, 0x0b)
VOICE_DEF(0x9DCF50F5, sceNgsVoiceDefGetReverbBuss, 0x0c)
VOICE_DEF(0x1F51C2BA, sceNgsVoiceDefGetSasEmuVoice, 0x0d)
VOICE_DEF(0xCD63A2BF, sceNgsVoiceDefGetScreamAtrac9Voice, 0x0e)
VOICE_DEF(0xCE53BC33, sceNgsVoiceDefGetScreamVoice, 0x0f)
VOICE_DEF(0x45CF2A73, sceNgsVoiceDefGetSimpleAtrac9Voice, 0x10)
VOICE_DEF(0x0D5399CF, sceNgsVoiceDefGetSimpleVoice, 0x11)
VOICE_DEF(0xE9B572B7, sceNgsVoiceDefGetTemplate1, 0x12)

EXPORT(SceNgs, 0x5551410D, int32_t, sceNgsVoiceGetInfo, SceNgsHVoice voice_handle,
       SceNgsVoiceInfo *info)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!info)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    memset(info, 0, sizeof(*info));
    if (!voice->initialized)
        info->voice_state = SCE_NGS_VOICE_STATE_AVAILABLE;
    else if (voice->keyed_off)
        info->voice_state = SCE_NGS_VOICE_STATE_KEY_OFF;
    else if (voice->paused)
        info->voice_state = SCE_NGS_VOICE_STATE_PAUSED;
    else if (voice->playing)
        info->voice_state = SCE_NGS_VOICE_STATE_ACTIVE;

    info->num_modules = 1;
    info->num_inputs = 1;
    info->num_outputs = 1;
    info->num_patches_per_output =
        voice->patches_per_output > 0 ? (uint32_t)voice->patches_per_output : 1;

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x431BF3AB, int32_t, sceNgsVoiceGetModuleBypass, SceNgsHVoice voice_handle,
       uint32_t module, uint32_t *bypass_flag)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!bypass_flag)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module;
    *bypass_flag = voice->bypassed ? SCE_NGS_MODULE_FLAG_BYPASSED : SCE_NGS_MODULE_FLAG_NOT_BYPASSED;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xB307185E, int32_t, sceNgsVoiceGetModuleType, SceNgsHVoice voice_handle,
       uint32_t module, SceNgsModuleID *module_type)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!module_type)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module;
    *module_type = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x01A52E3A, int32_t, sceNgsVoiceGetOutputPatch, SceNgsHVoice voice_handle,
       int32_t output_index, int32_t sub_index, SceNgsHPatch *patch_handle)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!patch_handle)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    *patch_handle = 0;
    for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
        NgsObject *object = &g_objects[i];

        if (object->used && object->kind == NGS_KIND_PATCH && object->source == voice_handle &&
            object->output_index == output_index && object->sub_index == sub_index) {
            *patch_handle = ngs_handle(i, NGS_KIND_PATCH);
            break;
        }
    }

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x4CBE08F3, int32_t, sceNgsVoiceGetParamsOutOfRange, SceNgsHVoice voice_handle,
       uint32_t module, char *message_buffer)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!message_buffer)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module;
    memcpy(message_buffer, "No value was out of range\n", 27);
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xC9B8C0B4, int32_t, sceNgsVoiceGetStateData, SceNgsHVoice voice_handle,
       uint32_t module, void *memory, uint32_t mem_size)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!memory && mem_size)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)module;
    if (memory)
        memset(memory, 0, mem_size);

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x1DDBEBEB, int32_t, sceNgsVoiceInit, SceNgsHVoice voice_handle,
       const SceNgsVoicePreset *preset, uint32_t init_flags)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->initialized = 1;
    voice->playing = 0;
    voice->paused = 0;
    voice->keyed_off = 0;

    if (init_flags & SCE_NGS_VOICE_INIT_CALLBACKS) {
        voice->callback = NULL;
        voice->callback_user_data = NULL;
    }

    if (init_flags & SCE_NGS_VOICE_INIT_ROUTING) {
        for (uint32_t i = 0; i < NGS_MAX_OBJECTS; i++) {
            NgsObject *object = &g_objects[i];

            if (object->used && object->kind == NGS_KIND_PATCH &&
                (object->source == voice_handle || object->dest == voice_handle))
                ngs_free(ngs_handle(i, NGS_KIND_PATCH), NGS_KIND_PATCH);
        }
    }

    (void)preset;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xBB13373D, int32_t, sceNgsVoiceKeyOff, SceNgsHVoice voice_handle)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->initialized = 1;
    voice->keyed_off = 1;
    voice->paused = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x0E291AAD, int32_t, sceNgsVoiceKill, SceNgsHVoice voice_handle)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->playing = 0;
    voice->paused = 0;
    voice->keyed_off = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xAB6BEF8F, int32_t, sceNgsVoiceLockParams, SceNgsHVoice voice_handle,
       uint32_t module, SceNgsParamsID params_interface_id, SceNgsBufferInfo *params_buffer)
{
    NgsObject *voice;
    void *buffer;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!params_buffer)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);
    if (voice->locked_params)
        NGS_LEAVE(SCE_NGS_ERROR_RESOURCE_LOCKED);

    (void)params_interface_id;

    buffer = calloc(1, NGS_LOCK_SIZE);
    if (!buffer)
        NGS_LEAVE(SCE_NGS_ERROR_INTERNAL_ALLOC);

    voice->locked_params = buffer;
    voice->locked_module = module;
    params_buffer->data = buffer;
    params_buffer->size = NGS_LOCK_SIZE;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xA3C807BC, int32_t, sceNgsVoicePatchSetVolume, SceNgsHPatch patch_handle,
       int32_t output_channel, int32_t input_channel, float volume)
{
    NgsObject *patch;

    NGS_ENTER();

    patch = ngs_lookup(patch_handle, NGS_KIND_PATCH);
    if (!patch)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (output_channel < 0 || output_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS || input_channel < 0 ||
        input_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    patch->matrix[output_channel][input_channel] = volume;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xBD6F57F0, int32_t, sceNgsVoicePatchSetVolumes, SceNgsHPatch patch_handle,
       int32_t output_channel, const float *volumes, int32_t volume_count)
{
    NgsObject *patch;

    NGS_ENTER();

    patch = ngs_lookup(patch_handle, NGS_KIND_PATCH);
    if (!patch)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!volumes || volume_count <= 0 || output_channel < 0 ||
        output_channel >= SCE_NGS_MAX_SYSTEM_CHANNELS)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    for (int32_t i = 0; i < volume_count && i < SCE_NGS_MAX_SYSTEM_CHANNELS; i++)
        patch->matrix[output_channel][i] = volumes[i];

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xA0F5402D, int32_t, sceNgsVoicePatchSetVolumesMatrix, SceNgsHPatch patch_handle,
       const SceNgsVolumeMatrix *matrix)
{
    NgsObject *patch;

    NGS_ENTER();

    patch = ngs_lookup(patch_handle, NGS_KIND_PATCH);
    if (!patch)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!matrix)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    memcpy(patch->matrix, matrix->matrix, sizeof(patch->matrix));
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xD7786E99, int32_t, sceNgsVoicePause, SceNgsHVoice voice_handle)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->paused = 1;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xFA0A0F34, int32_t, sceNgsVoicePlay, SceNgsHVoice voice_handle)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->initialized = 1;
    voice->playing = 1;
    voice->paused = 0;
    voice->keyed_off = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x54CFB981, int32_t, sceNgsVoiceResume, SceNgsHVoice voice_handle)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->paused = 0;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x17A6F564, int32_t, sceNgsVoiceSetFinishedCallback, SceNgsHVoice voice_handle,
       SceNgsCallbackFunc callback, void *user_data)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    voice->callback = (void *)callback;
    voice->callback_user_data = user_data;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x24E909A8, int32_t, sceNgsVoiceSetModuleCallback, SceNgsHVoice voice_handle,
       uint32_t module, SceNgsModuleCallbackFunc callback, void *user_data)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)module;
    voice->callback = (void *)callback;
    voice->callback_user_data = user_data;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xFB8174B1, int32_t, sceNgsVoiceSetParamsBlock, SceNgsHVoice voice_handle,
       const SceNgsModuleParamHeader *param_data, uint32_t size, int32_t *error_count)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);
    if (!param_data && size)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    (void)param_data;
    (void)size;

    if (error_count)
        *error_count = 0;

    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x8A88E665, int32_t, sceNgsVoiceSetPreset, SceNgsHVoice voice_handle,
       const SceNgsVoicePreset *voice_preset)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)voice_preset;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x3D46D8A7, int32_t, sceNgsVoiceUnlockParams, SceNgsHVoice voice_handle,
       uint32_t module)
{
    NgsObject *voice;

    NGS_ENTER();

    voice = ngs_lookup(voice_handle, NGS_KIND_VOICE);
    if (!voice)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    if (!voice->locked_params)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_STATE);
    if (voice->locked_module != module)
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_PARAM);

    free(voice->locked_params);
    voice->locked_params = NULL;
    NGS_LEAVE(0);
}

/* ------------------------------------------------------------------------- */
/* Sulpha tracing                                                            */
/* ------------------------------------------------------------------------- */

EXPORT(SceNgs, 0x5FD8AEDB, int32_t, sceSulphaNgsGetDefaultConfig, SceSulphaNgsConfig *config)
{
    if (!config)
        return SCE_NGS_ERROR_INVALID_PARAM;

    config->max_named_samples = 1024;
    config->api_trace_buffer_size = 4096;
    return 0;
}

EXPORT(SceNgs, 0x793E3E8C, int32_t, sceSulphaNgsGetNeededMemory,
       const SceSulphaNgsConfig *config, uint32_t *size_in_bytes)
{
    uint64_t total;

    if (!config || !size_in_bytes)
        return SCE_NGS_ERROR_INVALID_PARAM;

    total = (uint64_t)config->max_named_samples * 32 + config->api_trace_buffer_size + 0x1000;
    if (total > 0x01000000ull)
        total = 0x01000000ull;

    *size_in_bytes = ((uint32_t)total + 0xf) & ~0xfu;
    return 0;
}

EXPORT(SceNgs, 0xAFCD824F, int32_t, sceSulphaNgsInit, const SceSulphaNgsConfig *config,
       void *buffer, uint32_t size_in_bytes)
{
    (void)config;
    (void)buffer;
    (void)size_in_bytes;
    return 0;
}

EXPORT(SceNgs, 0x251AF6A9, int32_t, sceSulphaNgsSetRackName, SceNgsHRack rack_handle,
       const char *name)
{
    NGS_ENTER();

    if (!ngs_lookup(rack_handle, NGS_KIND_RACK))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)name;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x54EC5B8D, int32_t, sceSulphaNgsSetSampleName, const void *location,
       uint32_t length, const char *name)
{
    if (!location || !length)
        return SCE_NGS_ERROR_INVALID_PARAM;

    (void)name;
    return 0;
}

EXPORT(SceNgs, 0x2F3F7515, int32_t, sceSulphaNgsSetSynthName, SceNgsHSynSystem synth_handle,
       const char *name)
{
    NGS_ENTER();

    if (!ngs_lookup(synth_handle, NGS_KIND_SYSTEM))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)name;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0x508975BD, int32_t, sceSulphaNgsSetVoiceName, SceNgsHVoice voice_handle,
       const char *name)
{
    NGS_ENTER();

    if (!ngs_lookup(voice_handle, NGS_KIND_VOICE))
        NGS_LEAVE(SCE_NGS_ERROR_INVALID_HANDLE);

    (void)name;
    NGS_LEAVE(0);
}

EXPORT(SceNgs, 0xD124BFB1, int32_t, sceSulphaNgsShutdown, void)
{
    return 0;
}

EXPORT(SceNgs, 0xDC7C0F05, int32_t, sceSulphaNgsTrace, const char *message)
{
    (void)message;
    return 0;
}

DECLARE_LIBRARY(SceNgs, 0xB01598D9);
