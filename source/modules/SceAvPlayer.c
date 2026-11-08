/*
 * SceAvPlayer: synthetic video playback.
 *
 * The host has no H.264 decoder, so the player presents a generated 30 fps
 * 960x544 test pattern and silent 48 kHz stereo audio. Applications exercise
 * their normal frame-pulling and timing code; the pixels are not the file's.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/avplayer.h>

#include "host.h"
#include "module.h"

#define AVPLAYER_MAX_PLAYERS 4
#define AVPLAYER_WIDTH       960
#define AVPLAYER_HEIGHT      544
#define AVPLAYER_FPS         30
#define AVPLAYER_DURATION_MS 3000ull
#define AVPLAYER_AUDIO_SIZE  2048
#define AVPLAYER_Y_SIZE      (AVPLAYER_WIDTH * AVPLAYER_HEIGHT)
#define AVPLAYER_UV_SIZE     ((AVPLAYER_WIDTH / 2) * (AVPLAYER_HEIGHT / 2))
#define AVPLAYER_FRAME_SIZE  (AVPLAYER_Y_SIZE + 2 * AVPLAYER_UV_SIZE)

typedef struct {
    bool used;
    bool active;
    bool paused;
    bool looping;
    bool video_enabled;
    bool audio_enabled;
    bool have_source;
    int trick_speed;
    uint64_t position_ns;
    uint64_t resume_ns;
    uint64_t jump_ms;
    uint64_t last_video_ns;
    uint64_t last_audio_ns;
    uint32_t frame_index;
    uint8_t *video_buffer;
    uint8_t *audio_buffer;
    SceAvPlayerMemReplacement memory;
    void *event_arg;
    SceAvPlayerEventCallback event_callback;
    bool ended;
    char source[256];
} AvPlayer;

static AvPlayer g_players[AVPLAYER_MAX_PLAYERS];

static AvPlayer *player_get(SceAvPlayerHandle handle)
{
    int index = (int)handle - 1;

    if (index < 0 || index >= AVPLAYER_MAX_PLAYERS || !g_players[index].used)
        return NULL;

    return &g_players[index];
}

static uint64_t player_now_ms(const AvPlayer *player)
{
    uint64_t ns = player->position_ns;

    if (player->active && !player->paused)
        ns += host_monotonic_ns() - player->resume_ns;

    ns = ns / 1000000ull * (uint64_t)(player->trick_speed > 0 ? player->trick_speed : 100) / 100u;
    ns += player->jump_ms;

    if (player->looping && ns >= AVPLAYER_DURATION_MS)
        ns %= AVPLAYER_DURATION_MS;

    return ns;
}

static void player_fire_event(AvPlayer *player, int32_t event_id)
{
    if (player->event_callback)
        player->event_callback(player->event_arg, event_id, 0, NULL);
}

/* Synthetic playback is short; report the end so the guest can move on. */
static void player_check_end(AvPlayer *player)
{
    if (!player->active || player->ended || player->looping)
        return;
    if (player_now_ms(player) < AVPLAYER_DURATION_MS)
        return;

    player->ended = true;
    player->active = false;
    player->paused = false;
    player_fire_event(player, 1); /* stop */
}

