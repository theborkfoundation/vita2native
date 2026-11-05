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
#include <string.h>

#include "modules/SceAudiodec.h"

int sceAudiodecInitLibrary(SceUInt32 codec_type, SceAudiodecInitParam *init_param);
int sceAudiodecTermLibrary(SceUInt32 codec_type);
int sceAudiodecCreateDecoder(SceAudiodecCtrl *ctrl, SceUInt32 codec_type);
int sceAudiodecDeleteDecoder(SceAudiodecCtrl *ctrl);
int sceAudiodecGetContextSize(SceAudiodecCtrl *ctrl, SceUInt32 codec_type);

int main(void)
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

    SceAudiodec_finish();

    puts("audiodec_smoke: ok");
    return 0;
}
