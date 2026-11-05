/*
 * SceAtrac: ATRAC9 streaming playback.
 *
 * Titles hand over a guest buffer holding the first part of an .at9 file and
 * then stream the rest in with sceAtracAddStreamData. This implementation
 * locates the codec configuration and the data chunk inside the RIFF
 * container, then decodes frames straight out of the guest buffer with
 * LibAtrac9.
 */

#include <m-dict.h>
#include <psp2/atrac.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "libatrac9.h"
#include "log.h"
#include "module.h"
#include "modules/SceLibKernel.h"

#define ATRAC_MAX_HANDLES 16
#define ATRAC_ALIGNMENT   0x100u

typedef struct {
    SceUID uid;
    uint8_t *buffer;
    uint32_t buffer_size;
    uint32_t read_size;     /* bytes valid in `buffer` */
    uint32_t read_position; /* bytes already consumed */
    uint32_t data_offset;   /* offset of the first ES byte */
    uint32_t data_size;     /* ES bytes, 0 when unknown */
    uint8_t config[4];
    void *at9;
    Atrac9CodecInfo codec;
    uint32_t frame_samples;
    uint32_t bytes_per_frame;
    uint32_t frames_decoded;
    uint32_t total_frames; /* 0 when unknown */
    int loop_count;
    int loops_done;
    uint32_t next_output_sample;
    uint32_t output_samples;
} AtracStream;

DICT_DEF2(vita_atrac_dict, SceUID, M_DEFAULT_OPLIST, AtracStream *, M_POD_OPLIST)
static vita_atrac_dict_t g_streams;
static SceUID g_next_handle;
static bool g_decoder_group_created;
static uint32_t g_group_channels;

