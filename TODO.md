# TODO

## Audio: SceNgs rendering

**Priority:** high.

**Status:** implemented. `source/modules/SceNgs_render.c` renders the graph the
titles use: ATRAC9 voices, the HEVAG/PCM player (Simple voices), patch volume
matrices, buffer switch/end-of-data callbacks and the master buss output. The
guest pulls mixed S16 frames with `sceNgsVoiceGetStateData` and feeds
`sceAudioOutOutput`; `VITA2NATIVE_AUDIO_DEBUG=1` now reports non-zero peaks and
Angry Birds Trilogy plays menu music and effects on the R36S.

### Remaining gaps

- The EQ/envelope/pauser/filter modules after the source module are
  pass-through. Presets and their parameter blocks are ignored. ABT does not
  call `sceNgsVoiceSetPreset`, but other titles may.
- Only the master buss output is mixed; chained non-master busses and the
  extra Simple/ATRAC9 outputs are ignored.
- HEVAG/PCM sources are resampled to the system rate with linear interpolation;
  ATRAC9 is assumed to be 48 kHz.
- `SceAvPlayer` audio still returns zeroed buffers, so movie audio is silent by
  design.

### Notes

- `SceAudio` queues 4 device buffers per port (`source/modules/SceAudio.c`);
  diagnostics are gated by `VITA2NATIVE_AUDIO_DEBUG`.
- NGS tracing (`VITA2NATIVE_NGS_DEBUG=1`) logs the graph, parameter dumps,
  master peaks and callbacks.

## Rendering: remaining load-time work

**Priority:** medium.

### Parallel texture decode

**Status:** done. `include/gxm/decode_bands.h` splits a decode into
destination-row bands (aligned to the decoder's unit: 32-row Morton tiles, 4x4
blocks) across `VITA2NATIVE_DECODE_THREADS` workers (default 3, `1` is the A/B
control), skipping levels below 512x512. Applied to the swizzled U4/U8 tile
decoders, the swizzled U4 RGBA4 row decoder and PVRTC-I 4bpp.

Measured on the R36S with `texture_smoke --bench`, 1 thread -> 3:

| decoder | 1 thread | 3 threads |
| --- | --- | --- |
| u4 swizzled | 95.6 ms | 35.8 ms |
| u4 swizzled rgba4 ABGR | 128.9 ms | 57.9 ms |
| u4 swizzled rgba4 RGBA | 59.9 ms | 30.7 ms |
| u8 swizzled | 246.3 ms | 86.9 ms |
| pvrtc4 512 | 21.7 ms | 8.3 ms |
| pvrtc4 1024 | 87.1 ms | 33.3 ms |

Linear and PVRTC-II paths are unchanged (2.6-2.8x on the banded ones).
`texture_smoke` compares every banded decode against the reference decoder, so
the split is verified per-pixel, not just by timing.

**Found while measuring:** Iconoclasts' intro spends 65-83 ms per 640x360 video
texture, which is ~280 ns/pixel against 2.6-22 ns/pixel for the fast decoders -
those frames do not take any of the paths above. Worth a separate look; it is
the largest decode cost seen in that title.

### Async decode-ahead

A larger follow-up: start the decode when `sceGxmSetFragmentTexture` is called
and fence before the draw, or upload on a shared-context worker thread. This
can overlap decode/upload with guest work, but needs request tracking, guest
memory safety and cache-eviction ordering. Only attempt after the band-split
pool has been measured.

### 2bpp PVRTC decode

The optimised decoder only covers PVRTC-I 4bpp. 2bpp and PVRTC-II still use the
PowerVR SDK reference (~187 ns/pixel on the Orange Pi). Extend the fast decoder
only if a title is measured using those formats.

### Texture re-decode on cache eviction

The texture cache is byte-bounded and re-decodes evicted atlases on later
frames (previously seen at frames 4153/4155/4289/4308, ~160 ms each). Options:
raise the budget on devices with enough RAM, drop mip levels from the cache
footprint, or persist decoded textures on disk keyed by content hash. Measure
first whether the current level-load path still evicts.

### Orange Pi test-box stability

Angry Birds level loads hard-hang the Orange Pi (SSH banner times out, memory
watchdog cannot run) with `VITA2NATIVE_TEXTURE_CACHE_MB=128` and no
`VITA2NATIVE_FINISH_PER_FRAME`. The R36S survives the same path. If the Pi is
used for in-game testing again, run it with `VITA2NATIVE_FINISH_PER_FRAME=1`
and a cgroup memory cap, and keep logs on disk rather than tmpfs.

## Input: use the R36S Fn key as a Home/debug key

**Priority:** low.

**Status:** `BTN_TRIGGER_HAPPY5` (the R36S Fn key) is currently ignored by
`source/modules/SceCtrl_null.c`. It is a firmware hotkey modifier and has no
Vita equivalent, so forwarding it as Select/Start was wrong.

**Idea:** expose it as a host-side Home key. A short press could toggle a small
debug pane (FPS graph, frame profiler, input viewer, texture-cache stats); a
long press or Fn+Start could exit the game. Keep the key out of the guest's
`SceCtrlData` so titles never see a phantom button.

**Open questions:**

- Does ArkOS consume Fn combinations before evdev sees them?
- Should the pane be host-rendered like `fps_overlay.c` (composited into the
  guest framebuffer) or drawn by a guest overlay?
- Which gesture should open the menu without colliding with the existing Fn
  hotkeys (brightness, volume, etc.)?
