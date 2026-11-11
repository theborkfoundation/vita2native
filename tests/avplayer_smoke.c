/*
 * SceAvPlayer test: synthetic video/audio frames, stream info and timing.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <psp2/avplayer.h>
#include "test.h"

const void *module_get_func_export(uint32_t lib_nid, uint32_t func_nid, void *importer);

int sceAvPlayerPostInit(SceAvPlayerHandle handle);
int sceAvPlayerStreamCount(SceAvPlayerHandle handle);
int sceAvPlayerEnableStream(SceAvPlayerHandle handle, uint32_t stream_no);
int sceAvPlayerDisableStream(SceAvPlayerHandle handle, uint32_t stream_no);

static int check_export(uint32_t lib_nid, uint32_t func_nid, const char *what)
{
    if (module_get_func_export(lib_nid, func_nid, NULL))
        return 0;
    fprintf(stderr, "%s (0x%08x/0x%08x) is not in the export table\n", what, lib_nid, func_nid);
    return 1;
}

int main(void)
{
    SceAvPlayerInitData init_data;
    SceAvPlayerStreamInfo stream;
    SceAvPlayerFrameInfo frame;
    SceAvPlayerHandle player;

    failures += check_export(0xFA10CE65, 0x4C847ADF, "sceAvPlayerInit");
    failures += check_export(0xFA10CE65, 0x8BC3221B, "sceAvPlayerGetVideoData");

    memset(&init_data, 0, sizeof(init_data));
    player = sceAvPlayerInit(&init_data);
    CHECK(player >= 0, "init failed (%d)", player);
    if (player < 0)
        return 1;

    CHECK(sceAvPlayerPostInit(player) == 0, "post init failed");
    CHECK(sceAvPlayerAddSource(player, "ux0:/video/test.mp4") == 0, "add source failed");
    CHECK(sceAvPlayerStreamCount(player) == 2, "unexpected stream count");

    memset(&stream, 0, sizeof(stream));
    CHECK(sceAvPlayerGetStreamInfo(player, 0, &stream) == 0, "video stream info failed");
    CHECK(stream.type == SCE_AVPLAYER_VIDEO && stream.details.video.width == 960 &&
              stream.details.video.height == 544,
          "video stream is %ux%u type %u", stream.details.video.width, stream.details.video.height,
          stream.type);
    memset(&stream, 0, sizeof(stream));
    CHECK(sceAvPlayerGetStreamInfo(player, 1, &stream) == 0, "audio stream info failed");
    CHECK(stream.type == SCE_AVPLAYER_AUDIO && stream.details.audio.channelCount == 2 &&
              stream.details.audio.sampleRate == 48000,
          "audio stream info failed");

    CHECK(sceAvPlayerStart(player) == 0, "start failed");
    CHECK(sceAvPlayerIsActive(player) == SCE_TRUE, "player not active after start");

    memset(&frame, 0, sizeof(frame));
    CHECK(sceAvPlayerGetVideoData(player, &frame) == SCE_TRUE, "no video frame");
    CHECK(frame.pData != NULL && frame.details.video.width == 960, "video frame is empty");
    if (frame.pData) {
        uint32_t non_black = 0;

        for (uint32_t i = 0; i < 960 * 544 * 4; i += 4)
            non_black += frame.pData[i] != 0 || frame.pData[i + 1] != 0;
        CHECK(non_black > 1000, "video frame is nearly black (%u)", non_black);
    }
    usleep(40000);
    CHECK(sceAvPlayerGetVideoData(player, &frame) == SCE_TRUE, "no second video frame");
    CHECK(frame.pData != NULL, "second video frame is empty");

    memset(&frame, 0, sizeof(frame));
    CHECK(sceAvPlayerGetAudioData(player, &frame) == SCE_TRUE, "no audio frame");
    CHECK(frame.pData != NULL && frame.details.audio.size > 0, "audio frame is empty");

    CHECK(sceAvPlayerPause(player) == 0, "pause failed");
    CHECK(sceAvPlayerIsActive(player) == SCE_FALSE, "player still active after pause");
    CHECK(sceAvPlayerGetVideoData(player, &frame) == SCE_FALSE, "frame while paused");
    CHECK(sceAvPlayerResume(player) == 0, "resume failed");
    CHECK(sceAvPlayerIsActive(player) == SCE_TRUE, "player not active after resume");

    CHECK(sceAvPlayerJumpToTime(player, 5000) == 0, "jump failed");
    CHECK(sceAvPlayerCurrentTime(player) >= 5000, "current time did not jump");
    CHECK(sceAvPlayerSetTrickSpeed(player, SCE_AVPLAYER_TRICK_SPEED_FAST_FORWARD_2X) == 0,
          "trick speed failed");
    CHECK(sceAvPlayerSetLooping(player, SCE_TRUE) == 0, "looping failed");

    CHECK(sceAvPlayerDisableStream(player, 0) == 0, "disable video failed");
    CHECK(sceAvPlayerGetVideoData(player, &frame) == SCE_FALSE, "disabled video produced a frame");
    CHECK(sceAvPlayerEnableStream(player, 0) == 0, "enable video failed");

    CHECK(sceAvPlayerStop(player) == 0, "stop failed");
    CHECK(sceAvPlayerIsActive(player) == SCE_FALSE, "player active after stop");
    CHECK(sceAvPlayerClose(player) == 0, "close failed");
    CHECK(sceAvPlayerClose(player) < 0, "double close was accepted");

    return test_status("avplayer_smoke");
}
