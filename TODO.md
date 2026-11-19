# vita2native — open work

Open work only. Open defects and performance candidates belong in
[`BUG_REPORTS.md`](BUG_REPORTS.md); rendering traps belong in
[`AGENTS.md`](AGENTS.md); what the emulator supports today is in
[`README.md`](README.md). This file is a work list, not a log: when something is
finished, delete it; when something is learned, record it where it belongs.

## 1. Open work

### 1.1 Audio: NGS fidelity gaps

**Priority:** high. **Status:** the graph renders; the gaps are fidelity.

`source/modules/SceNgs_render.c` renders what the titles in hand use: ATRAC9
voices, the HEVAG/PCM player (Simple voices), patch volume matrices, buffer
switch/end-of-data callbacks and the master buss. The guest pulls mixed S16
frames with `sceNgsVoiceGetStateData` and feeds `sceAudioOutOutput`;
`VITA2NATIVE_AUDIO_DEBUG=1` reports peaks.

Still missing:

- The EQ/envelope/pauser/filter modules after the source module are pass-through.
  Presets and their parameter blocks are ignored; Angry Birds Trilogy never calls
  `sceNgsVoiceSetPreset`, but other titles may.
- Only the master buss is mixed; chained non-master busses and the extra
  Simple/ATRAC9 outputs are ignored.
- HEVAG/PCM is resampled to the device rate with linear interpolation; ATRAC9 is
  assumed to be 48 kHz.
- `SceAvPlayer` audio returns zeroed buffers, so movie audio is silent by design.

`SceAudio` queues four device buffers per port; `VITA2NATIVE_NGS_DEBUG=1` logs
the graph, parameter dumps, master peaks and callbacks.

### 1.2 Angry Birds Star Wars: guest infinite loop from a corrupt heap link

**Priority:** high. **Status:** unexplained; the emulator's GL path is ruled out.

The title intermittently wedges on a Play press (~2 of 6 attempts). Signature:
one guest thread (`g21:<main>`) burns exactly one core while every other thread,
including the software rasteriser workers, is asleep, and the log stops dead.
PC sits in one 28-byte loop.

The guest's own allocator splits a block and writes a remainder node whose
`next` is `0xff000000`; `base + 0xff000000` wraps below the block, so the free
list walk never reaches its terminator. Whether that lands on unmapped memory
(`SIGSEGV`) or on mapped zeroes (silent spin) depends only on where the block
landed that boot, which is why it is intermittent.

Ruled out with evidence: the GL path (reproduced with the null GPU backend),
leaked instances, save-data contamination, input delivery, missing assets,
dirty heap memory, and block placement outside `VITA_MEMBLOCK_LIMIT` (crash
bases both inside and outside the range fail identically).

Next step: log every free of that block inside the title's allocator and see
whether it is released twice, which would make the split operate on live texture
memory.

### 1.3 Rendering: load-time work

**Priority:** medium.

Open:

- **Async decode-ahead.** Start the decode when `sceGxmSetFragmentTexture` is
  called and fence before the draw, or upload on a shared-context worker thread.
  Overlaps decode/upload with guest work, but needs request tracking, guest
  memory safety and cache-eviction ordering. Attempt only after the band split
  has been measured against a real level load.
- **2bpp PVRTC decode.** The optimised decoder covers PVRTC-I 4bpp only; 2bpp and
  PVRTC-II use the PowerVR reference (~187 ns/pixel). Extend only if a title is
  measured using those formats.
- **Texture re-decode on cache eviction.** The cache is byte-bounded and
  re-decodes evicted atlases later (seen at ~160 ms each). Options: raise the
  budget where RAM allows, drop mip levels from the cache footprint, or persist
  decoded textures on disk keyed by content hash. Measure first whether the
  current level-load path still evicts.
- **Load-time hang on a 2 GB board.** Angry Birds level loads hard-hang a 2 GB
  board (SSH banner times out, watchdog cannot run) with a smaller texture-cache
  budget and no `FINISH_PER_FRAME`; the same path survives on a device with more
  RAM. If such a board is used for in-game testing, run with
  `VITA2NATIVE_FINISH_PER_FRAME=1`, a cgroup memory cap, and logs on disk rather
  than tmpfs.

Closed, kept because the numbers are the baseline:

- **Parallel texture decode** (`include/gxm/decode_bands.h`): destination-row
  bands aligned to the decoder's unit, across `VITA2NATIVE_DECODE_THREADS`
  workers. The default follows the CPUs the process may run on (affinity mask):
  all of them on one or two cores, one fewer on three or more, capped at 4 where
  the decoders turn memory-bound; `1` is the A/B control. Banded: swizzled U4/U8
  tiles, swizzled U4 RGBA4 rows, PVRTC-I 4bpp. 1 -> 3 threads: u4 swizzled
  95.6 -> 35.8 ms, u4 rgba4 ABGR 128.9 -> 57.9 ms, u8 swizzled 246.3 -> 86.9 ms,
  pvrtc4 1024 87.1 -> 33.3 ms.
- **Palettised decode** (`decode_paletted_fast`): P8/P4 frames went through a
  per-pixel format rebuild and palette unpack. A 256-entry LUT built once per
  level fixed it: 800x360 P4 85.6 -> 4.0 ms, 256x256 P4 swizzled 25.0 -> ~6 ms.
- **Four-byte texels** are a shift-based permutation rather than a per-byte loop
  for the linear ARGB/1RGB swizzles (41 -> 11.6 ns/pixel by bench); Morton
  layouts the tile walk rejects (non-power-of-two dimensions) get the fast
  per-texel body.

