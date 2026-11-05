/*
 * Exercises the complete SceNgs bookkeeping surface: system, racks, voices,
 * patches, parameter locks and the ATRAC9 section helper.
 */

#include "ngs_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("ngs_smoke: failed line %d: %s\n", __LINE__, #cond);                            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void finished_callback(const SceNgsCallbackInfo *info)
{
    (void)info;
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

    CHECK(sceNgsVoiceDefGetMasterBuss() != NULL);
    CHECK(sceNgsVoiceDefGetSimpleVoice() != NULL);
    CHECK(sceNgsVoiceDefGetTemplate1() != NULL);
    CHECK(sceNgsVoiceDefGetMasterBuss() != sceNgsVoiceDefGetSimpleVoice());

    CHECK(sceNgsSystemGetRequiredMemorySize(&params, &size) == 0);
    CHECK(size >= 0x1000);
    memory = malloc(size);
    CHECK(memory != NULL);

    CHECK(sceNgsSystemInit(memory, size, &params, &system) == 0);
    CHECK(system != 0);
    CHECK(sceNgsSystemSetFlags(system, 1) == 0);
    CHECK(sceNgsSystemLock(system) == 0);
    CHECK(sceNgsSystemUnlock(system) == 0);
    CHECK(sceNgsSystemSetParamErrorCallback(system, NULL) == 0);
    CHECK(sceNgsSystemUpdate(system) == 0);

    CHECK(sceNgsModuleGetNumPresets(system, 0, &presets) == 0);
    CHECK(presets == 0);
    CHECK(sceNgsModuleGetPreset(system, 0, 0, NULL) == SCE_NGS_ERROR_INVALID_PARAM);

    CHECK(sceNgsRackGetRequiredMemorySize(system, &rack_desc, &size) == 0);
    CHECK(size >= 0x400);
    memory = realloc(memory, size);
    CHECK(memory != NULL);
    rack_buffer.data = memory;
    rack_buffer.size = size;

    CHECK(sceNgsRackInit(system, &rack_buffer, &rack_desc, &rack) == 0);
    CHECK(rack != 0);
    CHECK(sceNgsRackSetParamErrorCallback(rack, NULL) == 0);

    for (uint32_t i = 0; i < 4; i++) {
        CHECK(sceNgsRackGetVoiceHandle(rack, i, &voices[i]) == 0);
        CHECK(voices[i] != 0);
        if (i > 0)
            CHECK(voices[i] != voices[i - 1]);
    }
    CHECK(sceNgsRackGetVoiceHandle(rack, 4, &found) == SCE_NGS_ERROR_INVALID_PARAM);

    CHECK(sceNgsVoiceInit(voices[0], NULL, SCE_NGS_VOICE_INIT_ALL) == 0);
    CHECK(sceNgsVoicePlay(voices[0]) == 0);
    CHECK(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    CHECK(voice_info.voice_state == SCE_NGS_VOICE_STATE_ACTIVE);
    CHECK(voice_info.num_outputs == 1);

    CHECK(sceNgsVoicePause(voices[0]) == 0);
    CHECK(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    CHECK(voice_info.voice_state == SCE_NGS_VOICE_STATE_PAUSED);
    CHECK(sceNgsVoiceResume(voices[0]) == 0);

    CHECK(sceNgsVoiceKeyOff(voices[0]) == 0);
    CHECK(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    CHECK(voice_info.voice_state == SCE_NGS_VOICE_STATE_KEY_OFF);
    CHECK(sceNgsVoiceKill(voices[0]) == 0);
    CHECK(sceNgsVoiceGetInfo(voices[0], &voice_info) == 0);
    CHECK(voice_info.voice_state == SCE_NGS_VOICE_STATE_AVAILABLE);
    CHECK(sceNgsVoicePlay(voices[0]) == 0);

    CHECK(sceNgsVoiceBypassModule(voices[0], 0, 1) == 0);
    CHECK(sceNgsVoiceGetModuleBypass(voices[0], 0, &flag) == 0);
    CHECK(flag == SCE_NGS_MODULE_FLAG_BYPASSED);
    CHECK(sceNgsVoiceBypassModule(voices[0], 0, 0) == 0);
    CHECK(sceNgsVoiceGetModuleBypass(voices[0], 0, &flag) == 0);
    CHECK(flag == SCE_NGS_MODULE_FLAG_NOT_BYPASSED);
    CHECK(sceNgsVoiceGetModuleType(voices[0], 0, &flag) == 0);

    CHECK(sceNgsVoiceLockParams(voices[0], 0, 0x01015caa, &params_buffer) == 0);
    CHECK(params_buffer.data != NULL);
    CHECK(params_buffer.size > 0);
    CHECK(sceNgsVoiceLockParams(voices[0], 0, 0x01015caa, &params_buffer) ==
          SCE_NGS_ERROR_RESOURCE_LOCKED);
    CHECK(sceNgsVoiceUnlockParams(voices[0], 1) == SCE_NGS_ERROR_INVALID_PARAM);
    CHECK(sceNgsVoiceUnlockParams(voices[0], 0) == 0);

    memset(state, 0xff, sizeof(state));
    CHECK(sceNgsVoiceGetStateData(voices[0], 0, state, sizeof(state)) == 0);
    CHECK(state[0] == 0 && state[sizeof(state) - 1] == 0);
    CHECK(sceNgsVoiceGetParamsOutOfRange(voices[0], 0, message) == 0);
    CHECK(strcmp(message, "No value was out of range\n") == 0);
    CHECK(sceNgsVoiceSetParamsBlock(voices[0], &header, sizeof(header), &errors) == 0);
    CHECK(errors == 0);
    CHECK(sceNgsVoiceSetPreset(voices[0], NULL) == 0);
    CHECK(sceNgsVoiceSetFinishedCallback(voices[0], finished_callback, NULL) == 0);
    CHECK(sceNgsVoiceSetModuleCallback(voices[0], 0, finished_callback, NULL) == 0);

    patch_info.source_voice = voices[0];
    patch_info.source_output_index = 0;
    patch_info.source_output_sub_index = -1;
    patch_info.destination_voice = voices[1];
    patch_info.destination_input_index = 0;
    CHECK(sceNgsPatchCreateRouting(&patch_info, &patch0) == 0);
    CHECK(patch0 != 0);

    patch_info.source_output_sub_index = -1;
    CHECK(sceNgsPatchCreateRouting(&patch_info, &patch1) == 0);
    CHECK(patch1 != 0 && patch1 != patch0);

    CHECK(sceNgsVoiceGetOutputPatch(voices[0], 0, 0, &found) == 0);
    CHECK(found == patch0);
    CHECK(sceNgsVoiceGetOutputPatch(voices[0], 0, 1, &found) == 0);
    CHECK(found == patch1);

    memset(&route_info, 0xcc, sizeof(route_info));
    memset(&patch_setup, 0, sizeof(patch_setup));
    CHECK(sceNgsPatchGetInfo(patch0, &route_info, &patch_setup) == 0);
    CHECK(route_info.output_channels == 2 && route_info.input_channels == 2);
    CHECK(patch_setup.source_voice == voices[0]);
    CHECK(patch_setup.destination_voice == voices[1]);
    CHECK(patch_setup.source_output_sub_index == 0);

    CHECK(sceNgsVoicePatchSetVolume(patch0, 0, 0, 0.5f) == 0);
    {
        const float row[2] = { 0.25f, 0.75f };
        CHECK(sceNgsVoicePatchSetVolumes(patch0, 1, row, 2) == 0);
    }
    memset(&matrix, 0, sizeof(matrix));
    matrix.matrix[0][0] = 0.125f;
    matrix.matrix[0][1] = 0.25f;
    matrix.matrix[1][0] = 0.5f;
    matrix.matrix[1][1] = 0.75f;
    CHECK(sceNgsVoicePatchSetVolumesMatrix(patch0, &matrix) == 0);
    CHECK(sceNgsPatchGetInfo(patch0, &route_info, NULL) == 0);
    CHECK(route_info.volume_matrix.matrix[0][0] == 0.125f);
    CHECK(route_info.volume_matrix.matrix[1][1] == 0.75f);
    CHECK(sceNgsVoicePatchSetVolume(patch0, 2, 0, 1.0f) == SCE_NGS_ERROR_INVALID_PARAM);

    CHECK(sceNgsPatchRemoveRouting(patch0) == 0);
    CHECK(sceNgsPatchRemoveRouting(patch1) == 0);
    CHECK(sceNgsVoiceGetOutputPatch(voices[0], 0, 0, &found) == 0);
    CHECK(found == 0);

    /* config: 0xFE magic, sample-rate code 4 (128 samples/frame), 16 -> 129 bytes. */
    CHECK(sceNgsAT9GetSectionDetails(0, 128, 0x001040fe, &at9_info) == 0);
    CHECK(at9_info.start_byte_offset == 0);
    CHECK(at9_info.start_skip == 128);
    CHECK(at9_info.num_bytes == 258);
    CHECK(at9_info.end_skip == 0);
    CHECK(sceNgsAT9GetSectionDetails(0, -1, 0x001040fe, &at9_info) == SCE_NGS_ERROR);
    CHECK(sceNgsAT9GetSectionDetails(0, 128, 0x001040ff, &at9_info) == SCE_NGS_ERROR);

    CHECK(sceSulphaNgsGetDefaultConfig(&sulpha_config) == 0);
    CHECK(sulpha_config.max_named_samples == 1024);
    CHECK(sulpha_config.api_trace_buffer_size == 4096);
    CHECK(sceSulphaNgsGetNeededMemory(&sulpha_config, &sulpha_size) == 0);
    CHECK(sulpha_size >= 4096);
    memory = malloc(sulpha_size);
    CHECK(memory != NULL);
    CHECK(sceSulphaNgsInit(&sulpha_config, memory, sulpha_size) == 0);
    CHECK(sceSulphaNgsSetRackName(rack, "rack") == 0);
    CHECK(sceSulphaNgsSetSynthName(system, "synth") == 0);
    CHECK(sceSulphaNgsSetVoiceName(voices[0], "voice") == 0);
    CHECK(sceSulphaNgsSetSampleName(memory, 16, "sample") == 0);
    CHECK(sceSulphaNgsTrace("trace") == 0);
    CHECK(sceSulphaNgsShutdown() == 0);

    CHECK(sceNgsRackRelease(rack, NULL) == 0);
    rc = sceNgsVoicePlay(voices[0]);
    CHECK(rc == SCE_NGS_ERROR_INVALID_HANDLE);
    CHECK(sceNgsSystemRelease(system) == 0);
    rc = sceNgsSystemUpdate(system);
    CHECK(rc == SCE_NGS_ERROR_INVALID_HANDLE);
    CHECK(sceNgsVoicePlay((SceNgsHVoice)0x12345678u) == SCE_NGS_ERROR_INVALID_HANDLE);

    puts("ngs_smoke: ok");
    return 0;
}