static void player_fill_video(AvPlayer *player)
{
    /* RGB value of eight colour bars, converted to I420 below. */
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 },   { 255, 0, 0 },   { 0, 0, 255 },   { 0, 0, 0 },
    };
    uint8_t *y_plane = player->video_buffer;
    uint8_t *v_plane = y_plane + AVPLAYER_Y_SIZE;
    uint8_t *u_plane = v_plane + AVPLAYER_UV_SIZE;
    uint32_t band = (player->frame_index * 8) % (AVPLAYER_HEIGHT + 64);

    /*
     * The player hands YUV420P to the guest, which uploads it as three
     * textures and converts in its shader; RGBA here renders as noise.
     */
    for (uint32_t y = 0; y < AVPLAYER_HEIGHT; y++) {
        bool in_band = y >= band && y < band + 48;

        for (uint32_t x = 0; x < AVPLAYER_WIDTH; x++) {
            const uint8_t *rgb = bars[x * 8 / AVPLAYER_WIDTH];
            float r = rgb[0], g = rgb[1], b = rgb[2];
            float luma = 0.299f * r + 0.587f * g + 0.114f * b;

            if (in_band)
                luma = 235.0f;
            y_plane[y * AVPLAYER_WIDTH + x] = (uint8_t)(luma + 0.5f);
        }
    }

    for (uint32_t y = 0; y < AVPLAYER_HEIGHT / 2; y++) {
        for (uint32_t x = 0; x < AVPLAYER_WIDTH / 2; x++) {
            const uint8_t *rgb = bars[(x * 2) * 8 / AVPLAYER_WIDTH];
            float r = rgb[0], g = rgb[1], b = rgb[2];

            u_plane[y * (AVPLAYER_WIDTH / 2) + x] =
                (uint8_t)(-0.169f * r - 0.331f * g + 0.5f * b + 128.0f + 0.5f);
            v_plane[y * (AVPLAYER_WIDTH / 2) + x] =
                (uint8_t)(0.5f * r - 0.419f * g - 0.081f * b + 128.0f + 0.5f);
        }
    }
}

EXPORT(SceAvPlayer, 0x4C847ADF, SceAvPlayerHandle, sceAvPlayerInit, SceAvPlayerInitData *data)
{
    for (int i = 0; i < AVPLAYER_MAX_PLAYERS; i++) {
        if (g_players[i].used)
            continue;

        memset(&g_players[i], 0, sizeof(g_players[i]));
        g_players[i].used = true;
        g_players[i].video_enabled = true;
        g_players[i].audio_enabled = true;
        g_players[i].trick_speed = 100;
        if (data) {
            g_players[i].memory = data->memoryReplacement;
            g_players[i].event_arg = data->eventReplacement.objectPointer;
            g_players[i].event_callback = data->eventReplacement.eventCallback;
        }
        fprintf(stderr, "SceAvPlayer: init handle %d callback %p arg %p\n", i + 1,
                (void *)g_players[i].event_callback, g_players[i].event_arg);

        return (SceAvPlayerHandle)(i + 1);
    }

    return (SceAvPlayerHandle)SCE_AVPLAYER_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceAvPlayer, 0x3F51D38A, int, sceAvPlayerPostInit, SceAvPlayerHandle handle)
{
    return player_get(handle) ? 0 : SCE_AVPLAYER_ERROR_INVALID_PARAM;
}

EXPORT(SceAvPlayer, 0xBC83C8FD, int, sceAvPlayerAddSource, SceAvPlayerHandle handle,
       const char *filename)
{
    AvPlayer *player = player_get(handle);

    if (!player || !filename)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    snprintf(player->source, sizeof(player->source), "%s", filename);
    player->have_source = true;
    player->ended = false;
    fprintf(stderr, "SceAvPlayer: add source '%s'\n", filename);

    /*
     * A source is opened and ready synchronously here, but guests wait for
     * these events (Angry Birds spins on a flag its callback clears after
     * AddSource returns), so they have to be delivered regardless.
     */
    player_fire_event(player, 5); /* buffering */
    player_fire_event(player, 2); /* ready */

    return 0;
}

EXPORT(SceAvPlayer, 0x2ACB4217, int, sceAvPlayerStart, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player || !player->have_source)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    if (!player->active || player->paused) {
        player->resume_ns = host_monotonic_ns();
        player->active = true;
        player->paused = false;
        player->ended = false;
        player->last_video_ns = 0;
        player->last_audio_ns = 0;
    }

    player_fire_event(player, 3); /* play */

    return 0;
}

EXPORT(SceAvPlayer, 0xBD35E360, int, sceAvPlayerStop, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    if (player->active && !player->paused)
        player->position_ns += host_monotonic_ns() - player->resume_ns;
    player->active = false;
    player->paused = false;

    player_fire_event(player, 1); /* stop */

    return 0;
}

