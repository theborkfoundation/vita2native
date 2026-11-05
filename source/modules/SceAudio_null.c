/*
 * Headless audio: there is no device, so SceAudio paces output with a sleep
 * instead. Guest timing (and therefore frame pacing) stays correct.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SceAudio_internal.h"

bool host_audio_open(int freq)
{
    (void)freq;

    return false;
}

void host_audio_close(void)
{
}

HostAudioPort *host_audio_port_create(int capacity_frames)
{
    (void)capacity_frames;

    return NULL;
}

void host_audio_port_destroy(HostAudioPort *port)
{
    (void)port;
}

int host_audio_port_write(HostAudioPort *port, const int16_t *frames, size_t frame_count)
{
    (void)port;
    (void)frames;
    (void)frame_count;

    return 0;
}

size_t host_audio_port_pending(HostAudioPort *port)
{
    (void)port;

    return 0;
}

void host_audio_port_drain(HostAudioPort *port)
{
    (void)port;
}
