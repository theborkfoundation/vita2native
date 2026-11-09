# TODO

## Audio: implement SceNgs rendering

**Priority:** low.

**Status:** Game audio is silent. The SDL/ALSA output path is verified working;
the guest submits all-zero sample buffers to `SceAudio`.

### Evidence

- Angry Birds Trilogy imports `SceNgs` (NID `0xb01598d9`); it does not import
  `SceAtrac` or `SceAudiodec`.
- `source/modules/SceNgs.c` is a bookkeeping stub whose rendering is silent.
- With `VITA2NATIVE_AUDIO_DEBUG=1`, every port reports `peak 0` while the
  mixer consumes ~48,000 frames/s:
  `SceAudio: port 0 calls 190 peak 0 vol 32768/32768 pending 1536 played 97280`
- R36S output path confirmed: SDL uses ALSA at 48 kHz, S16 stereo; the rk817
  mixer is `Playback Path = SPK_HP`; `aplay` of a 440 Hz test WAV is clean.
- `SceAvPlayer`'s audio stub also returns zeroed buffers, so movie audio is
  silent by design. The audible crackle is downstream of the guest silence
  (amp/ALSA artifacts), not game content.

### Plan

1. Add call tracing to `source/modules/SceNgs.c` behind a dedicated env var
   (for example `VITA2NATIVE_NGS_DEBUG`) logging systems, racks, voice
   definitions, modules/presets, patches, and pull/push buffers. Use it on
   Angry Birds to determine the minimal graph that must render.
2. Port the Vita3K NGS engine:
   - `external/Vita3K/vita3k/ngs/` — engine, definitions, scheduler and the
     AT9 voice module (~3.2k lines).
   - `external/Vita3K/vita3k/modules/SceNgsUser/SceNgs.cpp` — the guest API
     glue (~1.1k lines).
   - Requires a shim for Vita3K's `MemState`/`Ptr` translation and scheduler,
     wired to vita2native's guest memory.
3. Verify music and effects in Angry Birds Trilogy, then re-run the four-title
   regression (Flappy Bird, VitaShell, SuDokuL, PixelChess).

### Notes

- `SceAudio` currently queues 4 device buffers per port
  (`source/modules/SceAudio.c`); diagnostics are gated by
  `VITA2NATIVE_AUDIO_DEBUG`.
- The texture-cache byte budget (`source/modules/SceGxm.c`) and the removed
  `sceNpCheckCallback` log are unrelated to this item.