EXPORT(SceAvPlayer, 0x34FD744B, int, sceAvPlayerPause, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;
    if (player->active && !player->paused) {
        player->position_ns += host_monotonic_ns() - player->resume_ns;
        player->paused = true;
    }

    player_fire_event(player, 4); /* pause */

    return 0;
}

EXPORT(SceAvPlayer, 0x11FF162D, int, sceAvPlayerResume, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;
    if (player->paused) {
        player->resume_ns = host_monotonic_ns();
        player->paused = false;
        player->active = true;
    }

    player_fire_event(player, 3); /* play */

    return 0;
}

EXPORT(SceAvPlayer, 0xEC103ADF, int, sceAvPlayerSetLooping, SceAvPlayerHandle handle,
       SceBool looping)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    player->looping = looping != SCE_FALSE;
    return 0;
}

EXPORT(SceAvPlayer, 0x2E4FF35F, SceBool, sceAvPlayerIsActive, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_FALSE;

    player_check_end(player);
    return player->active && !player->paused ? SCE_TRUE : SCE_FALSE;
}

EXPORT(SceAvPlayer, 0x804DCECD, uint64_t, sceAvPlayerCurrentTime, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return 0;

    player_check_end(player);
    return player_now_ms(player);
}

EXPORT(SceAvPlayer, 0x5B7DB4BC, int, sceAvPlayerJumpToTime, SceAvPlayerHandle handle,
       uint64_t offset)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    player->position_ns = 0;
    player->resume_ns = host_monotonic_ns();
    player->jump_ms = offset;
    player->last_video_ns = 0;
    player->last_audio_ns = 0;

    return 0;
}

EXPORT(SceAvPlayer, 0x86E0CF55, int, sceAvPlayerSetTrickSpeed, SceAvPlayerHandle handle,
       int speed)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    if (player->active && !player->paused) {
        player->position_ns += host_monotonic_ns() - player->resume_ns;
        player->resume_ns = host_monotonic_ns();
    }
    player->trick_speed = speed;

    return 0;
}

EXPORT(SceAvPlayer, 0x2CD86FD6, int, sceAvPlayerStreamCount, SceAvPlayerHandle handle)
{
    return player_get(handle) ? 2 : SCE_AVPLAYER_ERROR_INVALID_PARAM;
}

EXPORT(SceAvPlayer, 0x34E2D370, int, sceAvPlayerGetStreamInfo, SceAvPlayerHandle handle,
       uint32_t id, SceAvPlayerStreamInfo *info)
{
    AvPlayer *player = player_get(handle);

    if (!player || !info || id > 1)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    memset(info, 0, sizeof(*info));
    if (id == 0) {
        info->type = SCE_AVPLAYER_VIDEO;
        info->details.video.width = AVPLAYER_WIDTH;
        info->details.video.height = AVPLAYER_HEIGHT;
        info->details.video.aspectRatio = (float)AVPLAYER_WIDTH / (float)AVPLAYER_HEIGHT;
    } else {
        info->type = SCE_AVPLAYER_AUDIO;
        info->details.audio.channelCount = 2;
        info->details.audio.sampleRate = 48000;
        info->details.audio.size = AVPLAYER_AUDIO_SIZE;
    }
    info->duration = AVPLAYER_DURATION_MS;

    return 0;
}

EXPORT(SceAvPlayer, 0x70127AB8, int, sceAvPlayerEnableStream, SceAvPlayerHandle handle,
       uint32_t stream_no)
{
    AvPlayer *player = player_get(handle);

    if (!player || stream_no > 1)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;
    if (stream_no == 0)
        player->video_enabled = true;
    else
        player->audio_enabled = true;

    return 0;
}

EXPORT(SceAvPlayer, 0x15365ECE, int, sceAvPlayerDisableStream, SceAvPlayerHandle handle,
       uint32_t stream_no)
{
    AvPlayer *player = player_get(handle);

    if (!player || stream_no > 1)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;
    if (stream_no == 0)
        player->video_enabled = false;
    else
        player->audio_enabled = false;

    return 0;
}