static AtracStream *atrac_lookup(int handle)
{
    return *vita_atrac_dict_get(g_streams, (SceUID)handle);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Returns true when `config` initialises a decoder. */
static bool config_is_valid(const uint8_t *config)
{
    void *handle = Atrac9GetHandle();
    bool ok;

    if (!handle)
        return false;

    ok = Atrac9InitDecoder(handle, (unsigned char *)config) == 0;
    Atrac9ReleaseHandle(handle);

    return ok;
}

/*
 * Finds the 4-byte AT9 configuration. Containers in the wild put it at
 * different offsets, so candidates are validated by handing them to the
 * decoder rather than trusting a fixed layout.
 */
static bool find_config(const uint8_t *buffer, uint32_t size, uint8_t out[4])
{
    static const int fmt_offsets[] = { 0x14, 0x18, 0x1C, 0x20, 0x24 };
    static const int fact_offsets[] = { 0x08, 0x04, 0x0C };

    if (size < 4)
        return false;

    if (size >= 12 && memcmp(buffer, "RIFF", 4) == 0 && memcmp(buffer + 8, "WAVE", 4) == 0) {
        uint32_t offset = 12;

        while (offset + 8 <= size) {
            uint32_t chunk_size = read_le32(buffer + offset + 4);
            const uint8_t *payload = buffer + offset + 8;
            uint32_t available = size - offset - 8;

            if (chunk_size > available)
                chunk_size = available;

            if (memcmp(buffer + offset, "fmt ", 4) == 0) {
                for (size_t i = 0; i < sizeof(fmt_offsets) / sizeof(fmt_offsets[0]); i++) {
                    int candidate = fmt_offsets[i];

                    if (candidate + 4 > (int)chunk_size)
                        continue;
                    if (config_is_valid(payload + candidate)) {
                        memcpy(out, payload + candidate, 4);
                        return true;
                    }
                }
                /* Older files put the configuration at the chunk start. */
                if (chunk_size >= 4 && config_is_valid(payload)) {
                    memcpy(out, payload, 4);
                    return true;
                }
            } else if (memcmp(buffer + offset, "fact", 4) == 0) {
                for (size_t i = 0; i < sizeof(fact_offsets) / sizeof(fact_offsets[0]); i++) {
                    int candidate = fact_offsets[i];

                    if (candidate + 4 > (int)chunk_size)
                        continue;
                    if (config_is_valid(payload + candidate)) {
                        memcpy(out, payload + candidate, 4);
                        return true;
                    }
                }
            }

            offset += 8 + chunk_size + (chunk_size & 1u);
            if (chunk_size == 0)
                break;
        }
    }

    /* Raw ES files and unusual containers: scan for a plausible config. */
    for (uint32_t offset = 0; offset + 4 <= size && offset < 512; offset += 4) {
        if (buffer[offset] != 0xFE)
            continue;
        if (config_is_valid(buffer + offset)) {
            memcpy(out, buffer + offset, 4);
            return true;
        }
    }

    return false;
}

/* Offset of the first elementary-stream byte, i.e. the `data` chunk payload. */
static uint32_t find_data_offset(const uint8_t *buffer, uint32_t size, uint32_t config_offset)
{
    if (size >= 12 && memcmp(buffer, "RIFF", 4) == 0 && memcmp(buffer + 8, "WAVE", 4) == 0) {
        uint32_t offset = 12;

        while (offset + 8 <= size) {
            uint32_t chunk_size = read_le32(buffer + offset + 4);
            uint32_t available = size - offset - 8;

            if (chunk_size > available)
                chunk_size = available;

            if (memcmp(buffer + offset, "data", 4) == 0)
                return offset + 8;

            offset += 8 + chunk_size + (chunk_size & 1u);
            if (chunk_size == 0)
                break;
        }
    }

    return config_offset + 4;
}

EXPORT(SceAtrac, 0x1A34B331, int, sceAtracQueryDecoderGroupMemSize, SceUInt32 atrac_type,
       const SceAtracDecoderGroup *group)
{
    (void)atrac_type;
    (void)group;

    /* The work area is only a token here; the decoders live on the host. */
    return 0x1000;
}

EXPORT(SceAtrac, 0x01BFB98C, int, sceAtracCreateDecoderGroup, SceUInt32 atrac_type,
       const SceAtracDecoderGroup *group, void *work_mem, int init_audiodec)
{
    (void)work_mem;
    (void)init_audiodec;

    if (atrac_type != SCE_ATRAC_TYPE_AT9)
        return SCE_ATRAC_ERROR_INVALID_TYPE;
    if (!group || group->totalCh == 0 || group->totalCh > SCE_ATRAC_AT9_MAX_TOTAL_CH)
        return SCE_ATRAC_ERROR_INVALID_TOTAL_CH;
    if (g_decoder_group_created)
        return SCE_ATRAC_ERROR_ALREADY_CREATED;

    g_decoder_group_created = true;
    g_group_channels = group->totalCh;

    return 0;
}

EXPORT(SceAtrac, 0x6CA2F08A, int, sceAtracDeleteDecoderGroup, SceUInt32 atrac_type,
       int term_audiodec)
{
    (void)atrac_type;
    (void)term_audiodec;

    g_decoder_group_created = false;
    return 0;
}

EXPORT(SceAtrac, 0xBF34605D, int, sceAtracGetDecoderGroupInfo, SceUInt32 atrac_type,
       SceAtracDecoderGroup *created, SceAtracDecoderGroup *available)
{
    if (atrac_type != SCE_ATRAC_TYPE_AT9)
        return SCE_ATRAC_ERROR_INVALID_TYPE;

    if (created) {
        memset(created, 0, sizeof(*created));
        created->size = sizeof(*created);
        created->wordLength = 16;
        created->totalCh = g_decoder_group_created ? g_group_channels : 0;
    }
    if (available) {
        memset(available, 0, sizeof(*available));
        available->size = sizeof(*available);
        available->wordLength = 16;
        available->totalCh = SCE_ATRAC_AT9_MAX_TOTAL_CH;
    }

    return 0;
}

EXPORT(SceAtrac, 0xD1997678, int, sceAtracSetDataAndAcquireHandle, SceUChar8 *buffer,
       SceUInt32 read_size, SceUInt32 buffer_size)
{
    AtracStream *stream;
    uint8_t config[4];
    uint32_t config_offset = 0;
    SceUID uid;

    if (!buffer)
        return SCE_ATRAC_ERROR_INVALID_POINTER;
    if (((uintptr_t)buffer & (ATRAC_ALIGNMENT - 1)) != 0 ||
        (buffer_size & (ATRAC_ALIGNMENT - 1)) != 0)
        return SCE_ATRAC_ERROR_INVALID_ALIGNMENT;
    if (buffer_size < read_size)
        return SCE_ATRAC_ERROR_READ_SIZE_OVER_BUFFER;
    if (read_size < 4)
        return SCE_ATRAC_ERROR_READ_SIZE_IS_TOO_SMALL;

    if (!find_config(buffer, read_size, config))
        return SCE_ATRAC_ERROR_UNSUPPORTED_DATA;

    /* Locate where the configuration sits so the data offset can skip it. */
    {
        uint32_t offset;

        for (offset = 0; offset + 4 <= read_size && offset < 512; offset += 4) {
            if (memcmp(buffer + offset, config, 4) == 0) {
                config_offset = offset;
                break;
            }
        }
    }

    stream = calloc(1, sizeof(*stream));
    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;

    stream->at9 = Atrac9GetHandle();
    if (!stream->at9 || Atrac9InitDecoder(stream->at9, config) != 0 ||
        Atrac9GetCodecInfo(stream->at9, &stream->codec) != 0) {
        Atrac9ReleaseHandle(stream->at9);
        free(stream);
        return SCE_ATRAC_ERROR_UNSUPPORTED_DATA;
    }

    memcpy(stream->config, config, sizeof(config));
    stream->frame_samples = (uint32_t)stream->codec.frameSamples;
    stream->bytes_per_frame = (uint32_t)stream->codec.superframeSize /
                              (uint32_t)stream->codec.framesInSuperframe;

    stream->buffer = buffer;
    stream->buffer_size = buffer_size;
    stream->read_size = read_size;
    stream->data_offset = find_data_offset(buffer, read_size, config_offset);
    if (stream->read_size > stream->data_offset)
        stream->data_size = stream->read_size - stream->data_offset;
    stream->read_position = stream->data_offset;
    stream->loop_count = 0;
    stream->output_samples = SCE_ATRAC_MAX_OUTPUT_SAMPLES;

    stream->uid = ++g_next_handle;
    uid = stream->uid;
    vita_atrac_dict_set_at(g_streams, uid, stream);

    LOG("SceAtrac: handle %d: %d ch, %d Hz, %d samples/frame, data at 0x%x",
        (int)uid, stream->codec.channels, stream->codec.samplingRate, stream->frame_samples,
        stream->data_offset);

    return (int)uid;
}

EXPORT(SceAtrac, 0x008EF251, int, sceAtracReleaseHandle, int handle)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;

    vita_atrac_dict_erase(g_streams, (SceUID)handle);
    Atrac9ReleaseHandle(stream->at9);
    free(stream);

    return 0;
}

