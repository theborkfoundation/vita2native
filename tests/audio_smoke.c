/*
 * Exercises SceAudioOut without a guest: port configuration, volume and the
 * blocking behaviour of sceAudioOutOutput (which is what paces audio threads).
 */

#include <math.h>
#include <psp2/audioout.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "modules/SceAudio.h"

int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode);
int sceAudioOutReleasePort(int port);
int sceAudioOutOutput(int port, const void *buf);
int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int *vol);
int sceAudioOutGetConfig(int port, SceAudioOutConfigType type);
int sceAudioOutGetRestSample(int port);

#define SAMPLES 1024
#define FREQ    48000
#define BUFFERS 20

int main(void)
{
    static int16_t buffer[SAMPLES * 2];
    uint64_t start, elapsed_ms;
    int volume[2] = { 16384, 16384 };
    int port;

    SceAudio_init();

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, SAMPLES, FREQ,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        printf("audio_smoke: open failed: 0x%x\n", (unsigned)port);
        return 1;
    }

    if (sceAudioOutGetConfig(port, SCE_AUDIO_OUT_CONFIG_TYPE_LEN) != SAMPLES ||
        sceAudioOutGetConfig(port, SCE_AUDIO_OUT_CONFIG_TYPE_FREQ) != FREQ ||
        sceAudioOutGetConfig(port, SCE_AUDIO_OUT_CONFIG_TYPE_MODE) != SCE_AUDIO_OUT_MODE_STEREO) {
        puts("audio_smoke: get config mismatch");
        return 1;
    }

    if (sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                             volume) != 0) {
        puts("audio_smoke: set volume failed");
        return 1;
    }

    for (int i = 0; i < SAMPLES; i++) {
        double phase = 2.0 * 3.14159265358979 * 440.0 * (double)i / (double)FREQ;
        int16_t sample = (int16_t)(sin(phase) * 12000.0);

        buffer[i * 2 + 0] = sample;
        buffer[i * 2 + 1] = sample;
    }

    start = host_monotonic_ns();
    for (int i = 0; i < BUFFERS; i++) {
        if (sceAudioOutOutput(port, buffer) != 0) {
            puts("audio_smoke: output failed");
            return 1;
        }
    }
    sceAudioOutOutput(port, NULL);
    elapsed_ms = (host_monotonic_ns() - start) / 1000000ull;

    /*
     * The port should pace the caller at the buffer rate. Accept a wide window:
     * the null backend sleeps and qemu/CI machines are not real-time.
     */
    {
        uint64_t expected = (uint64_t)BUFFERS * SAMPLES * 1000 / FREQ;

        if (elapsed_ms + 150 < expected / 2 || elapsed_ms > expected * 4 + 1000) {
            printf("audio_smoke: unexpected pacing (%llu ms for %llu ms of audio)\n",
                   (unsigned long long)elapsed_ms, (unsigned long long)expected);
            return 1;
        }
        printf("audio_smoke: %llu ms for %llu ms of audio\n", (unsigned long long)elapsed_ms,
               (unsigned long long)expected);
    }

    if (sceAudioOutGetRestSample(port) < 0) {
        puts("audio_smoke: rest sample failed");
        return 1;
    }

    if (sceAudioOutReleasePort(port) != 0) {
        puts("audio_smoke: release failed");
        return 1;
    }

    SceAudio_finish();

    puts("audio_smoke: ok");
    return 0;
}
