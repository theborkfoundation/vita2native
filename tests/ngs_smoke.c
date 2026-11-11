/*
 * Exercises the complete SceNgs bookkeeping surface: system, racks, voices,
 * patches, parameter locks and the ATRAC9 section helper.
 */

#include "ngs_types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#endif

#include "test.h"

int32_t sceNgsAT9GetSectionDetails(int32_t samples_start, int32_t num_samples, int32_t config_data,
                                   SceNgsAT9SkipBufferInfo *info);
int32_t sceNgsModuleGetNumPresets(SceNgsHSynSystem system, SceNgsModuleID module_id,
                                  uint32_t *num_presets);
int32_t sceNgsModuleGetPreset(SceNgsHSynSystem system, SceNgsModuleID module_id,
                              uint32_t preset_index, SceNgsBufferInfo *params_buffer);
int32_t sceNgsPatchCreateRouting(const SceNgsPatchSetupInfo *patch_info,
                                 SceNgsHPatch *patch_handle);
int32_t sceNgsPatchGetInfo(SceNgsHPatch patch_handle, SceNgsPatchRouteInfo *route_info,
                           SceNgsPatchSetupInfo *setup);
int32_t sceNgsPatchRemoveRouting(SceNgsHPatch patch_handle);
int32_t sceNgsRackGetRequiredMemorySize(SceNgsHSynSystem system,
                                        const SceNgsRackDescription *description, uint32_t *size);
int32_t sceNgsRackInit(SceNgsHSynSystem system, const SceNgsBufferInfo *info,
                       const SceNgsRackDescription *description, SceNgsHRack *rack_handle);
int32_t sceNgsRackGetVoiceHandle(SceNgsHRack rack_handle, uint32_t index,
                                 SceNgsHVoice *voice_handle);
int32_t sceNgsRackRelease(SceNgsHRack rack_handle, SceNgsRackReleaseCallbackFunc callback);
int32_t sceNgsRackSetParamErrorCallback(SceNgsHRack rack_handle,
                                        SceNgsParamsErrorCallbackFunc callback);
int32_t sceNgsSystemGetRequiredMemorySize(const SceNgsSystemInitParams *params, uint32_t *size);
int32_t sceNgsSystemInit(void *memspace, uint32_t memspace_size,
                         const SceNgsSystemInitParams *params, SceNgsHSynSystem *system);
int32_t sceNgsSystemLock(SceNgsHSynSystem system);
int32_t sceNgsSystemRelease(SceNgsHSynSystem system);
int32_t sceNgsSystemSetFlags(SceNgsHSynSystem system, uint32_t system_flags);
int32_t sceNgsSystemSetParamErrorCallback(SceNgsHSynSystem system,
                                          SceNgsParamsErrorCallbackFunc callback);
int32_t sceNgsSystemUnlock(SceNgsHSynSystem system);
int32_t sceNgsSystemUpdate(SceNgsHSynSystem system);
int32_t sceNgsVoiceBypassModule(SceNgsHVoice voice_handle, uint32_t module, uint32_t bypass_flag);
const SceNgsVoiceDefinition *sceNgsVoiceDefGetMasterBuss(void);
const SceNgsVoiceDefinition *sceNgsVoiceDefGetSimpleVoice(void);
const SceNgsVoiceDefinition *sceNgsVoiceDefGetTemplate1(void);
int32_t sceNgsVoiceGetInfo(SceNgsHVoice voice_handle, SceNgsVoiceInfo *info);
int32_t sceNgsVoiceGetModuleBypass(SceNgsHVoice voice_handle, uint32_t module,
                                   uint32_t *bypass_flag);
int32_t sceNgsVoiceGetModuleType(SceNgsHVoice voice_handle, uint32_t module,
                                 SceNgsModuleID *module_type);
int32_t sceNgsVoiceGetOutputPatch(SceNgsHVoice voice_handle, int32_t output_index,
                                  int32_t sub_index, SceNgsHPatch *patch_handle);
int32_t sceNgsVoiceGetParamsOutOfRange(SceNgsHVoice voice_handle, uint32_t module,
                                       char *message_buffer);
int32_t sceNgsVoiceGetStateData(SceNgsHVoice voice_handle, uint32_t module, void *memory,
                                uint32_t mem_size);
int32_t sceNgsVoiceInit(SceNgsHVoice voice_handle, const SceNgsVoicePreset *preset,
                        uint32_t init_flags);