EXPORT(SceAtrac, 0x53656F1C, int, sceAtracAddStreamData, int handle, SceUInt32 add_size)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (add_size == 0)
        return 0;
    if (stream->read_size + add_size > stream->buffer_size)
        return SCE_ATRAC_ERROR_ADDED_DATA_IS_TOO_BIG;

    stream->read_size += add_size;
    stream->data_size = stream->read_size - stream->data_offset;

    return 0;
}

EXPORT(SceAtrac, 0x29C74EE3, int, sceAtracDecode, int handle, void *output_buffer,
       SceUInt32 *output_samples, SceUInt32 *decoder_status)
{
    AtracStream *stream = atrac_lookup(handle);
    uint8_t *pcm = output_buffer;
    uint32_t decoded = 0;
    uint32_t channels;
    uint32_t pcm_frame_bytes;

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!output_buffer)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    channels = (uint32_t)stream->codec.channels;
    pcm_frame_bytes = stream->frame_samples * channels * (uint32_t)sizeof(int16_t);

    while (decoded + stream->frame_samples <= stream->output_samples &&
           stream->read_position + stream->bytes_per_frame <= stream->read_size) {
        int bytes_used = 0;

        if (Atrac9Decode(stream->at9, stream->buffer + stream->read_position, (short *)pcm,
                         &bytes_used) != 0) {
            return SCE_ATRAC_ERROR_INVALID_DATA;
        }

        stream->read_position += (uint32_t)bytes_used;
        stream->frames_decoded++;
        stream->next_output_sample += stream->frame_samples;
        pcm += pcm_frame_bytes;
        decoded += stream->frame_samples;
    }

    if (output_samples)
        *output_samples = decoded;

    if (decoder_status) {
        uint32_t status = 0;

        if (stream->read_position + stream->bytes_per_frame > stream->read_size)
            status |= SCE_ATRAC_DECODER_STATUS_ALL_DATA_WAS_DECODED;
        status |= SCE_ATRAC_DECODER_STATUS_ALL_DATA_IS_ON_MEMORY;
        *decoder_status = status;
    }

    if (decoded == 0)
        return SCE_ATRAC_ERROR_ALL_DATA_WAS_DECODED;

    return 0;
}