### 1.4 Remaining frame budget under software rendering

**Priority:** medium.

With the depth-store and uniform/texture findings landed, a software-rendered
3D frame at 960x544 is left with roughly:

```
finish   95 ms   glFinish at EndScene (the GPU render)
read     54 ms   glReadPixels of the 960x544 colour surface
draw     24 ms   uniform 8.5, tex 2.5, flush 8, gldraw 5
resolve   5 ms
```

Candidates, in order:

1. **`finish` + `read` (~149 ms).** A PBO readback was measured *slower* on
   Panfrost, so it needs its own test rather than an assumption. The `glFinish`
   is redundant with the read and is the cheaper experiment.
2. **`draw` at ~24 ms.** Uniform re-upload per draw and rebinding all 32 texture
   units per draw are already memoised; the rest is draw submission itself.
3. **Batching across blend states.** Iconoclasts alternates blend per sprite, so
   ~103 GL draws/frame are CPU-bound on submission. The design for merging them
   is in `BUG_REPORTS.md`; it needs the blend parameters in the per-draw
   channel, not in uniforms.

### 1.5 Diagnostics and structure

**Priority:** medium.

- **Standardize diagnostics on `LOG`.** Retire the raw
  `fprintf(stderr, ...)` sites in modules (~58 today) on one level-aware macro,
  and make every `*_trace()` helper cache its `getenv` like the older ones (the
  `VITA2NATIVE_TRACE_VIEWPORT` check currently runs on every state application).
- **Split `SceGxm.c`** (~10.7k lines) into `gxm_state.c`, `gxm_draw.c`,
  `gxm_texture.c`, `gxm_present.c`, `gxm_diag.c`.

### 1.6 Input: a pad firmware hotkey as a host debug key

**Priority:** low.

A pad's firmware hotkey modifier (`BTN_TRIGGER_HAPPY*`) is correctly ignored as
a Vita button. Idea: expose it as a host-side Home key — short press toggles a
debug pane (FPS graph, frame profiler, input viewer, texture-cache stats), long
press or hotkey+Start exits the game, and it never reaches `SceCtrlData`.

Open questions: whether the firmware consumes its combinations before evdev sees
them; whether the pane should be host-rendered like `modules/fps_overlay.c` or drawn as a
guest overlay; which gesture avoids existing brightness/volume hotkeys.

### 1.7 Security posture

**Priority:** low.

Guest code is native code in the host process: it can execute raw Linux
`svc #0`. Do not claim isolation. Write down what a deployment should do
(seccomp-bpf, bubblewrap/landlock, no untrusted titles) rather than implying the
emulator contains the guest.

## 2. Architecture decisions (locked for this fork)

- **Target baseline:** ARMv7-A, NEON, hard-float (`armhf`), 32-bit userland.
  ARMv6/no-NEON cannot safely execute Vita code.
- **OS baseline:** Linux with POSIX threads, `mmap`, EGL/GLES. SDL2 is the first
  portability layer for window, input and audio.
- **Graphics:** a `gfx` abstraction with a null backend (CPU-only homebrew) and a
  **GLES 3.1** backend. Do not port deko3d or UAM.
- **Shaders:** keep Vita3K's GXP recompiler; emit SPIR-V (`SpirVOpenGL`) and
  translate to GLSL ES 3.10/3.20 with SPIRV-Cross. Replace UAM with
  `glCompileShader` plus a program cache. GLES 3.1 guarantees no vertex SSBOs,
  so do not require them.
- **Guest ABI:** the host is compiled hard-float, as is the default VitaSDK
  toolchain and every caller seen so far, so float-taking exports use the host
  ABI directly. An entry point that a base-AAPCS caller needs is declared with
  `EXPORT_SOFTFP` in `include/module.h`, which pins that export alone to
  `pcs("aapcs")`.
- **Runtime filesystem:** Vita device prefixes map into a host root
  (`vita2native/`, `--root` later). Never let guest paths escape it.
- **Horizon OS code is gone.** Linux/ARM32 is the only supported host.

## 3. Known limitations

Behaviour that is deliberately incomplete, so a later reader does not treat it as
a bug:

- **Tiled and swizzled GXM transfers** return `SCE_GXM_ERROR_UNSUPPORTED`;
  `sceGxmTransferCopy` implements linear-to-linear with the PASS and REJECT
  colour-key modes only.
- **Negative transfer strides** are handled but no test exercises a bottom-up
  surface.
- **Shared-framebuffer / system-app path** (`sceSharedFb*`) does not exist: this
  host has no compositor arbitration, so `_sceSharedFbOpen` fails by design and
  callers fall back to their own render target.
- **Encrypted SELF** is not supported; only decrypted ELF/SELF loads.
- **`sceGu` / sceGum** are not implemented.
- **Network errno translation** covers the common errors; anything outside the
  table passes through as the Linux value.
- **Guest libc errno** copies the host value as-is, so where Linux and the Vita's
  libc number an error differently the guest sees the host number.
- **`sceAudioOutSetConfig`** restarts the stream: frames queued before the
  reconfiguration are dropped.
- **VPK identification** fingerprints the archive's first and last 64 KiB; a
  deliberate collision is possible in principle.
- **Save data** is namespaced per title (`savedata/<title-id>/`) with no
  migration path, by design — nothing was released when the change landed.
- **`ScePgf`** implements synthetic fonts and glyph paths but does not parse the
  PGF container.
