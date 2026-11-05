/*
 * SceAudiodec: audio decoding. ATRAC9 is the format Vita titles use for
 * music and voice, and Vita3K ships a portable C decoder for it
 * (external/Vita3K/external/LibAtrac9), so this is a real implementation
 * rather than a stub.
 *
 * The API hands the decoder its configuration through `pInfo` and then
 * exchanges one elementary-stream frame for PCM per call.
 */

#include <m-dict.h>
#include <psp2/audiodec.h>
#include <stdlib.h>
#include <string.h>

#include "libatrac9.h"
#include "log.h"
#include "module.h"
#include "modules/SceLibKernel.h"

#define AUDIODEC_MAX_HANDLES 64

typedef struct {
    SceUID uid;
    SceAudiodecType type;
    void *at9;
    Atrac9CodecInfo codec;
    uint32_t frame_samples;
} AudioDecoder;

DICT_DEF2(vita_audiodec_dict, SceUID, M_DEFAULT_OPLIST, AudioDecoder *, M_POD_OPLIST)
static vita_audiodec_dict_t g_decoders;
static bool g_at9_initialized;
static uint32_t g_at9_channels;
static SceUID g_next_handle;

static AudioDecoder *audiodec_lookup(SceUID uid)
{
    return *vita_audiodec_dict_get(g_decoders, uid);
}

EXPORT(SceAudiodecUser, 0x445C2CEF, int, sceAudiodecInitLibrary, SceUInt32 codec_type,
       SceAudiodecInitParam *init_param)
{
    if (!init_param)
        return SCE_AUDIODEC_ERROR_INVALID_INIT_PARAM;

    switch (codec_type) {
    case SCE_AUDIODEC_TYPE_AT9:
        if (g_at9_initialized)
            return SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED;
        if (init_param->at9.totalCh == 0 || init_param->at9.totalCh > SCE_AUDIODEC_AT9_MAX_CH_IN_LIBRARY)
            return SCE_AUDIODEC_ERROR_INVALID_INIT_PARAM;
        g_at9_channels = init_param->at9.totalCh;
        g_at9_initialized = true;
        return 0;
    case SCE_AUDIODEC_TYPE_MP3:
    case SCE_AUDIODEC_TYPE_AAC:
    case SCE_AUDIODEC_TYPE_CELP:
        /* Only ATRAC9 has a decoder on this host. */
        return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    default:
        return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    }
}

EXPORT(SceAudiodecUser, 0x45719B9D, int, sceAudiodecTermLibrary, SceUInt32 codec_type)
{
    (void)codec_type;

    g_at9_initialized = false;
    return 0;
}

EXPORT(SceAudiodecUser, 0xDB712ABC, int, sceAudiodecGetContextSize, SceAudiodecCtrl *ctrl,
       SceUInt32 codec_type)
{
    (void)codec_type;

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    /* Everything lives on the host; the guest work area can stay small. */
    ctrl->size = sizeof(*ctrl);
    return 0;
}

static uint32_t at9_bitrate(const Atrac9CodecInfo *info)
{
    uint64_t samples = (uint64_t)info->frameSamples * (uint64_t)info->framesInSuperframe;

    if (samples == 0)
        return 0;

    return (uint32_t)(((uint64_t)info->superframeSize * 8ull * (uint64_t)info->samplingRate) /
                      samples);
}