EXPORT(SceAtrac, 0xD9409E57, int, sceAtracGetStreamInfo, int handle, SceAtracStreamInfo *info)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!info)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->pWritePosition = stream->buffer + stream->read_size;
    info->readPosition = stream->read_size;
    info->writableSize = stream->buffer_size - stream->read_size;

    return 0;
}

EXPORT(SceAtrac, 0x9BDB14F7, int, sceAtracGetVacantSize, int handle, SceUInt32 *vacant_size)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!vacant_size)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    *vacant_size = stream->buffer_size - stream->read_size;
    return 0;
}

EXPORT(SceAtrac, 0x68DFC199, int, sceAtracGetContentInfo, int handle,
       SceAtracContentInfo *content_info)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!content_info)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    memset(content_info, 0, sizeof(*content_info));
    content_info->size = sizeof(*content_info);
    content_info->atracType = SCE_ATRAC_TYPE_AT9;
    content_info->channel = (SceUInt32)stream->codec.channels;
    content_info->samplingRate = (SceUInt32)stream->codec.samplingRate;
    content_info->frameSample = stream->frame_samples;
    content_info->fixedEncBlockSize = stream->bytes_per_frame;
    content_info->fixedEncBlockSample = stream->frame_samples;
    {
        uint32_t samples = (uint32_t)stream->codec.frameSamples *
                           (uint32_t)stream->codec.framesInSuperframe;

        content_info->bitRate =
            samples ? (uint32_t)(((uint64_t)stream->codec.superframeSize * 8ull *
                                  (uint64_t)stream->codec.samplingRate) / samples)
                    : 0;
    }
    content_info->endSample = stream->total_frames
                                  ? (int)(stream->total_frames * stream->frame_samples)
                                  : -1;

    return 0;
}

EXPORT(SceAtrac, 0xDC27E7C2, int, sceAtracGetRemainSamples, int handle, SceLong64 *remain)
{
    AtracStream *stream = atrac_lookup(handle);
    uint32_t available_frames;

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!remain)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    available_frames = (stream->read_size - stream->read_position) / stream->bytes_per_frame;
    *remain = (SceLong64)available_frames * stream->frame_samples;

    return 0;
}

EXPORT(SceAtrac, 0x5B36CDB5, int, sceAtracGetOutputableSamples, int handle,
       SceLong64 *outputable)
{
    return sceAtracGetRemainSamples(handle, outputable);
}

EXPORT(SceAtrac, 0xACA5F9CC, int, sceAtracGetNextOutputPosition, int handle,
       SceUInt32 *next_sample)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!next_sample)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    *next_sample = stream->next_output_sample;
    return 0;
}

