/*
 * Exercises the SceAtrac container handling: build a minimal RIFF/AT9 file
 * (48000 Hz stereo, 384-byte frames) and check that the handle, stream info
 * and content info match what the container describes.
 *
 * The ES payload is zeros, so real decoding is not attempted; this covers the
 * parsing and lifecycle that the graphics/audio glue depends on.
 */

#include <psp2/atrac.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/SceAtrac.h"

int sceAtracCreateDecoderGroup(SceUInt32 atrac_type, const SceAtracDecoderGroup *group,
                               void *work_mem, int init_audiodec);
int sceAtracDeleteDecoderGroup(SceUInt32 atrac_type, int term_audiodec);
int sceAtracSetDataAndAcquireHandle(SceUChar8 *buffer, SceUInt32 read_size,
                                    SceUInt32 buffer_size);
int sceAtracReleaseHandle(int handle);
int sceAtracGetStreamInfo(int handle, SceAtracStreamInfo *info);
int sceAtracGetContentInfo(int handle, SceAtracContentInfo *info);
int sceAtracGetVacantSize(int handle, SceUInt32 *vacant_size);
int sceAtracGetRemainSamples(int handle, SceLong64 *remain);
int sceAtracSetLoopNum(int handle, int loop_num);
int sceAtracGetLoopInfo(int handle, int *loop_num, SceUInt32 *loop_status);
int sceAtracAddStreamData(int handle, SceUInt32 add_size);

#define BUFFER_SIZE 0x1000
#define READ_SIZE   0x800

static uint8_t g_buffer[BUFFER_SIZE] __attribute__((aligned(0x100)));

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void build_container(void)
{
    uint8_t *fmt;
    uint8_t *fact;
    uint8_t *data;

    memset(g_buffer, 0, sizeof(g_buffer));

    memcpy(g_buffer + 0x00, "RIFF", 4);
    put_le32(g_buffer + 0x04, BUFFER_SIZE - 8);
    memcpy(g_buffer + 0x08, "WAVE", 4);

    /* fmt chunk: payload starts at 0x14, config at payload + 0x14. */
    memcpy(g_buffer + 0x0C, "fmt ", 4);
    put_le32(g_buffer + 0x10, 0x20);
    fmt = g_buffer + 0x14;
    fmt[0] = 0xFE; /* format tag 0xFFFE (extensible) low byte */
    fmt[1] = 0xFF;
    fmt[2] = 2; /* channels */
    put_le32(fmt + 4, 48000);
    put_le32(fmt + 0x14, 0); /* placeholder, config goes here */
    fmt[0x14] = 0xFE;
    fmt[0x15] = 0x72;
    fmt[0x16] = 0x2F;
    fmt[0x17] = 0xE0;

    /* fact chunk right after the fmt chunk. */
    fact = g_buffer + 0x14 + 0x20;
    memcpy(fact, "fact", 4);
    put_le32(fact + 4, 0x0C);
    put_le32(fact + 8, READ_SIZE);

    /* data chunk follows. */
    data = fact + 8 + 0x0C;
    memcpy(data, "data", 4);
    put_le32(data + 4, BUFFER_SIZE - (uint32_t)(data + 8 - g_buffer));
}

int main(void)
{
    SceAtracDecoderGroup group;
    SceAtracStreamInfo stream_info;
    SceAtracContentInfo content_info;
    SceLong64 remain = 0;
    SceUInt32 vacant = 0;
    SceUInt32 loop_status = 0;
    int loop_num = 0;
    int handle;

    build_container();

    SceAtrac_init();

    memset(&group, 0, sizeof(group));
    group.size = sizeof(group);
    group.wordLength = 16;
    group.totalCh = 2;
    if (sceAtracCreateDecoderGroup(SCE_ATRAC_TYPE_AT9, &group, g_buffer, 0) != 0) {
        puts("atrac_smoke: create decoder group failed");
        return 1;
    }

    handle = sceAtracSetDataAndAcquireHandle(g_buffer, READ_SIZE, BUFFER_SIZE);
    if (handle <= 0) {
        printf("atrac_smoke: acquire handle failed: 0x%x\n", (unsigned)handle);
        return 1;
    }

    memset(&stream_info, 0, sizeof(stream_info));
    if (sceAtracGetStreamInfo(handle, &stream_info) != 0 ||
        stream_info.readPosition != READ_SIZE ||
        stream_info.writableSize != BUFFER_SIZE - READ_SIZE) {
        printf("atrac_smoke: stream info mismatch (read %u, writable %u)\n",
               stream_info.readPosition, stream_info.writableSize);
        return 1;
    }

    memset(&content_info, 0, sizeof(content_info));
    if (sceAtracGetContentInfo(handle, &content_info) != 0 ||
        content_info.channel != 2 || content_info.samplingRate != 48000 ||
        content_info.frameSample != 256) {
        printf("atrac_smoke: content info mismatch (ch %u, rate %u, samples %u)\n",
               content_info.channel, content_info.samplingRate, content_info.frameSample);
        return 1;
    }

    if (sceAtracGetVacantSize(handle, &vacant) != 0 || vacant != BUFFER_SIZE - READ_SIZE) {
        puts("atrac_smoke: vacant size mismatch");
        return 1;
    }

    if (sceAtracGetRemainSamples(handle, &remain) != 0 || remain <= 0) {
        puts("atrac_smoke: remain samples mismatch");
        return 1;
    }

    if (sceAtracSetLoopNum(handle, SCE_ATRAC_INFINITE_LOOP_NUM) != 0 ||
        sceAtracGetLoopInfo(handle, &loop_num, &loop_status) != 0 ||
        loop_num != SCE_ATRAC_INFINITE_LOOP_NUM) {
        puts("atrac_smoke: loop configuration failed");
        return 1;
    }

    if (sceAtracAddStreamData(handle, 0x100) != 0) {
        puts("atrac_smoke: add stream data failed");
        return 1;
    }

    if (sceAtracReleaseHandle(handle) != 0) {
        puts("atrac_smoke: release handle failed");
        return 1;
    }

    if (sceAtracDeleteDecoderGroup(SCE_ATRAC_TYPE_AT9, 0) != 0) {
        puts("atrac_smoke: delete decoder group failed");
        return 1;
    }

    SceAtrac_finish();

    puts("atrac_smoke: ok");
    return 0;
}