static int create_at9_decoder(SceAudiodecCtrl *ctrl)
{
    AudioDecoder *decoder;
    unsigned char config[ATRAC9_CONFIG_DATA_SIZE];
    SceUID uid;

    if (!g_at9_initialized)
        return SCE_AUDIODEC_ERROR_NOT_INITIALIZED;
    if (!ctrl || !ctrl->pInfo)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    memcpy(config, ctrl->pInfo->at9.configData, sizeof(config));

    decoder = calloc(1, sizeof(*decoder));
    if (!decoder)
        return SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;

    decoder->at9 = Atrac9GetHandle();
    if (!decoder->at9) {
        free(decoder);
        return SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
    }

    if (Atrac9InitDecoder(decoder->at9, config) != 0 ||
        Atrac9GetCodecInfo(decoder->at9, &decoder->codec) != 0) {
        Atrac9ReleaseHandle(decoder->at9);
        free(decoder);
        return SCE_AUDIODEC_ERROR_INVALID_INIT_PARAM;
    }

    decoder->type = SCE_AUDIODEC_TYPE_AT9;
    decoder->frame_samples = (uint32_t)decoder->codec.frameSamples;
    decoder->uid = ++g_next_handle;

    ctrl->pInfo->at9.ch = (SceUInt32)decoder->codec.channels;
    ctrl->pInfo->at9.bitRate = at9_bitrate(&decoder->codec);
    ctrl->pInfo->at9.samplingRate = (SceUInt32)decoder->codec.samplingRate;
    ctrl->pInfo->at9.superFrameSize = (SceUInt32)decoder->codec.superframeSize;
    ctrl->pInfo->at9.framesInSuperFrame = (SceUInt32)decoder->codec.framesInSuperframe;

    ctrl->maxEsSize = (SceUInt32)decoder->codec.superframeSize;
    ctrl->maxPcmSize = decoder->frame_samples * (SceUInt32)decoder->codec.channels *
                       (SceUInt32)sizeof(int16_t);
    ctrl->wordLength = 16;

    uid = decoder->uid;
    vita_audiodec_dict_set_at(g_decoders, uid, decoder);
    ctrl->handle = (SceInt32)uid;

    LOG("SceAudiodec: ATRAC9 decoder: %d ch, %d Hz, %d samples/frame, superframe %d/%d",
        decoder->codec.channels, decoder->codec.samplingRate, decoder->codec.frameSamples,
        decoder->codec.framesInSuperframe, decoder->codec.superframeSize);

    return 0;
}

EXPORT(SceAudiodecUser, 0x4DFD3AAA, int, sceAudiodecCreateDecoder, SceAudiodecCtrl *ctrl,
       SceUInt32 codec_type)
{
    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    switch (codec_type) {
    case SCE_AUDIODEC_TYPE_AT9:
        return create_at9_decoder(ctrl);
    default:
        return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    }
}

EXPORT(SceAudiodecUser, 0x56085DFB, int, sceAudiodecCreateDecoderExternal, SceAudiodecCtrl *ctrl,
       SceUInt32 codec_type, SceUInt32 context_addr, SceUInt32 context_size)
{
    (void)context_addr;
    (void)context_size;

    return sceAudiodecCreateDecoder(ctrl, codec_type);
}

EXPORT(SceAudiodecUser, 0x98B44E1B, int, sceAudiodecCreateDecoderResident, SceAudiodecCtrl *ctrl,
       SceUInt32 codec_type)
{
    return sceAudiodecCreateDecoder(ctrl, codec_type);
}

EXPORT(SceAudiodecUser, 0xE7A24E16, int, sceAudiodecDeleteDecoder, SceAudiodecCtrl *ctrl)
{
    AudioDecoder *decoder;

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    decoder = audiodec_lookup((SceUID)ctrl->handle);
    if (!decoder)
        return SCE_AUDIODEC_ERROR_INVALID_HANDLE;

    vita_audiodec_dict_erase(g_decoders, (SceUID)ctrl->handle);
    Atrac9ReleaseHandle(decoder->at9);
    free(decoder);
    ctrl->handle = 0;

    return 0;
}

EXPORT(SceAudiodecUser, 0xE4EA05BB, int, sceAudiodecDeleteDecoderExternal, SceAudiodecCtrl *ctrl,
       SceUInt32 *context_addr)
{
    if (context_addr)
        *context_addr = 0;

    return sceAudiodecDeleteDecoder(ctrl);
}