EXPORT(SceAvPlayer, 0x8BC3221B, SceBool, sceAvPlayerGetVideoData, SceAvPlayerHandle handle,
       SceAvPlayerFrameInfo *info)
{
    AvPlayer *player = player_get(handle);
    uint64_t now;

    if (!player || !info || !player->active || player->paused || !player->video_enabled)
        return SCE_FALSE;

    player_check_end(player);
    if (!player->active)
        return SCE_FALSE;

    now = host_monotonic_ns();
    if (player->last_video_ns && now - player->last_video_ns < 1000000000ull / AVPLAYER_FPS)
        return SCE_FALSE;

    if (!player->video_buffer) {
        size_t size = AVPLAYER_FRAME_SIZE;

        if (player->memory.allocateTexture && player->memory.objectPointer)
            player->video_buffer =
                player->memory.allocateTexture(player->memory.objectPointer, 64, (uint32_t)size);
        else
            player->video_buffer = malloc(size);
        if (!player->video_buffer)
            return SCE_FALSE;
    }

    player_fill_video(player);
    player->last_video_ns = now;
    player->frame_index++;

    memset(info, 0, sizeof(*info));
    info->pData = player->video_buffer;
    info->timeStamp = player_now_ms(player);
    info->details.video.width = AVPLAYER_WIDTH;
    info->details.video.height = AVPLAYER_HEIGHT;
    info->details.video.aspectRatio = (float)AVPLAYER_WIDTH / (float)AVPLAYER_HEIGHT;

    return SCE_TRUE;
}

EXPORT(SceAvPlayer, 0x22986CD1, SceBool, sceAvPlayerGetVideoDataEx, SceAvPlayerHandle handle,
       SceAvPlayerFrameInfo *info)
{
    return sceAvPlayerGetVideoData(handle, info);
}

EXPORT(SceAvPlayer, 0xA10FC252, SceBool, sceAvPlayerGetAudioData, SceAvPlayerHandle handle,
       SceAvPlayerFrameInfo *info)
{
    AvPlayer *player = player_get(handle);
    uint64_t now;

    if (!player || !info || !player->active || player->paused || !player->audio_enabled)
        return SCE_FALSE;

    now = host_monotonic_ns();
    if (player->last_audio_ns && now - player->last_audio_ns < 1000000000ull / AVPLAYER_FPS)
        return SCE_FALSE;

    if (!player->audio_buffer) {
        if (player->memory.allocateTexture && player->memory.objectPointer)
            player->audio_buffer =
                player->memory.allocateTexture(player->memory.objectPointer, 64, AVPLAYER_AUDIO_SIZE);
        else
            player->audio_buffer = calloc(1, AVPLAYER_AUDIO_SIZE);
        if (!player->audio_buffer)
            return SCE_FALSE;
    }

    memset(player->audio_buffer, 0, AVPLAYER_AUDIO_SIZE);
    player->last_audio_ns = now;

    memset(info, 0, sizeof(*info));
    info->pData = player->audio_buffer;
    info->timeStamp = player_now_ms(player);
    info->details.audio.channelCount = 2;
    info->details.audio.sampleRate = 48000;
    info->details.audio.size = AVPLAYER_AUDIO_SIZE;

    return SCE_TRUE;
}

EXPORT(SceAvPlayer, 0x7AA29B2D, int, sceAvPlayerClose, SceAvPlayerHandle handle)
{
    AvPlayer *player = player_get(handle);

    if (!player)
        return SCE_AVPLAYER_ERROR_INVALID_PARAM;

    if (player->video_buffer) {
        if (player->memory.deallocateTexture && player->memory.objectPointer)
            player->memory.deallocateTexture(player->memory.objectPointer, player->video_buffer);
        else
            free(player->video_buffer);
    }
    if (player->audio_buffer) {
        if (player->memory.deallocateTexture && player->memory.objectPointer)
            player->memory.deallocateTexture(player->memory.objectPointer, player->audio_buffer);
        else
            free(player->audio_buffer);
    }
    memset(player, 0, sizeof(*player));

    return 0;
}

DECLARE_LIBRARY(SceAvPlayer, 0xFA10CE65);