EXPORT(SceAtrac, 0xD81D7280, int, sceAtracResetNextOutputPosition, int handle,
       SceUInt32 reset_sample)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;

    stream->next_output_sample = reset_sample;
    return 0;
}

EXPORT(SceAtrac, 0x075B0C63, int, sceAtracSetOutputSamples, int handle, SceUInt32 output_samples)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (output_samples > SCE_ATRAC_MAX_OUTPUT_SAMPLES)
        return SCE_ATRAC_ERROR_INVALID_MAX_OUTPUT_SAMPLES;

    stream->output_samples = output_samples ? output_samples : SCE_ATRAC_MAX_OUTPUT_SAMPLES;
    return 0;
}

EXPORT(SceAtrac, 0x37B3F335, int, sceAtracGetOutputSamples, int handle, SceUInt32 *output_samples)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!output_samples)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    *output_samples = stream->output_samples;
    return 0;
}

EXPORT(SceAtrac, 0x5C22E927, int, sceAtracSetLoopNum, int handle, int loop_num)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (loop_num < SCE_ATRAC_INFINITE_LOOP_NUM)
        return SCE_ATRAC_ERROR_INVALID_LOOP_NUM;

    stream->loop_count = loop_num;
    return 0;
}

EXPORT(SceAtrac, 0xDC768033, int, sceAtracGetLoopInfo, int handle, int *loop_num,
       SceUInt32 *loop_status)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;

    if (loop_num)
        *loop_num = stream->loop_count;
    if (loop_status)
        *loop_status = SCE_ATRAC_LOOP_STATUS_NON_RESETABLE_PART;

    return 0;
}

EXPORT(SceAtrac, 0x722F5004, int, sceAtracIsSubBufferNeeded, int handle)
{
    (void)handle;

    /* Everything is decoded from one buffer. */
    return 0;
}

EXPORT(SceAtrac, 0xE7D9DC4F, int, sceAtracGetSubBufferInfo, int handle, SceUInt32 *read_position,
       SceUInt32 *min_sub_buffer_size, SceUInt32 *data_size)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;

    if (read_position)
        *read_position = stream->read_size;
    if (min_sub_buffer_size)
        *min_sub_buffer_size = 0;
    if (data_size)
        *data_size = 0;

    return 0;
}

EXPORT(SceAtrac, 0x272B4CBA, int, sceAtracSetSubBuffer, int handle, SceUChar8 *sub_buffer,
       SceUInt32 sub_buffer_size)
{
    (void)handle;
    (void)sub_buffer;
    (void)sub_buffer_size;

    return SCE_ATRAC_ERROR_NO_NEED_SUB_BUFFER;
}

EXPORT(SceAtrac, 0x90B512AC, int, sceAtracGetDecoderStatus, int handle, SceUInt32 *status)
{
    AtracStream *stream = atrac_lookup(handle);

    if (!stream)
        return SCE_ATRAC_ERROR_INVALID_HANDLE;
    if (!status)
        return SCE_ATRAC_ERROR_INVALID_POINTER;

    *status = 0;
    if (stream->read_position + stream->bytes_per_frame > stream->read_size)
        *status |= SCE_ATRAC_DECODER_STATUS_ALL_DATA_WAS_DECODED;

    return 0;
}

EXPORT(SceAtrac, 0x67981334, int, sceAtracGetInternalError, int handle, int *internal_error)
{
    (void)handle;

    if (internal_error)
        *internal_error = 0;

    return 0;
}

DECLARE_LIBRARY(SceAtrac, 0x9aaedaa0);

int SceAtrac_init(void)
{
    vita_atrac_dict_init(g_streams);
    g_next_handle = 0;
    g_decoder_group_created = false;
    g_group_channels = 0;
    return 0;
}

int SceAtrac_finish(void)
{
    vita_atrac_dict_clear(g_streams);
    return 0;
}
