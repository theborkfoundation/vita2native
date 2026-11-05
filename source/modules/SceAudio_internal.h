#ifndef SCE_AUDIO_INTERNAL_H
#define SCE_AUDIO_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Host audio sink used by SceAudio. A port hands interleaved S16 stereo frames
 * at the device rate; the backend either plays them or, when there is no
 * device, lets the caller pace itself.
 */
typedef struct HostAudioPort HostAudioPort;

/* Opens the shared device. Returns NULL when no audio device is available. */
bool host_audio_open(int freq);
void host_audio_close(void);

/* Creates/destroys a per-port queue feeding the mixer. */
HostAudioPort *host_audio_port_create(int capacity_frames);
void host_audio_port_destroy(HostAudioPort *port);

/* Appends frames, blocking while the queue is full. Returns 0 on success. */
int host_audio_port_write(HostAudioPort *port, const int16_t *frames, size_t frame_count);

/* Frames still queued for this port. */
size_t host_audio_port_pending(HostAudioPort *port);

/* Waits until the queue is drained. */
void host_audio_port_drain(HostAudioPort *port);

#endif