int32_t sceNgsVoiceKeyOff(SceNgsHVoice voice_handle);
int32_t sceNgsVoiceKill(SceNgsHVoice voice_handle);
int32_t sceNgsVoiceLockParams(SceNgsHVoice voice_handle, uint32_t module,
                              SceNgsParamsID params_interface_id, SceNgsBufferInfo *params_buffer);
int32_t sceNgsVoicePatchSetVolume(SceNgsHPatch patch_handle, int32_t output_channel,
                                  int32_t input_channel, float volume);
int32_t sceNgsVoicePatchSetVolumes(SceNgsHPatch patch_handle, int32_t output_channel,
                                   const float *volumes, int32_t volume_count);
int32_t sceNgsVoicePatchSetVolumesMatrix(SceNgsHPatch patch_handle,
                                         const SceNgsVolumeMatrix *matrix);
int32_t sceNgsVoicePause(SceNgsHVoice voice_handle);
int32_t sceNgsVoicePlay(SceNgsHVoice voice_handle);
int32_t sceNgsVoiceResume(SceNgsHVoice voice_handle);
int32_t sceNgsVoiceSetFinishedCallback(SceNgsHVoice voice_handle, SceNgsCallbackFunc callback,
                                       void *user_data);
int32_t sceNgsVoiceSetModuleCallback(SceNgsHVoice voice_handle, uint32_t module,
                                     SceNgsModuleCallbackFunc callback, void *user_data);
int32_t sceNgsVoiceSetParamsBlock(SceNgsHVoice voice_handle,
                                  const SceNgsModuleParamHeader *param_data, uint32_t size,
                                  int32_t *error_count);
int32_t sceNgsVoiceSetPreset(SceNgsHVoice voice_handle, const SceNgsVoicePreset *voice_preset);
int32_t sceNgsVoiceUnlockParams(SceNgsHVoice voice_handle, uint32_t module);
int32_t sceSulphaNgsGetDefaultConfig(SceSulphaNgsConfig *config);
int32_t sceSulphaNgsGetNeededMemory(const SceSulphaNgsConfig *config, uint32_t *size_in_bytes);
int32_t sceSulphaNgsInit(const SceSulphaNgsConfig *config, void *buffer, uint32_t size_in_bytes);
int32_t sceSulphaNgsSetRackName(SceNgsHRack rack_handle, const char *name);
int32_t sceSulphaNgsSetSampleName(const void *location, uint32_t length, const char *name);
int32_t sceSulphaNgsSetSynthName(SceNgsHSynSystem synth_handle, const char *name);
int32_t sceSulphaNgsSetVoiceName(SceNgsHVoice voice_handle, const char *name);
int32_t sceSulphaNgsShutdown(void);
int32_t sceSulphaNgsTrace(const char *message);

static void finished_callback(const SceNgsCallbackInfo *info)
{
    (void)info;
}

/* Guest ABI of the Simple voice player parameters (FW 3.60). */
typedef struct {
    uint32_t buffer;
    int32_t bytes;
    int16_t loop_count;
    int16_t next_buffer;
} TestPlayerBuffer;

typedef struct {
    uint32_t id;
    uint32_t size;
    TestPlayerBuffer buffers[4];
    float playback_frequency;
    float playback_scalar;
    int32_t lead_in;
    int32_t limit;
    int32_t start_bytes;
    int8_t channels;
    int8_t channel_map[2];
    int8_t type;
    int8_t res;
    int8_t start_buffer;
    int8_t padding[2];
} TestPlayerParams;

_Static_assert(sizeof(TestPlayerParams) == 84, "player params size");
_Static_assert(offsetof(TestPlayerParams, channels) == 76, "player channels offset");
_Static_assert(offsetof(TestPlayerParams, type) == 79, "player type offset");

/* Guest pointers are real 32-bit host pointers, so keep the sample buffer low. */
static uint8_t *alloc_guest_bytes(size_t size)
{
#if defined(__linux__) && defined(__x86_64__)
    void *memory =
        mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

    return memory == MAP_FAILED ? NULL : memory;
#else
    return calloc(1, size);
#endif
}

static void free_guest_bytes(uint8_t *memory, size_t size)
{
#if defined(__linux__) && defined(__x86_64__)
    munmap(memory, size);
#else
    (void)size;
    free(memory);
#endif
}

/*
 * Render a 64-frame mono HEVAG buffer with a marker in frame 40. The decoder
 * used to advance the guest position by 64 frames while only decoding the 36
 * that fit its scratch buffer, silently dropping the rest of the stream.
 */
