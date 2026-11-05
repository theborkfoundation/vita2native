/*
 * Minimal SceNgs ABI definitions.
 *
 * vita-headers ships psp2/ngs_internal.h but not the public psp2/ngs.h, so the
 * guest-visible types used by this port live here. Layouts and constants are
 * the FW 3.60 ones documented by the public SceNgs API.
 */

#ifndef VITA2NATIVE_NGS_TYPES_H
#define VITA2NATIVE_NGS_TYPES_H

#include <stdint.h>

typedef uint32_t SceNgsHSynSystem;
typedef uint32_t SceNgsHRack;
typedef uint32_t SceNgsHVoice;
typedef uint32_t SceNgsHPatch;
typedef uint32_t SceNgsModuleID;
typedef uint32_t SceNgsParamsID;

typedef struct SceNgsVoiceDefinition SceNgsVoiceDefinition;

#define SCE_NGS_MAX_SYSTEM_CHANNELS 2

typedef struct SceNgsBufferInfo {
    void *data;
    uint32_t size;
} SceNgsBufferInfo;

typedef struct SceNgsSystemInitParams {
    int32_t max_racks;
    int32_t max_voices;
    int32_t granularity;
    int32_t sample_rate;
    int32_t reserved;
} SceNgsSystemInitParams;

typedef struct SceNgsRackDescription {
    const SceNgsVoiceDefinition *voice_definition;
    int32_t voice_count;
    int32_t channels_per_voice;
    int32_t max_patches_per_input;
    int32_t patches_per_output;
    void *user_data;
} SceNgsRackDescription;

typedef struct SceNgsPatchSetupInfo {
    SceNgsHVoice source_voice;
    int32_t source_output_index;
    int32_t source_output_sub_index;
    SceNgsHVoice destination_voice;
    int32_t destination_input_index;
} SceNgsPatchSetupInfo;

typedef struct SceNgsVoicePreset {
    int32_t name_offset;
    uint32_t name_length;
    int32_t preset_data_offset;
    uint32_t preset_data_size;
    int32_t bypass_flags_offset;
    uint32_t bypass_flags_count;
} SceNgsVoicePreset;

typedef struct SceNgsCallbackInfo {
    SceNgsHVoice voice_handle;
    SceNgsHRack rack_handle;
    SceNgsModuleID module_id;
    uint32_t callback_reason;
    uint32_t callback_reason2;
    void *callback_ptr;
    void *user_data;
} SceNgsCallbackInfo;

typedef void (*SceNgsCallbackFunc)(const SceNgsCallbackInfo *callback_info);
typedef SceNgsCallbackFunc SceNgsRackReleaseCallbackFunc;
typedef SceNgsCallbackFunc SceNgsModuleCallbackFunc;
typedef SceNgsCallbackFunc SceNgsParamsErrorCallbackFunc;

typedef struct SceNgsParamsDescriptor {
    SceNgsParamsID id;
    uint32_t size;
} SceNgsParamsDescriptor;

typedef struct SceNgsAT9SkipBufferInfo {
    int32_t start_byte_offset;
    int32_t num_bytes;
    int16_t start_skip;
    int16_t end_skip;
    int32_t is_super_packet;
} SceNgsAT9SkipBufferInfo;

typedef struct SceNgsModuleParamHeader {
    int32_t module_index;
    int32_t channel_index;
} SceNgsModuleParamHeader;

typedef struct SceNgsVolumeMatrix {
    float matrix[SCE_NGS_MAX_SYSTEM_CHANNELS][SCE_NGS_MAX_SYSTEM_CHANNELS];
} SceNgsVolumeMatrix;

typedef struct SceNgsPatchRouteInfo {
    int32_t output_channels;
    int32_t input_channels;
    SceNgsVolumeMatrix volume_matrix;
} SceNgsPatchRouteInfo;

typedef struct SceNgsVoiceInfo {
    uint32_t voice_state;
    uint32_t num_modules;
    uint32_t num_inputs;
    uint32_t num_outputs;
    uint32_t num_patches_per_output;
    uint32_t update_passed;
} SceNgsVoiceInfo;

typedef struct SceSulphaNgsConfig {
    uint32_t max_named_samples;
    uint32_t api_trace_buffer_size;
} SceSulphaNgsConfig;

enum {
    SCE_NGS_OK                        = 0x00000000,
    SCE_NGS_ERROR                     = 0x804A0001,
    SCE_NGS_ERROR_INVALID_PARAM       = 0x804A0002,
    SCE_NGS_ERROR_INVALID_ALIGNMENT   = 0x804A0003,
    SCE_NGS_ERROR_NOT_IMPL            = 0x804A0004,
    SCE_NGS_ERROR_DEPENDENCY          = 0x804A0005,
    SCE_NGS_ERROR_OUT_OF_ASSETS       = 0x804A0006,
    SCE_NGS_ERROR_MODULE_NOT_AVAIL    = 0x804A0007,
    SCE_NGS_ERROR_RESOURCE_LOCKED     = 0x804A0008,
    SCE_NGS_ERROR_PARAM_OUT_OF_RANGE  = 0x804A0009,
    SCE_NGS_ERROR_INVALID_VOICE_TYPE  = 0x804A000A,
    SCE_NGS_ERROR_SYSTEM_MISMATCH     = 0x804A000B,
    SCE_NGS_ERROR_INVALID_HANDLE      = 0x804A000C,
    SCE_NGS_ERROR_SIZE_MISMATCH       = 0x804A000D,
    SCE_NGS_ERROR_PATCH_NOT_AVAIL     = 0x804A000E,
    SCE_NGS_ERROR_PARAM_TYPE_MISMATCH = 0x804A000F,
    SCE_NGS_ERROR_INVALID_STATE       = 0x804A0010,
    SCE_NGS_ERROR_INTERNAL_ALLOC      = 0x804A0011,
    SCE_NGS_ERROR_INTERNAL_PROCESSING = 0x804A0012,
    SCE_NGS_ERROR_INVALID_BUFFER      = 0x804A0013,
};

enum {
    SCE_NGS_VOICE_STATE_AVAILABLE = 0x00000000,
    SCE_NGS_VOICE_STATE_ACTIVE    = 0x00000001,
    SCE_NGS_VOICE_STATE_FINALIZE  = 0x00000004,
    SCE_NGS_VOICE_STATE_UNLOADING = 0x00000008,
    SCE_NGS_VOICE_STATE_PENDING   = 0x00000010,
    SCE_NGS_VOICE_STATE_PAUSED    = 0x00000020,
    SCE_NGS_VOICE_STATE_KEY_OFF   = 0x00000040,
};

enum {
    SCE_NGS_MODULE_FLAG_NOT_BYPASSED = 0,
    SCE_NGS_MODULE_FLAG_BYPASSED     = 2,
};

enum {
    SCE_NGS_VOICE_INIT_BASE      = 0x00000000,
    SCE_NGS_VOICE_INIT_ROUTING   = 0x00000001,
    SCE_NGS_VOICE_INIT_PRESET    = 0x00000002,
    SCE_NGS_VOICE_INIT_CALLBACKS = 0x00000004,
    SCE_NGS_VOICE_INIT_ALL       = 0x00000007,
};

#endif /* VITA2NATIVE_NGS_TYPES_H */
