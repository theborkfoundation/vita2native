/*
 * Exercises SceAudiodec's ATRAC9 path with a synthetic (but structurally
 * valid) codec configuration: 48000 Hz, 2 channels, 384-byte frames.
 *
 * Decoding real audio needs a real bitstream, so this checks the decoder is
 * created with the right parameters and that the control block is filled in
 * the way titles expect.
 */

#include <psp2/audiodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/SceAudiodec.h"

#include "aac_sample.inc"

int sceAudiodecInitLibrary(SceUInt32 codec_type, SceAudiodecInitParam *init_param);
int sceAudiodecTermLibrary(SceUInt32 codec_type);
int sceAudiodecCreateDecoder(SceAudiodecCtrl *ctrl, SceUInt32 codec_type);
int sceAudiodecDeleteDecoder(SceAudiodecCtrl *ctrl);
int sceAudiodecDecode(SceAudiodecCtrl *ctrl);
int sceAudiodecGetContextSize(SceAudiodecCtrl *ctrl, SceUInt32 codec_type);
int sceAudiodecClearContext(SceAudiodecCtrl *ctrl);

/* Real ADTS AAC stream (0.25 s, 440 Hz sine, 44.1 kHz stereo). */
static int test_aac(void)
{
    SceAudiodecInitParam init_param;
    SceAudiodecInfo info;
    SceAudiodecCtrl ctrl;
    int16_t pcm[8192];
    SceUInt32 first_frame;
    uint32_t nonzero = 0;

    memset(&init_param, 0, sizeof(init_param));
    init_param.aac.size = sizeof(SceAudiodecInitStreamParam);
    init_param.aac.totalStreams = 1;
    if (sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &init_param) != 0) {
        puts("audiodec_smoke: AAC init library failed");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    info.aac.size = sizeof(SceAudiodecInfoAac);
    info.aac.isAdts = 1;
    info.aac.ch = 2;
    info.aac.samplingRate = 44100;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.size = sizeof(ctrl);
    ctrl.pInfo = &info;
    if (sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AAC) != 0) {
        puts("audiodec_smoke: AAC create decoder failed");
        return 1;
    }

    memset(pcm, 0, sizeof(pcm));
    ctrl.pEs = _tmp_test_aac;
    ctrl.maxEsSize = _tmp_test_aac_len;
    ctrl.pPcm = pcm;
    ctrl.maxPcmSize = sizeof(pcm);
    if (sceAudiodecDecode(&ctrl) != 0) {
        puts("audiodec_smoke: AAC decode failed");
        return 1;
    }
    if (ctrl.inputEsSize == 0 || ctrl.outputPcmSize == 0) {
        printf("audiodec_smoke: AAC consumed %u bytes, produced %u bytes\n", ctrl.inputEsSize,
               ctrl.outputPcmSize);
        return 1;
    }
    if (info.aac.ch != 2 || info.aac.samplingRate != 44100) {
        printf("audiodec_smoke: AAC info is %u ch, %u Hz\n", info.aac.ch, info.aac.samplingRate);
        return 1;
    }
    for (uint32_t i = 0; i < ctrl.outputPcmSize / sizeof(int16_t); i++)
        nonzero += pcm[i] != 0;
    if (nonzero < 100) {
        printf("audiodec_smoke: AAC PCM is nearly silent (%u samples)\n", nonzero);
        return 1;
    }

    first_frame = ctrl.inputEsSize;
    if (first_frame < _tmp_test_aac_len) {
        ctrl.pEs += first_frame;
        ctrl.maxEsSize -= first_frame;
        if (sceAudiodecDecode(&ctrl) != 0 || ctrl.outputPcmSize == 0) {
            puts("audiodec_smoke: second AAC frame failed");
            return 1;
        }
    }

    ctrl.pEs = _tmp_test_aac;
    ctrl.maxEsSize = _tmp_test_aac_len;
    if (sceAudiodecClearContext(&ctrl) != 0 || sceAudiodecDecode(&ctrl) != 0) {
        puts("audiodec_smoke: AAC clear/restart failed");
        return 1;
    }

    if (sceAudiodecDeleteDecoder(&ctrl) != 0 ||
        sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC) != 0) {
        puts("audiodec_smoke: AAC teardown failed");
        return 1;
    }

    puts("audiodec_smoke: AAC ok");
    return 0;
}