static int run_hevag_render_test(void)
{
    SceNgsSystemInitParams init = {
        .max_racks = 4,
        .max_voices = 8,
        .granularity = 1024,
        .sample_rate = 48000,
        .reserved = 0,
    };
    SceNgsRackDescription master_desc = {
        .voice_definition = sceNgsVoiceDefGetMasterBuss(),
        .voice_count = 1,
        .channels_per_voice = 2,
        .max_patches_per_input = 4,
        .patches_per_output = 4,
        .user_data = NULL,
    };
    SceNgsRackDescription simple_desc = {
        .voice_definition = sceNgsVoiceDefGetSimpleVoice(),
        .voice_count = 1,
        .channels_per_voice = 1,
        .max_patches_per_input = 4,
        .patches_per_output = 4,
        .user_data = NULL,
    };
    SceNgsPatchSetupInfo setup = {
        .source_voice = 0,
        .source_output_index = 0,
        .source_output_sub_index = -1,
        .destination_voice = 0,
        .destination_input_index = 0,
    };
    SceNgsVolumeMatrix identity;
    SceNgsBufferInfo buffer_info;
    SceNgsBufferInfo params_buffer;
    SceNgsHSynSystem system;
    SceNgsHRack master_rack, simple_rack;
    SceNgsHVoice master, simple;
    SceNgsHPatch patch;
    TestPlayerParams *params;
    uint32_t size;
    void *master_mem;
    void *simple_mem;
    void *system_mem;
    uint8_t *samples;
    int16_t state[1024 * 2];
    uint8_t *frame;

    REQUIRE(sceNgsSystemGetRequiredMemorySize(&init, &size) == 0);
    system_mem = malloc(size);
    REQUIRE(system_mem != NULL);
    REQUIRE(sceNgsSystemInit(system_mem, size, &init, &system) == 0);

    REQUIRE(sceNgsRackGetRequiredMemorySize(system, &master_desc, &size) == 0);
    master_mem = malloc(size);
    REQUIRE(master_mem != NULL);
    buffer_info.data = master_mem;
    buffer_info.size = size;
    REQUIRE(sceNgsRackInit(system, &buffer_info, &master_desc, &master_rack) == 0);
    REQUIRE(sceNgsRackGetVoiceHandle(master_rack, 0, &master) == 0);

    REQUIRE(sceNgsRackGetRequiredMemorySize(system, &simple_desc, &size) == 0);
    simple_mem = malloc(size);
    REQUIRE(simple_mem != NULL);
    buffer_info.data = simple_mem;
    buffer_info.size = size;
    REQUIRE(sceNgsRackInit(system, &buffer_info, &simple_desc, &simple_rack) == 0);
    REQUIRE(sceNgsRackGetVoiceHandle(simple_rack, 0, &simple) == 0);

    setup.source_voice = simple;
    setup.destination_voice = master;
    REQUIRE(sceNgsPatchCreateRouting(&setup, &patch) == 0);
    memset(&identity, 0, sizeof(identity));
    identity.matrix[0][0] = 1.0f;
    identity.matrix[1][1] = 1.0f;
    REQUIRE(sceNgsVoicePatchSetVolumesMatrix(patch, &identity) == 0);

    samples = alloc_guest_bytes(64 * 16);
    REQUIRE(samples != NULL);
    memset(samples, 0, 64 * 16);
    frame = samples + 40 * 16;
    frame[0] = 0x08; /* coef 0, shift nibble 8 */
    frame[1] = 0x00; /* flag 0 */
    memset(frame + 2, 0x77, 14);

    REQUIRE(sceNgsVoiceLockParams(simple, 0, 0x01015CE6, &params_buffer) == 0);
    params = params_buffer.data;
    memset(params, 0, sizeof(*params));
    params->id = 0x01015CE6;
    params->size = sizeof(*params);
    params->buffers[0].buffer = (uint32_t)(uintptr_t)samples;
    params->buffers[0].bytes = 64 * 16;
    params->buffers[0].next_buffer = -1;
    params->playback_frequency = 48000.0f;
    params->playback_scalar = 1.0f;
    params->channels = 1;
    params->type = 1; /* HEVAG */
    REQUIRE(sceNgsVoiceUnlockParams(simple, 0) == 0);
    REQUIRE(sceNgsVoicePlay(simple) == 0);

    REQUIRE(sceNgsSystemUpdate(system) == 0);
    REQUIRE(sceNgsVoiceGetStateData(master, 1, state, sizeof(state)) == 0);
    REQUIRE(state[100 * 2] == 0 && state[100 * 2 + 1] == 0);

    REQUIRE(sceNgsSystemUpdate(system) == 0);
    REQUIRE(sceNgsVoiceGetStateData(master, 1, state, sizeof(state)) == 0);
    REQUIRE(state[95 * 2] == 0 && state[96 * 2] == 112 && state[96 * 2 + 1] == 112);
    REQUIRE(state[123 * 2] == 112 && state[124 * 2] == 0);

    REQUIRE(sceNgsRackRelease(simple_rack, NULL) == 0);
    REQUIRE(sceNgsRackRelease(master_rack, NULL) == 0);
    REQUIRE(sceNgsSystemRelease(system) == 0);
    free(system_mem);
    free(master_mem);
    free(simple_mem);
    free_guest_bytes(samples, 64 * 16);
    return 0;
}