EXPORT(SceAudiodecUser, 0x164DAEDA, int, sceAudiodecDeleteDecoderResident,
       SceAudiodecCtrl *ctrl)
{
    return sceAudiodecDeleteDecoder(ctrl);
}

EXPORT(SceAudiodecUser, 0xF72F9B64, int, sceAudiodecClearContext, SceAudiodecCtrl *ctrl)
{
    AudioDecoder *decoder;

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    decoder = audiodec_lookup((SceUID)ctrl->handle);
    if (!decoder)
        return SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE;

    /* LibAtrac9 keeps no cross-frame state we can reset without rebuilding. */
    return 0;
}

static int audiodec_decode_frames(SceAudiodecCtrl *ctrl, SceUInt32 frame_count)
{
    AudioDecoder *decoder;
    uint8_t *es;
    uint8_t *pcm;

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (!ctrl->pEs || !ctrl->pPcm)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    decoder = audiodec_lookup((SceUID)ctrl->handle);
    if (!decoder)
        return SCE_AUDIODEC_ERROR_INVALID_HANDLE;

    ctrl->inputEsSize = 0;
    ctrl->outputPcmSize = 0;

    es = ctrl->pEs;
    pcm = ctrl->pPcm;

    for (SceUInt32 i = 0; i < frame_count; i++) {
        int bytes_used = 0;
        int result;

        if (decoder->type != SCE_AUDIODEC_TYPE_AT9)
            return SCE_AUDIODEC_ERROR_INVALID_TYPE;

        result = Atrac9Decode(decoder->at9, es, (short *)pcm, &bytes_used);
        if (result != 0)
            return SCE_AUDIODEC_ERROR_API_FAIL;

        ctrl->inputEsSize += (SceUInt32)bytes_used;
        ctrl->outputPcmSize += decoder->frame_samples *
                               (SceUInt32)decoder->codec.channels * (SceUInt32)sizeof(int16_t);

        es += bytes_used;
        pcm += decoder->frame_samples * (SceUInt32)decoder->codec.channels *
               (SceUInt32)sizeof(int16_t);
    }

    return 0;
}

EXPORT(SceAudiodecUser, 0xCCDABA04, int, sceAudiodecDecode, SceAudiodecCtrl *ctrl)
{
    return audiodec_decode_frames(ctrl, 1);
}

EXPORT(SceAudiodecUser, 0x8018AA9B, int, sceAudiodecDecodeNFrames, SceAudiodecCtrl *ctrl,
       SceUInt32 frame_count)
{
    return audiodec_decode_frames(ctrl, frame_count);
}

EXPORT(SceAudiodecUser, 0x68F4A9CB, int, sceAudiodecDecodeNStreams,
       SceAudiodecCtrl *ctrls[], SceUInt32 stream_count)
{
    int result = 0;

    if (!ctrls)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    for (SceUInt32 i = 0; i < stream_count; i++) {
        result = audiodec_decode_frames(ctrls[i], 1);
        if (result != 0)
            return result;
    }

    return 0;
}

EXPORT(SceAudiodecUser, 0x2742E642, int, sceAudiodecPartlyDecode, SceAudiodecCtrl *ctrl,
       SceUInt32 sample_count)
{
    (void)sample_count;

    return sceAudiodecDecode(ctrl);
}

EXPORT(SceAudiodecUser, 0x883B0CF5, int, sceAudiodecGetInternalError, SceAudiodecCtrl *ctrl,
       SceInt32 *internal_error)
{
    (void)ctrl;

    if (internal_error)
        *internal_error = 0;

    return 0;
}

DECLARE_LIBRARY(SceAudiodecUser, 0x2aa62046);

int SceAudiodec_init(void)
{
    vita_audiodec_dict_init(g_decoders);
    g_at9_initialized = false;
    g_at9_channels = 0;
    g_next_handle = 0;
    return 0;
}

int SceAudiodec_finish(void)
{
    vita_audiodec_dict_clear(g_decoders);
    return 0;
}