static int test_mp3(const char *path)
{
    FILE *file = fopen(path, "rb");
    SceAudiodecInitParam init_param;
    SceAudiodecInfo info;
    SceAudiodecCtrl ctrl;
    long file_size;
    uint8_t *data;
    uint8_t *cursor;
    int16_t pcm[1152 * 2];
    size_t remaining;
    int frames = 0;

    if (!file) {
        printf("audiodec_smoke: cannot open %s\n", path);
        return 1;
    }
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)file_size);
    if (!data || fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        free(data);
        puts("audiodec_smoke: cannot read mp3");
        return 1;
    }
    fclose(file);

    memset(&init_param, 0, sizeof(init_param));
    init_param.mp3.size = sizeof(init_param.mp3);
    init_param.mp3.totalStreams = 1;
    if (sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &init_param) != 0) {
        free(data);
        puts("audiodec_smoke: mp3 init library failed");
        return 1;
    }

    memset(&info, 0, sizeof(info));
    info.mp3.size = sizeof(info.mp3);
    info.mp3.ch = 2;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.size = sizeof(ctrl);
    ctrl.pInfo = &info;
    ctrl.pPcm = pcm;
    ctrl.wordLength = 16;

    if (sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3) != 0 ||
        ctrl.maxEsSize != 1441 || ctrl.maxPcmSize != 1152 * 2 * 2) {
        free(data);
        puts("audiodec_smoke: mp3 create decoder failed");
        return 1;
    }

    cursor = data;
    remaining = (size_t)file_size;
    while (remaining > 4) {
        int16_t *samples = pcm;
        int nonzero = 0;
        int result;

        ctrl.pEs = cursor;
        ctrl.pPcm = pcm;
        result = sceAudiodecDecode(&ctrl);
        if (result != 0) {
            /* File-oriented input can end on a truncated frame; anything
             * shorter than one maximum-size ES is treated as end of stream. */
            if (remaining < ctrl.maxEsSize)
                break;
            printf("audiodec_smoke: mp3 decode failed at frame %d\n", frames);
            free(data);
            return 1;
        }
        if (ctrl.inputEsSize == 0 || ctrl.outputPcmSize == 0) {
            printf("audiodec_smoke: mp3 decode made no progress at frame %d\n", frames);
            free(data);
            return 1;
        }
        for (size_t i = 0; i < ctrl.outputPcmSize / sizeof(int16_t); i++)
            nonzero |= samples[i] != 0;
        /* The first frames carry the encoder delay/bit reservoir and can be
         * legitimately silent. */
        if (!nonzero && frames >= 2) {
            puts("audiodec_smoke: mp3 produced silence");
            free(data);
            return 1;
        }
        frames++;
        cursor += ctrl.inputEsSize;
        remaining -= ctrl.inputEsSize;
    }

    if (frames == 0) {
        free(data);
        puts("audiodec_smoke: no mp3 frames decoded");
        return 1;
    }

    sceAudiodecDeleteDecoder(&ctrl);
    sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
    free(data);
    printf("audiodec_smoke: mp3 ok (%d frames)\n", frames);
    return 0;
}

int main(int argc, char *argv[])
{
    SceAudiodecInitParam init_param;
    SceAudiodecInfo info;
    SceAudiodecCtrl ctrl;

    SceAudiodec_init();

    memset(&init_param, 0, sizeof(init_param));
    init_param.at9.size = sizeof(init_param.at9);
    init_param.at9.totalCh = 2;

    if (sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AT9, &init_param) != 0) {
        puts("audiodec_smoke: init library failed");
        return 1;
    }

    /* 48000 Hz, channel config 1 (two mono blocks), 384-byte frames. */
    memset(&info, 0, sizeof(info));
    info.at9.size = sizeof(info.at9);
    info.at9.configData[0] = 0xFE;
    info.at9.configData[1] = 0x72;
    info.at9.configData[2] = 0x2F;
    info.at9.configData[3] = 0xE0;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.size = sizeof(ctrl);
    ctrl.pInfo = &info;

    if (sceAudiodecGetContextSize(&ctrl, SCE_AUDIODEC_TYPE_AT9) != 0) {
        puts("audiodec_smoke: get context size failed");
        return 1;
    }

    if (sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AT9) != 0) {
        puts("audiodec_smoke: create decoder failed");
        return 1;
    }

    if (info.at9.ch != 2 || info.at9.samplingRate != 48000 ||
        info.at9.superFrameSize != 384 || info.at9.framesInSuperFrame != 1) {
        printf("audiodec_smoke: wrong codec info (ch %u, rate %u, superframe %u, frames %u)\n",
               info.at9.ch, info.at9.samplingRate, info.at9.superFrameSize,
               info.at9.framesInSuperFrame);
        return 1;
    }

    if (ctrl.maxPcmSize != 256 * 2 * 2 || ctrl.maxEsSize != 384 || ctrl.wordLength != 16) {
        printf("audiodec_smoke: wrong window sizes (es %u, pcm %u, word %u)\n", ctrl.maxEsSize,
               ctrl.maxPcmSize, ctrl.wordLength);
        return 1;
    }

    if (ctrl.handle <= 0) {
        puts("audiodec_smoke: no decoder handle");
        return 1;
    }

    if (sceAudiodecDeleteDecoder(&ctrl) != 0) {
        puts("audiodec_smoke: delete decoder failed");
        return 1;
    }

    if (sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AT9) != 0) {
        puts("audiodec_smoke: term library failed");
        return 1;
    }

    if (argc > 1 && test_mp3(argv[1]) != 0)
        return 1;

    if (test_aac() != 0)
        return 1;

    SceAudiodec_finish();

    puts("audiodec_smoke: ok");
    return 0;
}