int main(void)
{
    SceNgsSystemInitParams params = {
        .max_racks = 4,
        .max_voices = 16,
        .granularity = 256,
        .sample_rate = 48000,
        .reserved = 0,
    };
    SceNgsBufferInfo rack_buffer;
    SceNgsRackDescription rack_desc = {
        .voice_definition = sceNgsVoiceDefGetMasterBuss(),
        .voice_count = 4,
        .channels_per_voice = 2,
        .max_patches_per_input = 4,
        .patches_per_output = 4,
        .user_data = NULL,
    };
    SceNgsVoiceInfo voice_info;
    SceNgsBufferInfo params_buffer;
    SceNgsPatchSetupInfo patch_info;
    SceNgsPatchSetupInfo patch_setup;
    SceNgsPatchRouteInfo route_info;
    SceNgsVolumeMatrix matrix;
    SceNgsModuleParamHeader header = { 0, -1 };
    SceSulphaNgsConfig sulpha_config;
    SceNgsAT9SkipBufferInfo at9_info;
    SceNgsHVoice voices[4];
    SceNgsHPatch patch0, patch1, found;
    SceNgsHRack rack;
    SceNgsHSynSystem system;
    uint32_t size;
    uint32_t flag;
    uint32_t presets;
    uint32_t sulpha_size;
    int32_t errors;
    uint8_t state[64];
    char message[128];
    void *memory;
    int32_t rc;

    REQUIRE(sceNgsVoiceDefGetMasterBuss() != NULL);
    REQUIRE(sceNgsVoiceDefGetSimpleVoice() != NULL);
    REQUIRE(sceNgsVoiceDefGetTemplate1() != NULL);
    REQUIRE(sceNgsVoiceDefGetMasterBuss() != sceNgsVoiceDefGetSimpleVoice());

    REQUIRE(sceNgsSystemGetRequiredMemorySize(&params, &size) == 0);
    REQUIRE(size >= 0x1000);
    memory = malloc(size);
    REQUIRE(memory != NULL);

    REQUIRE(sceNgsSystemInit(memory, size, &params, &system) == 0);
    REQUIRE(system != 0);
    REQUIRE(sceNgsSystemSetFlags(system, 1) == 0);
    REQUIRE(sceNgsSystemLock(system) == 0);
    REQUIRE(sceNgsSystemUnlock(system) == 0);
    REQUIRE(sceNgsSystemSetParamErrorCallback(system, NULL) == 0);
    REQUIRE(sceNgsSystemUpdate(system) == 0);

    REQUIRE(sceNgsModuleGetNumPresets(system, 0, &presets) == 0);
    REQUIRE(presets == 0);
    REQUIRE(sceNgsModuleGetPreset(system, 0, 0, NULL) == SCE_NGS_ERROR_INVALID_PARAM);

    REQUIRE(sceNgsRackGetRequiredMemorySize(system, &rack_desc, &size) == 0);
    REQUIRE(size >= 0x400);
    memory = realloc(memory, size);
    REQUIRE(memory != NULL);
    rack_buffer.data = memory;
    rack_buffer.size = size;

    REQUIRE(sceNgsRackInit(system, &rack_buffer, &rack_desc, &rack) == 0);
    REQUIRE(rack != 0);
    REQUIRE(sceNgsRackSetParamErrorCallback(rack, NULL) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        REQUIRE(sceNgsRackGetVoiceHandle(rack, i, &voices[i]) == 0);
        REQUIRE(voices[i] != 0);
        if (i > 0)
            REQUIRE(voices[i] != voices[i - 1]);
    }
    REQUIRE(sceNgsRackGetVoiceHandle(rack, 4, &found) == SCE_NGS_ERROR_INVALID_PARAM);

    REQUIRE(sceNgsVoiceInit(voices[0], NULL, SCE_NGS_VOICE_INIT_ALL) == 0);
    REQUIRE(sceNgsVoicePlay(voices[0]) == 0);
    REQUIRE(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    REQUIRE(voice_info.voice_state == SCE_NGS_VOICE_STATE_ACTIVE);
    REQUIRE(voice_info.num_outputs == 1);

    REQUIRE(sceNgsVoicePause(voices[0]) == 0);
    REQUIRE(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    REQUIRE(voice_info.voice_state == SCE_NGS_VOICE_STATE_PAUSED);
    REQUIRE(sceNgsVoiceResume(voices[0]) == 0);

    REQUIRE(sceNgsVoiceKeyOff(voices[0]) == 0);
    REQUIRE(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    REQUIRE(voice_info.voice_state == SCE_NGS_VOICE_STATE_KEY_OFF);
    REQUIRE(sceNgsVoiceKill(voices[0]) == 0);
    REQUIRE(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    REQUIRE(voice_info.voice_state == SCE_NGS_VOICE_STATE_AVAILABLE);
    REQUIRE(sceNgsVoicePlay(voices[0]) == 0);

    REQUIRE(sceNgsVoiceBypassModule(voices[0], 0, 1) == 0);
    REQUIRE(sceNgsVoiceGetModuleBypass(voices[0], 0, &flag) == 0);
    REQUIRE(flag == SCE_NGS_MODULE_FLAG_BYPASSED);
    REQUIRE(sceNgsVoiceBypassModule(voices[0], 0, 0) == 0);
    REQUIRE(sceNgsVoiceGetModuleBypass(voices[0], 0, &flag) == 0);
    REQUIRE(flag == SCE_NGS_MODULE_FLAG_NOT_BYPASSED);
    REQUIRE(sceNgsVoiceGetModuleType(voices[0], 0, &flag) == 0);

    REQUIRE(sceNgsVoiceLockParams(voices[0], 0, 0x01015caa, &params_buffer) == 0);
    REQUIRE(params_buffer.data != NULL);
    REQUIRE(params_buffer.size > 0);
    REQUIRE(sceNgsVoiceLockParams(voices[0], 0, 0x01015caa, &params_buffer) ==
            SCE_NGS_ERROR_RESOURCE_LOCKED);
    REQUIRE(sceNgsVoiceUnlockParams(voices[0], 1) == SCE_NGS_ERROR_INVALID_PARAM);
    REQUIRE(sceNgsVoiceUnlockParams(voices[0], 0) == 0);

    memset(state, 0xff, sizeof(state));
    REQUIRE(sceNgsVoiceGetStateData(voices[0], 0, state, sizeof(state)) == 0);
    REQUIRE(state[0] == 0 && state[sizeof(state) - 1] == 0);
    REQUIRE(sceNgsVoiceGetParamsOutOfRange(voices[0], 0, message) == 0);
    REQUIRE(strcmp(message, "No value was out of range\n") == 0);
    REQUIRE(sceNgsVoiceSetParamsBlock(voices[0], &header, sizeof(header), &errors) == 0);
    REQUIRE(errors == 0);
    REQUIRE(sceNgsVoiceSetPreset(voices[0], NULL) == 0);
    REQUIRE(sceNgsVoiceSetFinishedCallback(voices[0], finished_callback, NULL) == 0);
    REQUIRE(sceNgsVoiceSetModuleCallback(voices[0], 0, finished_callback, NULL) == 0);

    patch_info.source_voice = voices[0];
    patch_info.source_output_index = 0;
    patch_info.source_output_sub_index = -1;
    patch_info.destination_voice = voices[1];
    patch_info.destination_input_index = 0;
    REQUIRE(sceNgsPatchCreateRouting(&patch_info, &patch0) == 0);
    REQUIRE(patch0 != 0);

    patch_info.source_output_sub_index = -1;
    REQUIRE(sceNgsPatchCreateRouting(&patch_info, &patch1) == 0);
    REQUIRE(patch1 != 0 && patch1 != patch0);

    REQUIRE(sceNgsVoiceGetOutputPatch(voices[0], 0, 0, &found) == 0);
    REQUIRE(found == patch0);
    REQUIRE(sceNgsVoiceGetOutputPatch(voices[0], 0, 1, &found) == 0);
    REQUIRE(found == patch1);

    memset(&route_info, 0xcc, sizeof(route_info));
    memset(&patch_setup, 0, sizeof(patch_setup));
    REQUIRE(sceNgsPatchGetInfo(patch0, &route_info, &patch_setup) == 0);
    REQUIRE(route_info.output_channels == 2 && route_info.input_channels == 2);
    REQUIRE(patch_setup.source_voice == voices[0]);
    REQUIRE(patch_setup.destination_voice == voices[1]);
    REQUIRE(patch_setup.source_output_sub_index == 0);

    REQUIRE(sceNgsVoicePatchSetVolume(patch0, 0, 0, 0.5f) == 0);
    {
        const float row[2] = { 0.25f, 0.75f };
        REQUIRE(sceNgsVoicePatchSetVolumes(patch0, 1, row, 2) == 0);
    }
    memset(&matrix, 0, sizeof(matrix));
    matrix.matrix[0][0] = 0.125f;
    matrix.matrix[0][1] = 0.25f;
    matrix.matrix[1][0] = 0.5f;
    matrix.matrix[1][1] = 0.75f;
    REQUIRE(sceNgsVoicePatchSetVolumesMatrix(patch0, &matrix) == 0);
    REQUIRE(sceNgsPatchGetInfo(patch0, &route_info, NULL) == 0);
    REQUIRE(route_info.volume_matrix.matrix[0][0] == 0.125f);
    REQUIRE(route_info.volume_matrix.matrix[1][1] == 0.75f);
    REQUIRE(sceNgsVoicePatchSetVolume(patch0, 2, 0, 1.0f) == SCE_NGS_ERROR_INVALID_PARAM);

    REQUIRE(sceNgsPatchRemoveRouting(patch0) == 0);
    REQUIRE(sceNgsPatchRemoveRouting(patch1) == 0);
    REQUIRE(sceNgsVoiceGetOutputPatch(voices[0], 0, 0, &found) == 0);
    REQUIRE(found == 0);

    /* config: 0xFE magic, sample-rate code 4 (128 samples/frame), 16 -> 129 bytes. */
    REQUIRE(sceNgsAT9GetSectionDetails(0, 128, 0x001040fe, &at9_info) == 0);
    REQUIRE(at9_info.start_byte_offset == 0);
    REQUIRE(at9_info.start_skip == 128);
    REQUIRE(at9_info.num_bytes == 258);
    REQUIRE(at9_info.end_skip == 0);
    REQUIRE(sceNgsAT9GetSectionDetails(0, -1, 0x001040fe, &at9_info) == SCE_NGS_ERROR);
    REQUIRE(sceNgsAT9GetSectionDetails(0, 128, 0x001040ff, &at9_info) == SCE_NGS_ERROR);

    REQUIRE(sceSulphaNgsGetDefaultConfig(&sulpha_config) == 0);
    REQUIRE(sulpha_config.max_named_samples == 1024);
    REQUIRE(sulpha_config.api_trace_buffer_size == 4096);
    REQUIRE(sceSulphaNgsGetNeededMemory(&sulpha_config, &sulpha_size) == 0);
    REQUIRE(sulpha_size >= 4096);
    memory = malloc(sulpha_size);
    REQUIRE(memory != NULL);
    REQUIRE(sceSulphaNgsInit(&sulpha_config, memory, sulpha_size) == 0);
    REQUIRE(sceSulphaNgsSetRackName(rack, "rack") == 0);
    REQUIRE(sceSulphaNgsSetSynthName(system, "synth") == 0);
    REQUIRE(sceSulphaNgsSetVoiceName(voices[0], "voice") == 0);
    REQUIRE(sceSulphaNgsSetSampleName(memory, 16, "sample") == 0);
    REQUIRE(sceSulphaNgsTrace("trace") == 0);
    REQUIRE(sceSulphaNgsShutdown() == 0);

    REQUIRE(sceNgsRackRelease(rack, NULL) == 0);
    rc = sceNgsVoicePlay(voices[0]);
    REQUIRE(rc == SCE_NGS_ERROR_INVALID_HANDLE);
    REQUIRE(sceNgsSystemRelease(system) == 0);
    rc = sceNgsSystemUpdate(system);
    REQUIRE(rc == SCE_NGS_ERROR_INVALID_HANDLE);
    REQUIRE(sceNgsVoicePlay((SceNgsHVoice)0x12345678u) == SCE_NGS_ERROR_INVALID_HANDLE);

    REQUIRE(run_hevag_render_test() == 0);

    puts("ngs_smoke: ok");
    return 0;
}
