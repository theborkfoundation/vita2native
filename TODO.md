# vita2native — plan, open work and findings

One document for the project: the port plan and its locked architecture (1-9),
what is still open (10), the behaviour we accept as incomplete (11), and a dated
log of what was found, fixed and ruled out. It is tracked in the repository — the
old `PLAN.md` working copy and `TODO.md` were merged into it.

Read section 10 first if you are picking up work, and the dated log if you are
chasing a behaviour that looks wrong: the why is usually recorded there with the
measurement that established it.

## 1. Goal

Change the host platform from Horizon OS (Nintendo Switch) to a typical
**32-bit ARM Linux** system, while keeping the core idea: run PlayStation Vita
ARMv7 executables natively by resolving Vita module imports to host routines.

This is a translation layer, not a CPU emulator. On ARM32 Linux the Vita code
runs in the same instruction set, so the Switch "JIT" only needs to become
executable-memory allocation.

### Status (2026-11-06)

- **Guest float ABI corrected.** The default VitaSDK is hard-float, not softfp;
  exports were compiled with `pcs("aapcs")`, so a hard-float guest's
  `sceGxmSetViewport` read garbage and rendered a black frame. `GUEST_SOFTFP_ABI`
  is now empty by default and `-DVITA2NATIVE_SOFTFP=ON` builds the old ABI for
  legacy homebrew. SuDokuL now renders and animates on the Orange Pi over X11.
- `sceKernelCreateThread` accepts priority 0 as "same as the calling thread",
  which is what SDL2's Vita thread wrapper passes; previously it returned
  `SCE_KERNEL_ERROR_ILLEGAL_PRIORITY` and SDL init failed.

- `sceIoDread` now filters the host filesystem's `.` and `..` entries. VitaShell
  adds its own `..` row and walks every returned name, so the old code showed
  duplicate `../` rows (plus `./`) and could make a recursive copy/remove walk
  into the parent directory. `SceFios2` already filtered them; the SceLibKernel
  path did not.

- SDL now owns input as well as video. The video thread polls the keyboard and
  game controllers every 16 ms (also while no frame is pending), maps them to
  `SCE_CTRL_*` and latches presses until the guest peeks. Pads SDL has no game
  controller mapping for fall back to raw `SDL_Joystick` buttons/axes. Hotplug
  is rescanned once a second. The same poll drives the front touch panel through
  the presentation letterbox. Validated end to end on x86_64 (Xvfb + xdotool for
  keyboard and pointer, `uinput_pad` for a controller) and on ARM (X11 under
  Xvfb: `SceCtrl peek` logged the injected cross/circle/square/dpad presses).

- `SceFios2`: synchronous file/dir/stat/date/filter surface implemented on the
  host filesystem; every 3.60 NID resolves (unsupported ones return
  `SCE_FIOS_ERROR_UNSUPPORTED`). `fios2_smoke` passes on x86_64 and ARM32.
- `SceSqlite`: **173/173 NIDs resolve**, backed by the vendored SQLite 3.11.1
  amalgamation. The `psp2` VFS translates Vita device paths into the host root
  and matches Sony's `sqlite3_file` + `int *fd` layout, so the guest VFS wrapper
  VitaShell registers (`psp2_rw`, copying the default VFS and replacing
  `xOpen`/`xDelete`) works through native code. `sqlite_smoke` covers create,
  bind/step, column access, persistence, the guest wrapper and delete on x86_64
  and ARM32.
- `SceCommonDialog`: **119/119 NIDs resolve**. Message dialogs and the IME
  complete immediately (the IME accepts its initial text); dialog families
  that need PSN, camera, photo or store services fail Init with
  `SCE_COMMON_DIALOG_ERROR_NOT_SUPPORTED` so callers take their fallback path.
- `SceProcessmgr`: **31/31 NIDs resolve**, including real process identity,
  libc clock/time/«struct tm» wrappers and termination callbacks.
- `SceTouch`: **27/27 NIDs resolve**. The front panel is driven by the host
  pointer (mouse left button) over the presentation window and scaled into the
  1920x1088 active area; the back panel has no host source. `touch_smoke`
  covers panel info, sampling state, peek/read, regions and pixel density.
- **Coverage:** the 34 libraries targeted before this milestone are at
  **1951/1951 NIDs**. `SceHttp` (93/93), `SceSsl` (11/11) and `SceNetCtl`
  (18/18) are complete. `SceCamera` (5/45), `SceVshBridge` (3/206),
  `SceRegMgr` (1/22), `SceNpDrm` (2/8) and `SceAppMgr` (3/191) declare only
  the entry points real homebrew imports; the rest falls back to the loader's
  unresolved-import stub.
- `SceHttp`/`SceSsl`: a real HTTP/1.1 client (status/headers/body, redirects,
  chunked transfer) over POSIX sockets. HTTPS uses the host `libssl.so.3` via
  `dlopen` — no build-time TLS dependency; without it HTTPS fails with
  `SCE_HTTP_ERROR_SSL` and plain HTTP still works. Verified against
  `https://example.com` from both x86_64 and ARM32 (`http_smoke`).
- `SceThreadmgr`: **173/173 NIDs resolve**. Signals, VM hooks, priority/VFP
  attributes, UID classification, lightweight mutex/cond introspection, the
  underscore message-pipe vector calls and `ForUser` thread creation are
  implemented; `threadmgr_smoke` passes on x86_64 and ARM32 under qemu.
  VM suspend/resume only tracks a counter (the host cannot preempt a thread),
  and VM context writes are accepted but ignored — no Mono runtime uses them.
- `SceLibKernel`: **299/299 NIDs resolve**. The remaining diagnostics/SBL
  entry points are safe stubs; `sceSblACMgrIsGameProgram` reports true and
  `sceKernelCallWithChangeStack` returns `EINVAL` instead of guessing at the
  callback ABI.
- `ScePgf`: synthetic font with real metrics and 8-bit glyph images from the
  public-domain 8x8 VGA font, scaled to 12x16 so it matches the layout width
  VitaShell's system-font metrics assume. System-font open/find/info/char-info/
  glyph paths are implemented; the PGF container itself is not parsed.
- `sceIoDevctl(0x3001)` reports real host `statvfs` capacity, so file managers
  show device free/total sizes instead of `-`.
- Loader: handles zero-length segments; unresolved imports log the stub address.
- `SceGxm` surface modes: colour-surface gamma encoding, texture gamma
  decoding, ordered dithering for 16-bit colour formats and the MSAA surface
  scale modes are implemented. `gxm_draw_smoke` covers gamma round trips,
  dithered versus uniform 565 writeback and both MSAA scale modes on x86_64
  and ARM32/softpipe.
- `sceGxmTextureSetGammaMode` had been rejecting every value because its
  validation mask tested bits 29-30 instead of the gamma bits 27-28; fixed.
- `SceRegMgr`/`SceRegMgrForGame`: the registry is now host-backed (in-memory,
  case-insensitive category/name lookup, `language` and `button_assign`
  defaults). Get/Set for int, string and binary values round-trip; unknown keys
  fail with -1 and a zeroed buffer so applications keep their own fallbacks.
  All 22 `SceRegMgr` and 7 `SceRegMgrForGame` NIDs resolve; `regmgr_smoke`
  covers the surface on x86_64 and ARM32.
- Headless testing: `VITA2NATIVE_CTRL_SCRIPT` drives the pad from a
  display-frame-keyed script and `VITA2NATIVE_DUMP_FRAME_AT` accepts a list of
  frames (with `%u` path substitution). `ctrl_script_smoke` covers the parser.
  VitaShell navigation under qemu/softpipe is impractical (a single 960x544
  frame takes minutes), so real-app input validation stays deferred.
- Optional real-app libraries now resolve instead of falling back to the
  loader's error stub: `ScePspnetAdhoc` 29/29 (empty network, socket operations
  return "not connected"), `SceMusicExport`/`SceVideoExport` (unavailable),
  `ScePhotoExport` 4/4, `ScePromoterUtil` 10/10 declared entry points,
  `SceUsbstorVStor` 4/4, `SceMtpIf` start/stop, and `SceUdcd` 6/6 (cable
  disconnected). `extra_imports_smoke` checks both the behaviour and the export
  table NIDs on x86_64 and ARM32.
- `SceJpegUser` (11/11), `SceJpegEncUser` (10/10) and the three declared
  `SceJpegArm` entry points are implemented on the vendored stb_image decoder
  and stb_image_write encoder: SOF parsing for dimensions/sampling, output-info
  pitches, RGBA and YCbCr decoded output (with 1/2, 1/4 and 1/8 downscale),
  YUV444/420 conversion, and JPEG encoding from ARGB8888 or YCbCr input.
  `jpeg_smoke` covers the export NIDs, output info, both decode paths, CSC and
  an encoder round trip on x86_64 and ARM32.
- `SceCamera`: all 45 user NIDs resolve. Device 0/1 present a synthetic
  SMPTE-style colour-bar feed with a moving marker, supporting YUV420/YUV422
  planes, packed YUYV, ARGB, ABGR and RAW8, plus the full control set
  (controls are stored and read back). `camera_smoke` covers the lifecycle,
  both read formats, controls and the export table.
- `SceAudiodec` AAC: backed by the vendored FAAD2 decoder (GPLv2). ADTS
  streams decode to 16-bit PCM, including the priming frames FAAD returns with
  no samples. `audiodec_smoke` decodes a real 44.1 kHz stereo ADTS sample on
  x86_64 and ARM32. CELP remains unsupported (no suitable decoder source).
- `SceAvPlayer`: all 20 NIDs resolve. Playback is synthetic: a generated
  30 fps 960x544 RGBA colour-bar feed with a moving marker, silent 48 kHz
  stereo audio, host-time-based current time, pause/resume/stop, looping, jump,
  trick speed, stream enable/disable and the memory/allocator callbacks.
  `avplayer_smoke` covers the stream info, frame pulls and lifecycle.
- Real-title coverage: VitaSudoku, Minesweeper, Pong, ElevenMPV and the
  VitaSDK AvPlayer sample load with **zero unresolved imports**. Pong, Minesweeper
  and VitaSudoku render end-to-end (`build/evidence/pong.png`,
  `build/evidence/minesweeper.png`, `build/evidence/sudoku2.png`). VitaSudoku
  needs `ux0:/data/VitaSudoku` to exist; its Lua script errors otherwise. This
  surfaced two legacy VitaSDK library aliases (`SceLibRng` and
  `SceThreadmgrCoredumpTime` with NID 0x52) that are now exported.
- `VitaShell.vpk` boots through module load and font-cache construction into
  its first frame. Softpipe under qemu makes each shader variant ~9 s on first
  use, so a full frame can take minutes.

## 2. Architecture decisions (locked for this fork)

- **Target baseline:** ARMv7-A, NEON, hard-float (`armhf`), 32-bit userland.
- **OS baseline:** Linux with POSIX threads, `mmap`, EGL/GLES. SDL2 is the
  first portability layer for window, input, and later audio.
- **Graphics:** new `gfx` abstraction. Start with a **null backend** so
  CPU-only Vita homebrew runs; then add a **GLES 3.1** backend. Do not try to
  port deko3d or UAM.
- **Shaders:** keep Vita3K's GXP recompiler. Emit SPIR-V (`SpirVOpenGL`) and
  translate to GLSL ES 3.10/3.20 with SPIRV-Cross. Replace UAM with
  `glCompileShader` + a program cache.
- **Guest ABI:** host is compiled armhf (`-march=armv7-a -mfpu=neon
  -mfloat-abi=hard -mthumb`). The default VitaSDK toolchain is also hard-float,
  so float-taking exports use the host ABI directly. Homebrew built with the
  old base-AAPCS/softfp profile can be supported with
  `-DVITA2NATIVE_SOFTFP=ON`, which restores `GUEST_SOFTFP_ABI`
  (`pcs("aapcs")`) on those exports.
- **Runtime filesystem:** Vita device prefixes are mapped into a host root
  directory (`vita2native/` by default, `--root` later). Never let guest paths
  escape that root.
- **Horizon OS code is gone.** The Switch/deko3d/libnx target, its backends and
  build options were removed; Linux/ARM32 is the only supported host.
- **Security:** guest code is native code in the host process. Document this;
  consider seccomp/bubblewrap later. Do not claim isolation.

## 3. Target requirements and reference hardware

| Requirement | Reason |
| --- | --- |
| 32-bit ARMv7-A userland | Guest pointers/ABI are 32-bit; requires 32-bit address space. |
| NEON + VFPv3/v4, hard-float | VitaSDK default flags: `--with-arch=armv7-a --with-tune=cortex-a9 --with-fpu=neon --with-float=hard --with-mode=thumb`. |
| Linux + EGL + GLES | 3D path. GLES 3.1 for shaders; **no vertex SSBO required** (GLES 3.1 only guarantees 0 vertex storage blocks, and Panfrost/Mali reports 0). |
| Not ARMv6/no-NEON | Pi 1/Zero cannot safely execute Vita ARMv7/NEON code. |

Good references:

- Raspberry Pi 4/5 with 32-bit OS: Mesa V3D, GLES 3.1 conformant.
- Orange Pi Zero 3 (H618, Mali-G31/Panfrost, armhf userland on aarch64 kernel): validated, see below.
- Odroid XU4 / modern ARM32 handheld SoCs: Mali, GLES 3.x.
- Avoid Pi 0–3 for 3D: Mesa VC4 is GLES 2.0 only.
- Pi 4/5 can also run 64-bit, but this project must be built 32-bit.

## 4. Current host couplings

| Concern | Current code | Linux replacement |
| --- | --- | --- |
| Device/GPU init | `source/main.c` deko3d device | SDL2/EGL or null device |
| Executable memory | `source/load.c` `Jit` | `memfd_create` + RW/RX mappings, or `mmap`/`mprotect` |
| GPU memory | `SceSysmem` + `DkMemBlock` | CPU allocations + shadow GL resources (later) |
| Threads/TLS/sync | libnx `Thread`, `Mutex`, `CondVar`, `UEvent`, `LEvent`, `RwLock`, `waitSingle` | POSIX pthread shim |
| File I/O | `SceLibKernel` strips `ux0:` and calls `fopen` | Vita mount-point VFS rooted in host directory |
| Display | `SceDisplay` + `DkSwapchain` | SDL2 renderer/texture first, GLES later |
| Controller/touch | libnx `pad`/`hid` | SDL2 game controller + keyboard; touch later |
| GXM | deko3d command buffers/images/shaders | new GLES 3.1 backend |
| Shaders | Vita3K recompiler + UAM | Vita3K recompiler + SPIRV-Cross GLSL ES |
| Build | devkitARM/libnx32/.nsp | armhf CMake executable |
| Logging | `svcOutputDebugString`, libnx applet error | stderr/file/netlog |

## 5. Milestones

### Phase 0 — Planning and scaffolding (done)

- [x] Write this plan.
- [x] Add `PLAN.md` to `.gitignore`.
- [x] Add `host` POSIX compatibility layer (threads, events, TLS, time).
- [x] Add Linux `Jit` implementation.
- [x] Add Vita mount-point VFS.
- [x] Add null GPU backend and CPU-only GXM stub.
- [x] Add SDL2 display and controller implementations.
- [x] Add Linux CMake branch and presets.
- [x] Add `VITA2NATIVE_HEADLESS` null display/input variant.
- [x] Verify an armhf build with Zig's `arm-linux-gnueabihf` target
      (headless; linked ELF32 ARM executable).
- [x] Run the host-layer smoke test natively.
- [x] Run the armhf `host_smoke` in the x86_64 Ubuntu VM under `qemu-arm-static`.
- [x] Run a synthetic CPU-only guest ELF end-to-end (loader, relocations,
      import patching, `sceKernelExitProcess`, guest-main return).
- [ ] Verify a full armhf build on a real Linux ARM32 host (Pi 4).

### Phase 1 — Headless loader and runtime

- [x] Verify ELF reloading and `PT_SCE_RELA` relocations (`R_ARM_ABS32`).
- [x] Verify import patching (`movw`/`movt`/`bx` stubs) against real exports.
- [x] Test thread create/start/join/TLS and `sceKernelExitProcess`.
- [x] Test the guest main thread returning normally (must end the process).
- [x] Test file I/O through `ux0:`/`app0:` with a sample.
- [x] `sceIoGetstat`, `sceIoGetstatByFd`, `sceIoMkdir`, `sceIoRmdir`,
      `sceIoRemove`, `sceIoRename`, `sceIoChstat`, 64-bit `sceIoLseek`.
- [x] Add VPK extraction/mount for `app0:` assets (extracted to
      `vita2native/app0`, cached by archive size + mtime, zip-slip filtered).
- [ ] Verify VELF/SELF loading and cache flush on real samples.
- [x] Add signal handlers that dump guest ARM registers on crash.

### Phase 2 — Display and input

- [x] SDL2 framebuffer presenter.
- [x] SDL2 game controller + keyboard fallback.
- [x] Null display/input backend for headless runs and local QEMU testing.
- [x] EGL window presentation: the GLES device opens a window-capable platform
      display, attaches the SDL/X11 window and swaps directly from
      `sceDisplaySetFrameBuf` (letterboxed, with the SDL texture path as the
      fallback for Wayland or when EGL cannot present).
- [x] Runtime `--headless` option, also inferred when `DISPLAY` and
      `WAYLAND_DISPLAY` are unset.
- [x] Touchscreen via the SDL pointer (headless builds have no touch source).
- [ ] Verify framebuffer pixel format and pitch on real hardware.

### Phase 3 — GXM on GLES

- [x] Define the `gfx` device API (`include/gfx.h`).
- [x] GLES 3.x device/context: EGL + GLESv2 loaded with `dlopen`, surfaceless
      platform with a pbuffer, runtime fallback to the null backend,
      `gfx_smoke` clear/readback test.
- [x] Offscreen RGBA8 render target (FBO) at the Vita resolution, frame
      begin/end, full-surface readback.
- [x] Window surface + swapchain (X11 via SDL native handles, Wayland falls
      back to the SDL texture path); `egl_window_smoke` verifies an actual X11
      window with XGetImage under Xvfb.
- [x] Port the full `SceGxm` entry-point surface to GLES:
  - GXP -> SPIR-V (Vita3K) -> GLSL ES 3.10 (SPIRV-Cross), shader objects and
    lazily linked vertex/fragment program pairs
  - render targets (FBO + depth/stencil renderbuffer), scenes, viewport,
    depth/stencil/blend state, vertex streams, index buffers, draw
  - default uniforms as SSBOs 0/1, render-info UBOs 2/3, samplers at texture
    unit bindings, `sceGxmSetUniformDataF` packing
  - `sceGxmEndScene` reads the color target back into the guest color surface
    (A8B8G8R8 conversion + vertical flip) for `sceDisplaySetFrameBuf`
- [x] Morton-swizzled texture decode and CPU conversion to RGBA8
      (all uncompressed formats: 1/2/3/4 components, 4/8/16/24/32/64 bpp,
      packed 4444/1555/565/U8U3U3U2, float16/float32 and 10/11-bit floats,
      plus every GXM channel swizzle).
- [x] Paletted texture formats (P4/P8 with A8B8G8R8 palettes).
- [x] Mip chains, cube maps, tiled (32x32) and linear-strided layouts.
- [x] Full render state: viewport, region clip/scissor, cull mode, polygon
      modes (fill/line/point), depth bias, two-sided and fragment-program
      enable flags.
- [x] Color/depth surface query API, disabled surfaces and notifications.
- [x] Program reflection API, shader patcher reference counting, precomputed
      draws/states, instanced draws, transfer fill/copy/downscale.
- [x] Vertex textures and named uniform buffers (reflective SSBO layout).
- [x] End-to-end draw test with real GXP shaders and real GLES
      (`tests/gxm_draw_smoke.c`, x86_64 Mesa llvmpipe): textured quad,
      viewport/clip, texture sampling and wireframe all verified.
- [x] Depth/stencil surfaces: force load/store through a GL depth/stencil
      texture, background clear, per-format depth and stencil tests (DF32,
      S8D24 packed), ZLS control encoding fixed to the hardware layout.
- [x] Deferred contexts and command lists: draws are snapshotted (programs,
      streams, textures, uniforms, render state) and replayed on the immediate
      context; buffer getters/setters return the guest ring buffers.
- [x] Tiled colour surfaces: 32x32 tile writeback matching tiled texture
      sampling (round-trip test).
- [x] Swizzled (square POT) colour surfaces: Morton writeback matching
      swizzled texture sampling (round-trip test).
- [x] Colour surface clip honoured on writeback.
- [x] 2x/4x multisampled render targets: MSAA renderbuffers plus a resolve
      blit into the single-sample FBO at `sceGxmEndScene`.
- [x] On-disk GLSL ES shader cache under `<vfs root>/cache/shaders`
      (versioned hash of the GXP bytes, stage and vertex attribute bindings).
- [x] Guest-ABI draw fixture (`guest_gxm_draw.elf`): 21 imported SceGxm
      functions, stack arguments, shader patcher, streams, uniforms and draw;
      verified on ARM32/qemu.
- [x] GXM draw-path validation with a real VitaSDK sample: PixelChess (a
      libvita2d title) loads, initialises GXM and renders its title screen on
      ARM32/qemu. Frame captured to `build/evidence/pixelchess-frame.png`.
- [x] Tiled depth/stencil surfaces: force load and store use the same 32x32
      tile order as tiled colour surfaces. The guest does not inspect the raw
      depth layout, so a self-consistent mapping is sufficient for rendering.
- [x] Colour surface gamma/dither modes and texture gamma modes: gamma surfaces
      encode linear shader output to sRGB at writeback, gamma textures are
      decoded to linear during upload, and 16-bit formats use an ordered 4x4
      Bayer dither when the surface asks for it. Because the public headers
      alias `SCE_GXM_*_GAMMA_BGR` to the `_R` constant, every non-zero gamma
      mode is treated as full sRGB (the same choice Vita3K makes).
- [x] MSAA surface scale modes: `SCALE_MSAA_DOWNSCALE` writes the resolved
      image at render-target size; `SCALE_NONE` on a doubled surface receives
      each resolved pixel replicated into its 2x2 block, so a subsequent
      `sceGxmTransferDownscale` reconstructs the resolved image. The requested
      sample count is kept even when the driver cannot create a multisampled
      framebuffer (softpipe), so the surface layout stays consistent.

### Phase 3b — Running real homebrew

- [x] Per-thread EGL contexts (guest threads are host threads).
- [x] Contexts smaller than our `SceGxmContext` are allocated on the host
      (real titles pass `SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE` = 2 KiB).
- [x] Modules imported by real titles: `SceAppMgrUser`, `SceSharedFb`,
      `SceCommonDialog`, `SceSysmodule`, `SceRtcUser`, `SceNet`,
      `SceGxmInternalForVsh`, plus mutexes/message pipes/thread info.
- [x] `sceAppMgrGetBudgetInfo` fails on purpose so libvita2d takes its normal
      render-target path instead of the shared-framebuffer one.
- [x] Headless frame capture (`VITA2NATIVE_DUMP_FRAME`, `..._AT`) for tests
      that have no window.
- [ ] Real titles beyond libvita2d (GXM direct, sceGu, engine-based).
- [ ] Shared-framebuffer / system-app path (`sceSharedFb*`).

### Phase 4 — Coverage

- [x] `SceAudioOut` via SDL2 (per-port queues, real blocking, volume,
      config). Headless builds pace by sleeping so guest timing is unchanged.
- [x] `SceAudioIn` port bookkeeping (silence; no microphone).
- [x] `SceRtc` (full tick/date arithmetic), `SceAppMgr` basics.
- [x] `SceNet` via POSIX sockets (BSD↔Linux flag/option/sockaddr translation).
- [x] More `SceIofilemgr` calls (`getstat`, `mkdir`, `remove`, `rename`).
- [x] Kernel sync primitives: semaphores, event flags, condition variables,
      lightweight mutexes.
- [x] `SceAppUtil`: save-data slots and files under `savedata0:`, data
      save/remove, quota, safe memory, system/app parameters.
- [x] `ScePower`: battery and clock queries and the no-op request/setter calls.
- [x] `SceShellUtil` (lock/unlock/events/launch hooks).
- [x] `sceClib*` libc shims (mem/str/printf/mspace, fortified variants,
      `__stack_chk_fail`, EABI divide-by-zero).
- [x] Controller variants (Peek/Read, Positive/Negative/Ext, sampling mode,
      port info, actuator).
- [x] `SceSysmem` model and free-memory queries.
- [x] `SceNet` resolver (`StartNtoa`/`StartAton` via getaddrinfo/getnameinfo).
- [x] `SceRtc` completed (RFC 2822/3339 format+parse, DOS/Win32 times).
- [x] Positioned file I/O (`sceIoPread`/`sceIoPwrite`) and `sceIoIoctl`.
- [x] Display and sysmodule libraries complete against db/360.
- [x] Unknown UID lookups return the firmware error instead of dereferencing
      NULL (threads, mutexes, semaphores, event flags, conds, memblocks,
      files, ATRAC/audio decoder handles).
- [x] `SceShellSvc` (the full 6-function surface, including launch requests).
- [x] `SceAudiodec` ATRAC9 decoding (real decoder, LibAtrac9) and `SceAtrac`
      streaming: decoder groups, RIFF/AT9 container parsing, decode from the
      guest buffer, loop configuration.
- [x] `SceNgs`: complete 68-function surface against db/360 (systems, racks,
      voices, patches, parameter locks, ATRAC9 section helper, Sulpha). The
      synthesis graph is bookkeeping with silent rendering; handles and routing
      are real, patches track their gain matrices, and every lifecycle call
      succeeds. `tests/ngs_smoke.c` covers the graph and `guest_ngs.elf`
      resolves every NGS import through a real guest stub.
- [x] Module manager: `sceKernelLoadModule`, `LoadStartModule`, `StartModule`,
      `StopModule`, `UnloadModule`, `StopUnloadModule`, open/close by path,
      module info/list/address queries and system-version queries. Loaded
      modules keep their own JIT/data allocations alive. Exported guest
      functions and variables are registered and resolved by later modules;
      `guest_module_export.elf` loads one module that imports from another.
- [x] `SceSysmem` query/VM surface: block lookup by address/name, info by
      address/range, `AllocMemBlockForVM`, VM domain open/close/sync.
- [x] Thread start/end event handlers dispatch to guest handlers;
      `guest_thread_event.elf` validates both events and unregistering.
- [x] Controller buffer aliases (the `*2` family), wireless info, rapid fire,
      light bar, battery and sampling-mode queries.
- [x] `SceIofilemgr` complete against db/360 (59/59): the async family
      (`sceIo*Async`, `sceIoComplete`/`Cancel`) completes synchronously and
      records results per fd, the `_sceIo*` option-block wrappers delegate to
      the public calls, priorities/locking are bookkeeping, and the guest
      fixture `guest_io_async.elf` exercises the whole lifecycle.
- [x] `SceLibKernel` non-I/O internals (thread module loading, atomics,
      timers, event callbacks) and the remaining `_sce*` entry points.
- [x] `SceLibKernel` runtime helpers: all 72 atomic exports (8/16/32/64-bit
      set/CAS/add/sub/and/or/xor/clear/exchange/dec-if-positive), process and
      system clocks, `sceKernelGetRandomNumber`, and `sceKernelGetOpenPsId`.
      `guest_kernel_runtime.elf` exercises them through real import stubs.
- [x] RW locks and lightweight condition variables: create/delete/open/close,
      read/write/try/unlock, cancel and info; LwCond shares the LwMutex host
      mutex. `guest_kernel_locks.elf` covers the lifecycle and NIDs.
- [x] Timers: create/open/close/delete, start/stop, set/get time (including
      wide variants), event interval and remaining-time queries, timer info.
      `guest_kernel_timer.elf` covers the clock arithmetic and all NIDs.
- [x] Thread info, priority and affinity queries.
- [x] Internal thread-manager aliases for semaphores, event flags, mutexes and
      condition variables now resolve to the same implementations; semaphores,
      event flags, mutexes, message pipes and condition variables can be opened
      by name and closed.
- [x] Callbacks registered against a simple event are notified when the event
      is set or pulsed.
- [x] Mutex ownership/count tracking with `sceKernelGetMutexInfo` and
      `sceKernelCancelMutex`; stack-check guard helpers exported.
- [x] `ScePower` callback unregister and wireless setter complete.
- [x] Thread callbacks: create/delete, notify/cancel, pending count, info and
      `sceKernelCheckCallback`; the callback executes guest code directly and
      runs outside the callback-table lock. Thread event handlers are stored
      and dispatched on guest thread start/end.
- [x] Message pipes completed: byte-stream send/receive, non-blocking and
      callback variants, vectors, open/close by name, cancel, info and creator
      query. `guest_thread_extra.elf` runs a real guest callback and a
      two-element vector round trip.
- [x] Simple events: create/open/close/delete, set/clear/pulse/cancel, wait,
      poll, pattern/info queries and the `WithNotifyCallback` entry points.
      Waiters use a host condition variable with real microsecond timeouts;
      `guest_kernel_event.elf` covers the lifecycle through import stubs.
- [x] Remaining thread-manager gaps: `sceKernelGetThreadEventInfo` is real;
      exception/signal waits complete immediately because the host has no
      guest exception delivery, and VM context calls are accepted as no-ops.
- [x] `SceMotion`: full stationary sensor model (state, raw sensor samples,
      device location, orientation, settings and sampling lifecycle).
- [x] `SceNet` epoll family on top of the host epoll API.
- [x] `SceCommonDialog` message-dialog lifecycle completes immediately, so
      titles polling for dialog status do not hang.
- [x] Guest/host float ABI boundary: float-taking exports use base AAPCS. Tested
      for GXM, NGS and SceMotion.
- [x] AAC (faad2), MP3 (minimp3) and ATRAC9 decoding; CELP is a silent
      stub so voice-only tracks do not abort titles.
- [x] SceFiber implemented on host ucontext.
- [x] VPK asset extraction for `app0:`.

### Phase 5 — Hardening and testing

- [x] Test armhf binaries in the Ubuntu x86_64 VM under `qemu-arm-static`.
      The 22 host smoke tests pass as a set on ARM32 (graphics tests use
      softpipe, which is required: llvmpipe crashes under qemu-arm).
- [ ] armhf build on real ARM32 hardware.
- [ ] Pi 4 GLES test.
- [ ] seccomp/bubblewrap guidance.
- [x] Packaging (`vita2native` binary, `.desktop` file optional).
      `cmake --install` now installs the binary, a `.desktop` entry, a VPK MIME
      type and the README/licence under the standard GNU directories.

## 6. Critical traps

1. **64-bit build cannot run the guest.** Enforce/verify 32-bit.
2. **armel/softfp host ABI mismatch.** Must be `armhf` for default VitaSDK
   binaries.
3. **ARMv6/no-NEON** → SIGILL.
4. **W^X:** use memfd dual mapping or RW→RX `mprotect`; don't assume RWX.
5. **Branch relocation range:** keep code and data in one arena where possible
   (`R_ARM_CALL`/`JUMP24`/`THM_PC22`).
6. **`ux0:` stripping currently writes to `/`.** VFS mapping is mandatory.
7. **Guest work areas:** `SceKernelLwMutexWork` is `0x20`; do not embed
   `pthread_mutex_t`/`pthread_cond_t` blindly. The host shim uses 4-byte
   handle IDs.
8. **GL context threading:** one context serialized, or one render thread with
   a command queue.
9. **Texture format/swizzle:** the removed deko3d backend treated everything as
   RGBA8; that will not survive real samples.
10. **GLES gaps:** no `glPolygonMode`, `glGetTexImage`, `glTextureBarrier`;
    SSBOs need GLES 3.1.
11. **Security:** guest code can execute raw Linux `svc #0`; no isolation.
12. **Encrypted SELF** remains unsupported.
13. **Missing modules:** only a small set of Sce modules exists; simple
    homebrew is the realistic target.
14. **Zig `-O0` links a 256 KiB TLS signal stack.** glibc refuses any explicit
    pthread stack smaller than the executable's static TLS block, so 64 KiB
    Vita thread stacks fail with `EINVAL` in debug builds. `threadStart()`
    grows the requested size until the host accepts it; do not assume
    `PTHREAD_STACK_MIN` is the only lower bound.
15. **Test binaries need the armhf sysroot.** The Ubuntu VM is x86_64; run
    guest binaries with `qemu-arm-static -L /usr/arm-linux-gnueabihf`.
16. **ARM32 unaligned 64-bit loads in the shader translator.** GXP instruction
    streams are only 4-byte aligned; Vita3K analysed them as `uint64_t*`, which
    traps on ARM. `usse_translator_entry.cpp` now copies the phases into
    aligned vectors using byte pointers (and offsets are relative to the offset
    field, not the struct base).
17. **EGL platform selection on Linux.** glvnd exposes `eglGetPlatformDisplay`
    (core EGL 1.5) but not necessarily `eglGetPlatformDisplayEXT`; calling
    `eglGetDisplay(EGL_DEFAULT_DISPLAY)` on a headless box selects the X11
    platform and fails with `xcb_connect`. Try
    `EGL_PLATFORM_SURFACELESS_MESA` through the core and EXT entry points first.
18. **Guest float ABI.** The host and the default VitaSDK toolchain are both
    hard-float. Any export taking or returning `float` must use the host PCS;
    the `GUEST_SOFTFP_ABI` in `module.h` is deliberately empty unless
    `VITA2NATIVE_SOFTFP` is defined for old softfp homebrew. Integer-only
    exports are unaffected. This covers GXM viewport, depth background and
    W-clamp, NGS patch volume, SceMotion angles, PGF conversions and the EABI
    float-to-integer helpers. Getting this wrong is silent: SuDokuL's
    `sceGxmSetViewport` received garbage (`off=0, scale=-6e18`) and the whole
    frame was black.

## 7. Graphics strategy

### Current deko3d usage (removed)

The upstream `SceGxm.c` used ~60 deko3d entry points: command buffers, memory
blocks, images, shader code, descriptor sets, fences, queues, and a swapchain.
That implementation and its deko3d/libnx dependencies were deleted when the
Horizon OS target was dropped; the GLES path is written against `gfx.h` instead.

### Chosen approach

- Introduce `include/gfx.h` and a `source/gfx/` backend.
- First backend: `gfx_null` (no-op), enough for CPU-rendered homebrew.
- Second backend: `gfx_gl` (GLES 3.1).
- Keep GXM state translation; replace deko3d calls.
- Guest memory is authoritative; GPU resources are shadows:
  - upload textures/vertex/index data on use;
  - render targets are FBO textures;
  - read back to guest memory at `sceGxmEndScene`/`sceGxmFinish` or when the
    guest samples the surface.
- Reuse Vita3K's format/swizzle helpers (GPLv2-compatible):
  - `renderer/src/gl/texture_formats.cpp`
  - `renderer/src/gl/color_formats.cpp`
  - `renderer/src/gl/attribute_formats.cpp`
  - `renderer/src/texture/format.cpp`
    (`swizzled_texture_to_linear_texture`, `tiled_texture_to_linear_texture`)
- Do not use Vita3K's GL renderer wholesale: it requires desktop GL 4.4+
  features (`glBufferStorage`, SSBOs, image load/store, `glCopyImageSubData`,
  `glGetTexImage`, `glPolygonMode`).

### Shader pipeline

1. Vita GXP binary.
2. `shader::convert_gxp(..., Target::SpirVOpenGL, ...)`.
3. SPIRV-Cross with `options.es = true; version = 310;`.
4. `glShaderSource` / `glCompileShader` / `glLinkProgram`.
5. Cache by GXP hash.

Vita3K's own GLSL target hardcodes desktop `#version 430` and
`options.es = false` in `vita3k/shader/src/spirv_recompiler.cpp`, so it cannot
be used directly for GLES.

## 8. Test environment

The development machine is macOS (arm64). Builds are cross-compiled with Zig
(`arm-linux-gnueabihf`, hard-float) and executed on an Ubuntu x86_64 VM at
`builder@192.168.1.9` (direct SSH; it is a test target only, never a build
host). The VM has a passwordless sudo account, `qemu-arm-static`, armhf Mesa
and Xvfb.

```sh
# on the Mac
cmake -S . -B build/cmake-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/build/zig-arm.cmake \
  -DVITA2NATIVE_HEADLESS=ON \
  -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake-arm -j 8
scp build/cmake-arm/host_smoke build/cmake-arm/vita2native builder@192.168.1.9:/tmp/

# on the VM (no compilation)
qemu-arm-static -L /usr/arm-linux-gnueabihf /tmp/host_smoke
```

The VM has `qemu-arm-static` 8.2.2 and the armhf runtime sysroot
(`/usr/arm-linux-gnueabihf`, glibc 2.39) from `libc6-armhf-cross`.

Guest ELF fixtures are generated, not checked in:

```sh
python3 tests/fixtures/build_guest_elf.py build/guest
mkdir -p /tmp/v2n-test && cd /tmp/v2n-test
qemu-arm-static -L /usr/arm-linux-gnueabihf /tmp/vita2native /tmp/guest_return.elf
qemu-arm-static -L /usr/arm-linux-gnueabihf /tmp/vita2native /tmp/guest_exit.elf
qemu-arm-static -L /usr/arm-linux-gnueabihf /tmp/vita2native /tmp/guest_io.elf
```

Expected: `guest_return.elf` logs `Thread 0x1 returned with: 0x2a` and exits 0;
`guest_exit.elf` logs `sceKernelExitProcess called!` and exits 0;
`guest_io.elf` returns 42 and creates `vita2native/ux0/fix`.

`guest_gxm_scene.elf` exercises initialize/context/render-target/begin/end and
`guest_gxm_draw.elf` performs a full imported draw (shader patcher, streams,
default uniforms, texture, draw). Both return 42; under the null backend the
pixel check is skipped, so this validates the guest ARM ABI and import patching
rather than the rendered image.

The GLES backend is exercised on the x86_64 VM (Mesa llvmpipe) with an x86_64
build of `gfx_smoke`:

```sh
zig cc -target x86_64-linux-gnu -Iinclude -Iexternal/khronos/include \
  tests/gfx_smoke.c source/host/host_gfx.c \
  source/host/host_gfx_gl.c source/host/host_gfx_null.c \
  -o /tmp/gfx_smoke -ldl -lm
/tmp/gfx_smoke   # "GLES 3.2 (... llvmpipe ...)" + red readback
```

The armhf binary reports the null backend there because the VM has no armhf
EGL/GLES libraries; that fallback path is verified too.

### Real GLES on ARM32 under qemu

The VM also carries armhf Mesa so the *ARM* build can render for real:

```sh
sudo dpkg --add-architecture armhf
# armhf lives on ports.ubuntu.com, not in the x86 archive
printf 'deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports noble main \
restricted universe multiverse\n' | sudo tee /etc/apt/sources.list.d/armhf-ports.list
sudo apt-get update
sudo apt-get install -y libegl1:armhf libgles2:armhf libegl-mesa0:armhf gdb-multiarch
```

Run with the armhf library path and `/proc/self/maps`-visible addresses:

```sh
GALLIUM_DRIVER=softpipe \
LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf:/usr/arm-linux-gnueabihf/lib \
VITA2NATIVE_DUMP_FRAME=/tmp/frame.bmp VITA2NATIVE_DUMP_FRAME_AT=5 \
qemu-arm-static -L /usr/arm-linux-gnueabihf ./vita2native --headless game.vpk
```

**Use `GALLIUM_DRIVER=softpipe`.** Under `qemu-arm-static`, llvmpipe crashes on
any thread other than the main one inside
`llvm::TargetLoweringBase::initActions()`: the ~230 KiB frame it allocates
overruns the top of the thread stack. The same shaders compile fine on the main
thread and on a plain pthread, so this looks like a qemu/LLVM interaction
rather than a vita2native bug — but it means llvmpipe cannot be used to test
guest rendering here. softpipe renders the same content, just slower.

The full draw path is tested with real GLES on the x86_64 VM, using an x86_64
Linux cross-build of the shader toolchain (compiled on the Mac with Zig):

```sh
cmake -S . -B build/cmake-x64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/build/zig-x64.cmake \
  -DVITA2NATIVE_HEADLESS=ON -DBUILD_TESTING=ON \
  -DVITA2NATIVE_ALLOW_64BIT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/cmake-x64 --target gxm_draw_smoke -j 8
scp build/cmake-x64/gxm_draw_smoke builder@192.168.1.9:/tmp/

# on the VM (Mesa llvmpipe)
/tmp/gxm_draw_smoke external/Vita3K/tools/native-tool/src/shaders
```

Expected: solid textured quad, region clip, 4-colour nearest sampling and
wireframe all pass (`gxm_draw_smoke: ok`). Texture data in this test is
allocated below 2 GiB because GXM packs texture addresses as 30-bit words.

EGL window presentation is verified with `egl_window_smoke`, which creates an
X11 window through `libX11` (dlopen, no link dependency), attaches it to the
production GLES device and reads the window back with `XGetImage`:

```sh
xvfb-run -a -s "-screen 0 640x480x24" ./egl_window_smoke
# egl_window_smoke: ok
```

The test checks the letterboxed checkerboard colours and that presenting a NULL
framebuffer clears the window to black.

`display_egl_smoke` (non-headless builds) validates the whole production path:
`SceDisplay_init` creates the SDL/X11 window, `host_gpu_attach_window` attaches
it to EGL, and `sceDisplaySetFrameBuf` presents red and blue guest framebuffers
which are read back from the window with `XGetImage`:

```sh
xvfb-run -a -s "-screen 0 1024x768x24" ./display_egl_smoke
# GPU: GLES 3.2 Mesa ...
#   centre pixel: 255,0,0
#   centre pixel: 0,0,255
# display_egl_smoke: ok
```

## Real-hardware validation (Orange Pi Zero 3, 2026-11)

Setup: Allwinner H618, 4x Cortex-A53, Mali-G31 MP2 via Panfrost, Ubuntu 24.04
aarch64 kernel with armhf multiarch enabled. The ARM32 build runs natively
(no qemu). `EGL_PLATFORM=surfaceless` is enough; no display session needed.

Results:

- `GLES 3.1 Mesa 25.2.8 (Mali-G31, Panfrost), EGL 1.5`.
- 30/30 ARM32 smoke tests pass, including `gxm_draw`, `shader`, `texture`,
  `gl_thread` and `gfx`.
- VitaShell renders its home screen with real device capacities.
- x86_64 suite still 31/31 on the builder VM.

Portability bugs found and fixed by running on the GPU:

1. **Vertex SSBOs do not exist on Mali.** The GXP translator emits the default
   and named uniform containers as a read-only SSBO. GLES 3.1 requires
   `MAX_VERTEX_SHADER_STORAGE_BLOCKS >= 0`; Panfrost reports 0, so program link
   failed with "Too many vertex shader storage blocks".
   `spirv_cross_glsl_es.cpp` now retags read-only storage buffers as uniform
   blocks (their std140 and std430 layouts are identical here), and SceGxm binds
   them as UBOs.
2. **Per-unit texture cache thrashes.** VitaShell alternates its 512×512 font
   atlas and small icons on one unit, so the atlas was decoded every draw
   (204 ms). Replaced with a 64-slot global LRU keyed by content hash, plus a
   once-per-frame hash check. The decoder now parses the format once per level
   (not per texel) and has an 8-bit UNORM fast path.
3. **Streaming buffers stall the driver.** Re-uploading into buffers the GPU is
   still reading cost more than the draw. Per-draw vertex/index data now comes
   from a per-frame arena that is orphaned once in `sceGxmBeginScene`.
4. **Panfrost readback is 2× slower with tiled/AFBC layouts.** `PAN_MESA_DEBUG=linear`
   is set by default (when the caller has not set the variable), because this
   port reads the whole frame back every frame. 102 ms → 50 ms for 960×544.

Performance measured on the board (RelWithDebInfo, `VITA2NATIVE_PROFILE=1`):

| Phase | ms/frame |
| --- | --- |
| Draw setup + submission (847 GXM draws → ~119 GL draws) | ~23 |
| GPU wait (`glFinish`) | ~96 |
| Framebuffer readback | ~50 |
| Guest-surface conversion | ~2.5 |
| **Total / frame rate** | **~175 ms / 4.8 fps** |

Draw batching: consecutive draws with equal program, textures, uniform
containers and render state are appended to one GL draw. Triangle strips and
fans are expanded to triangle lists while merging, since concatenating GL
strips would create bridging triangles (this corrupted text on the first
attempt). `VITA2NATIVE_NO_BATCH=1` disables it, `VITA2NATIVE_BATCH_LIMIT=N`
caps the batch length, and `gxm_draw_smoke` has a two-strip regression scene.

Remaining optimisation candidates, in order:

- The GPU wait is now fill/shader bound: ~522k pixels through translated GXP
  fragment shaders at 432 MHz. Reducing shader complexity or render resolution
  is the only lever left there.
- Plain uniforms instead of UBO subdata for the small per-draw container
  (`glUniform4fv` measured ~6 ms/847 draws vs ~56 ms for two UBO uploads).
- The readback is Panfrost's per-pixel path (195 ns/pixel, halved by the linear
  layout). A dma-buf/pbuffer presentation path would remove it entirely, but
  the guest expects pixels in its own memory.
- Optional: move the readback off the main thread behind a one-frame delay
  (PBO double buffering was slower on Panfrost, so this needs its own test).

Testing procedure (build on the Mac, run on the board):

```sh
rsync -az build/cmake-arm-rel/vita2native opiz3:/tmp/v2n-arm/vita2native-rel
ssh opiz3 'cd /tmp/v2n-run && LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf \
  EGL_PLATFORM=surfaceless /tmp/v2n-arm/vita2native-rel /tmp/VitaShell.vpk'
```

The GPU is test-only; nothing is compiled on the board.

## Real-hardware validation (R36S, 2026-11)

Setup: Rockchip RK3326, 4x Cortex-A35, Mali-G31 through the proprietary ARM
Bifrost blob (r13p0, GLES 3.2 over EGL 1.4), vendor 4.4.189 kernel, Debian 13
aarch64 with armhf multiarch, 640x480 DSI panel. The ARM32 build runs natively.
Mesa cannot drive this GPU here (the vendor kernel has no Panfrost/Lima node),
so the blob is the only GLES implementation; the blob needs `/tmp/mali-egl`
symlinks for `libEGL.so.1` / `libGLESv2.so.2` and no `EGL_PLATFORM`.

Results:

- 25/25 non-GL smoke tests pass; `gxm_draw_smoke` passes fully after the
  depth/stencil fallbacks below; `shader_smoke` passes.
- VitaShell renders on the physical panel at ~1.3 fps (119 GL draws, 5079
  vertices per frame).

### The fbdev trap

`/dev/fb0` exists and is owned by `rockchip-drm`, but it is **not** what the
VOP scans out. With the panel enabled, `/sys/kernel/debug/dri/0/summary` showed
`win1-0: ACTIVE` with `buf[0]: addr: 0x0 pitch: 2560`: the display controller
was scanning a null buffer. Writes to `/dev/fb0` are coherent (write, readback
and compare match) but land in a GEM buffer nobody displays, and `vtcon0` is a
dummy device, so nothing else was going to attach it.

Presentation goes through SDL, loaded with `dlopen` so the headless ARM build
keeps no SDL dependency. SDL's own video driver does the platform work: KMSDRM
on handhelds (it sets the CRTC on the first swap, then page-flips GBM buffers,
and ignores a failed `drmSetMaster`), X11/Wayland on desktops, and
offscreen/dummy for headless runs. `SDL_VIDEODRIVER` selects a driver
explicitly; there is no project-specific video selector.

The guest frame is uploaded as `SDL_PIXELFORMAT_ABGR8888`, identical to the
guest's A8B8G8R8 byte order, so no per-pixel conversion is needed.
`SDL_RenderSetLogicalSize` provides aspect-preserving letterboxing, while
`VITA2NATIVE_STRETCH` fills the display instead and
`VITA2NATIVE_SDL_WINDOWED` keeps a 960x544 window for desktop development.

History: the fork first drove DRM/KMS with raw ioctls (dumb buffers, SETCRTC,
page flips) plus an fbdev fallback. On the R36S vendor kernel the VOP kept
scanning the SETCRTC buffer (`dri/0/summary` showed the same `buf[0]` address
across thousands of accepted flips) while the panel froze; SDL works there
because its first swap uses `drmModeSetCrtc` before flipping. The custom
presenter was removed once SDL covered the same devices; it remains in git
history if a board ever needs it again.

`tests/present_probe.c` exercises the path without the emulator (solid colours
plus a moving marker); use `SDL_VIDEODRIVER=dummy` for a headless run.

SDL runs on its own thread (`source/host/host_video_sdl.c`). Its GL renderer
makes an EGL context current on the presenting thread, and on Mesa that leaks
errors into the guest's GLES context: EGL tracks the current context per
display, so neither `eglGetCurrentContext` nor a cached binding detects the
switch. Rebinding before every draw hid the errors but cost an `eglMakeCurrent`
per draw. With a dedicated video thread the guest thread never changes context;
it only copies finished frames into a staging buffer and signals the thread
(dropping a frame if the previous one is still being uploaded).

Two details matter for X11 forwarding. The headless build forces the GLES
context onto the offscreen platform even when `DISPLAY` is set; otherwise Mesa
selects the X11 EGL platform and falls back to llvmpipe over the forwarded
connection (~5x slower). llvmpipe JITs shaders on the calling thread, so host
threads also get a 1 MiB stack minimum regardless of the guest-requested stack
size (LLVM overflows a 64 KiB stack in `TargetLoweringBase::initActions`).
`VITA2NATIVE_SDL_SIZE=480x272` shrinks the window for remote testing.

### Input

Two sources feed `SceCtrl`, and their button states are ORed:

1. **SDL**, sampled on the video thread with `SDL_PumpEvents`. This is the
   desktop and X11-forwarding path. Keyboards are read with
   `SDL_GetKeyboardState`; game controllers go through
   `SDL_GameControllerOpen` (SDL's controller database handles mainstream
   pads) with a raw `SDL_Joystick` fallback when no mapping exists. Raw input is
   positional: buttons 0..3 = face, 4/5 = L1/R1, 6/7 = L2/R2, 8/9 =
   select/start, 10..13 = D-pad; axes 0..3 = left/right sticks. A game
   controller is rescanned once a second, and `SDL_GameControllerGetAttached`
   drops a disconnected pad.
2. **evdev**, the original headless backend, which opens every
   `/dev/input/event*` node and reads it from a poll thread, mapping
   `BTN_SOUTH/EAST/NORTH/WEST`, `BTN_TL/TR/TL2/TR2`, `BTN_DPAD_*`,
   `BTN_SELECT/START`, `BTN_TRIGGER_HAPPY1..5` (the R36S F1..F5 buttons) and
   `ABS_X/Y/RX/RY` (±1800 -> 0..255). This is the reliable path on the R36S.
   Keyboard arrows, Z/X/A/S, Q/E, Enter and Backspace are mapped too.

SDL axes are used only while a controller is attached, so a keyboard-only
session never overwrites evdev's stick values. Keyboard mapping: arrows,
Z/Enter = cross, X/Backspace = circle, A = square, S = triangle, Q/E = L1/R1,
1/2 = L2/R2, Tab/Space = select/start. `VITA2NATIVE_INPUT_DEBUG=1` logs both
paths and, for SDL pads, prints the mapping string SDL selected. Scripted input
still works and is ORed with both sources.

The front touch panel is sampled in the same `SDL_PumpEvents` pass: mouse
coordinates are mapped through the same letterbox rectangle SDL uses for the
frame into the Vita's 1920x1088 active area, and a press is latched until the
guest reads it. Clicks in the black bars are ignored; the back panel has no
source. This replaces the old direct `SDL_GetMouseState` call in
`SceTouch_linux.c`, which ran on the guest thread (SDL calls belong to the
thread that initialised the subsystem) and was compiled out of headless builds
entirely, so touch did nothing in the builds we actually ship.

The R36S shell labels its east button "A" while the driver reports it as
`BTN_EAST`. The positional mapping (south = cross) matches a real Vita;
`VITA2NATIVE_CTRL_SWAP_AB` swaps cross/circle on both sources for users who
expect A to confirm.

`sceCtrlSetActuator` queues a rumble request; the video thread applies it with
`SDL_GameControllerRumble` (small -> high-frequency, large -> low-frequency,
0..255 scaled to 0..65535) and leaves it running until the game sends a new
value. The video thread also polls `SDL_PollEvent` for `SDL_WINDOWEVENT_CLOSE`
and `SDL_QUIT`: closing the window terminates the process through `_exit(0)`,
since the guest has no portable way to be unwound from another thread. Window
size changes update the letterbox used by presentation and touch.

Validated end to end with `tests/uinput_pad.c` (evdev on the R36S and SDL on
x86_64), with `tests/present_probe.c` under Xvfb + `xdotool` (every keyboard
mapping reached `HostPadState`; a click at the window centre produced panel
coordinates 960,544 and a click in a letterbox bar produced none), and with the
ARM emulator under X11, where `VITA2NATIVE_INPUT_DEBUG=1` showed the guest's
`sceCtrlPeekBufferPositive` receiving the injected buttons.

Because the guest polls the pad only once per rendered frame (about 1.3 Hz on
the R36S), a human tap can be released before it is ever sampled. The backend
latches each new press until a `sceCtrlPeekBufferPositive` call has seen it, so
short taps register exactly once; a 150 ms tap test (12 taps) moved VitaShell's
selection 12 rows (`gro0:` to `xmc0:`).

### Blob quirks and fixes

- `glReadPixels` of depth and stencil returns `GL_INVALID_ENUM`. Depth is read
  back as 24/8, then `UNSIGNED_INT`, then `FLOAT`; if all fail it is exported by
  a shader into an RGBA8 target and packed on the CPU. Stencil has no readback
  at all, so it uses `GL_ARM_shader_framebuffer_fetch_depth_stencil`
  (`gl_LastFragStencilARM`) and falls back to a `GL_STENCIL_INDEX` read where
  that constant exists.
- Every `glDrawElements` costs ~5 ms regardless of size (119 draws ≈ 590 ms),
  while `glUniform4fv` is cheap; the per-draw driver overhead dominates, not
  the GPU. Reducing distinct state changes and draw count is the next lever.
- `glBufferSubData` is slow, so per-batch vertex/index arenas keep a CPU mirror
  and commit once per batch (`gxm_arena_commit`).

Frame budget with `VITA2NATIVE_PROFILE=1`: draw ~640-870 ms (of which gldraw
~590-780 ms), `glFinish` ~55 ms, readback ~13 ms, conversion ~3 ms.

CPU use is ~200% while VitaShell runs, but almost none of it is in this project:
`ps -L` shows the blob's two `mali-utility-wo` worker threads at ~90% each in
state `R` (busy-spinning while GL work is in flight), against ~10% each for the
guest and render threads. The lever is the same as for frame rate: fewer GL
draws and state changes, because each `glDrawElements` blocks for ~5 ms and
keeps the workers spinning for that long.

A full frame capture on the Orange Pi (55 consecutive guest frames) showed no
black frames, which isolated the panel flash to the presenter rather than the
guest.

Testing procedure (build on the Mac, copy to the board; nothing is compiled on
the device):

```sh
scp build/cmake-arm-rel/vita2native r36s:/tmp/v2n-arm-rel/vita2native
ssh r36s 'cd /tmp/v2n-run && \
  LD_LIBRARY_PATH=/tmp/mali-egl:/usr/lib/arm-linux-gnueabihf \
  /tmp/v2n-arm-rel/vita2native /tmp/VitaShell.vpk'
```

## 9. References

Local:

- `source/load.c` — loader, relocations, import patching.
- `source/main.c` — deko3d init, module bring-up, process lifetime.
- `source/modules/SceSysmem.c` — guest memory + GPU mapping.
- `source/modules/SceDisplay.c` — swapchain/presenter.
- `source/modules/SceGxm.c` — GXM backend.
- `source/modules/SceKernelThreadMgr.c`, `SceLibKernel.c` — threads/files.
- `include/gxm/gxm_to_dk.h`, `include/display/display_to_dk.h` — mappings.
- `external/CMakeLists.txt` — Vita3K shader/gxm/glslang integration.
- `external/Vita3K/vita3k/renderer/src/gl/` — GL reference.
- `external/Vita3K/vita3k/shader/src/spirv_recompiler.cpp` — shader targets.

External:

- VitaSDK build flags:
  <https://github.com/vitasdk/buildscripts/blob/master/cmake/recipes/GccCommonArgs.cmake>
- VitaSDK softfp ABI shims:
  <https://github.com/vitasdk/buildscripts/blob/master/softfp-shim/README.md>
- Mesa V3D (GLES 3.1):
  <https://docs.mesa3d.org/drivers/v3d.html>
- Mesa VC4 (GLES 2.0):
  <https://docs.mesa3d.org/drivers/vc4.html>
- Vita3K GL renderer:
  <https://github.com/Vita3K/Vita3K/tree/master/vita3k/renderer/src/gl>
- Vita3K shader recompiler:
  <https://github.com/Vita3K/Vita3K/blob/master/vita3k/shader/src/spirv_recompiler.cpp>
- SDL2:
  <https://wiki.libsdl.org/SDL2/>
- Linux input event codes:
  <https://www.kernel.org/doc/html/latest/input/event-codes.html>

## 10. Open work

### 10.1 Audio: NGS fidelity gaps

**Priority:** high. **Status:** the graph renders; the gaps are fidelity.

`source/modules/SceNgs_render.c` renders what the titles in hand use: ATRAC9
voices, the HEVAG/PCM player (Simple voices), patch volume matrices, buffer
switch/end-of-data callbacks and the master buss. The guest pulls mixed S16
frames with `sceNgsVoiceGetStateData` and feeds `sceAudioOutOutput`;
`VITA2NATIVE_AUDIO_DEBUG=1` reports peaks, and Angry Birds Trilogy plays menu
music and effects on the R36S.

Still missing:

- The EQ/envelope/pauser/filter modules after the source module are pass-through.
  Presets and their parameter blocks are ignored; ABT never calls
  `sceNgsVoiceSetPreset`, but other titles may.
- Only the master buss is mixed; chained non-master busses and the extra
  Simple/ATRAC9 outputs are ignored.
- HEVAG/PCM is resampled to the device rate with linear interpolation; ATRAC9 is
  assumed to be 48 kHz.
- `SceAvPlayer` audio returns zeroed buffers, so movie audio is silent by design.

Notes: `SceAudio` queues four device buffers per port; NGS tracing
(`VITA2NATIVE_NGS_DEBUG=1`) logs the graph, parameter dumps, master peaks and
callbacks.

### 10.2 Rendering: load-time work

**Priority:** medium.

Closed (kept here because the numbers are the baseline):

- **Parallel texture decode** (`include/gxm/decode_bands.h`): destination-row
  bands aligned to the decoder's unit, across `VITA2NATIVE_DECODE_THREADS`
  workers. The default follows the CPUs the process may run on (affinity mask):
  all of them on one or two cores, one fewer on three or more, capped at 4 where
  the decoders turn memory-bound; `1` is the A/B control. Banded: swizzled U4/U8
  tiles, swizzled U4 RGBA4 rows, PVRTC-I 4bpp. On the R36S, 1 -> 3 threads:
  u4 swizzled 95.6 -> 35.8 ms, u4 rgba4 ABGR 128.9 -> 57.9 ms, u8 swizzled
  246.3 -> 86.9 ms, pvrtc4 1024 87.1 -> 33.3 ms.
- **Palettised decode** (`decode_paletted_fast`): Iconoclasts' 640x360/800x360
  frames are P8/P4 (`0x95000000`/`0x94000000`), and went through a per-pixel
  format rebuild and palette unpack. A 256-entry LUT built once per level fixed
  it: 800x360 P4 85.6 -> 4.0 ms, 256x256 P4 swizzled 25.0 -> ~6 ms. The worst
  decode in that title's intro is now ~6 ms.
- **Four-byte texels** are a shift-based permutation rather than a per-byte loop
  for the linear ARGB/1RGB swizzles (41 -> 11.6 ns/pixel by bench); Morton
  layouts the tile walk rejects (non-power-of-two dimensions) get the fast
  per-texel body.

Open:

- **Async decode-ahead.** Start the decode when `sceGxmSetFragmentTexture` is
  called and fence before the draw, or upload on a shared-context worker thread.
  Overlaps decode/upload with guest work, but needs request tracking, guest
  memory safety and cache-eviction ordering. Attempt only after the band split
  has been measured against a real level load.
- **2bpp PVRTC decode.** The optimised decoder covers PVRTC-I 4bpp only; 2bpp and
  PVRTC-II use the PowerVR reference (~187 ns/pixel on the Orange Pi). Extend
  only if a title is measured using those formats.
- **Texture re-decode on cache eviction.** The cache is byte-bounded and
  re-decodes evicted atlases later (seen at frames 4153/4155/4289/4308, ~160 ms
  each). Options: raise the budget on devices with RAM to spare, drop mip levels
  from the cache footprint, or persist decoded textures on disk keyed by content
  hash. Measure first whether the current level-load path still evicts.
- **Orange Pi stability.** Angry Birds level loads hard-hang the Pi (SSH banner
  times out, watchdog cannot run) with `VITA2NATIVE_TEXTURE_CACHE_MB=128` and no
  `VITA2NATIVE_FINISH_PER_FRAME`; the R36S survives the same path. If the Pi is
  used for in-game testing, run with `VITA2NATIVE_FINISH_PER_FRAME=1`, a cgroup
  memory cap, and logs on disk rather than tmpfs.

### 10.3 Input: R36S Fn key as a Home/debug key

**Priority:** low.

`BTN_TRIGGER_HAPPY5` (the Fn key) is ignored; it is a firmware hotkey modifier
with no Vita equivalent, so forwarding it as Select/Start was wrong. Idea:
expose it as a host-side Home key — short press toggles a debug pane (FPS graph,
frame profiler, input viewer, texture-cache stats), long press or Fn+Start exits
the game, and it never reaches the guest's `SceCtrlData`.

Open questions: does ArkOS consume Fn combinations before evdev sees them; should
the pane be host-rendered like `fps_overlay.c` or drawn by a guest overlay; which
gesture avoids the existing Fn hotkeys (brightness, volume).

## 11. Known limitations

Behaviour that is deliberately incomplete, so a later reader does not treat it as
a bug:

- **Tiled and swizzled GXM transfers** return `SCE_GXM_ERROR_UNSUPPORTED`;
  `sceGxmTransferCopy` implements linear-to-linear with the PASS and REJECT
  colour-key modes only.
- **Negative transfer strides** are handled but no test exercises a bottom-up
  surface.
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

## 2026-11-07 — IL2CPP title (CrossyRoadsClone) bring-up

### Root cause found: SCE relocation format
The loader treated every relocation entry as "short (8) / long (12)" and ignored
format-0's `code2`/`dist2` pair. Sony's and Unity's linkers express a MOVW/MOVT
pair as **one** format-0 entry (`code` + `code2` at `offset + dist2*2`), so the
high half of every absolute address stayed at the link-time image base
(0x81xxxxxx) and modules crashed on their first static access.

`source/load.c:relocate()` now implements the full stateful format (0-9):
- format 0: absolute offset, two patch sites (`code`, `code2` + `dist2*2`)
- format 1: 32-bit offset split across fields
- formats 2-5: deltas inheriting the previous entry's type/addend
- formats 6-9: re-derive ABS32 addends from the current value (used by large
  data tables; needs `p_vaddr`, now kept in `segment_info_t`)

### Module loading
- `SceModulemgr_load_app_modules()` scans `sce_module/` and `Media/Modules/`,
  loads every `.suprx` (dedup by module name), re-patches all imports, then
  starts them in dependency-ish order (pthread first).
- Imports are re-patched after every module is registered (`load_repatch_imports`)
  so load order inside a directory does not matter.
- `module_start` now runs on its own pthread: Sony's `libc.suprx` ends its start
  routine with `sceKernelExitThread`, which used to kill the main thread.
- Thumb detection is relocation-based (`module_is_thumb`): VitaSDK sets bit 0 on
  entry points, Sony's and Unity's linkers do not. Override: `VITA2NATIVE_GUEST_ARM=1`.
- `VITA2NATIVE_SKIP_MODULES=libc.suprx` (etc.) skips individual modules.
  `VITA2NATIVE_SKIP_MODULE_START=<names>` loads but does not start them.

### Host libraries
- New `source/modules/SceLibc.c`: SceLibc (95 exports), SceLibm (40) and
  SceLibstdcxx (5 + `std::nothrow`) mapped onto the host C library, including
  Vita path translation for `fopen`/`remove`, a `setjmp`/`longjmp` pair written
  as ARM assembly, `mspace_*` backed by the host heap, and a per-module TLS block
  allocator behind `__tls_get_addr`.
- `sceKernelGetProcessParam` now reports version 5 and a valid `sce_libc_param`
  (size 0x0c) — libc.suprx refuses to initialise otherwise. `SceLibcParam`
  `__sce_libcparam` is exported as a host variable.
- Host threads that were not created by the guest (module starts, SDL callbacks)
  get a fallback `VitaThreadInfo` so `sceKernelGetTLSAddr` and friends work.
- Added `sceAppMgrSetInfobarState` and `SceShutterSound::sceShutterSoundPlay`.

### Current status of CrossyRoadsClone
All 7 shipped modules load and start; the eboot reaches `<main>`, Unity creates
`Unity_main_thread`, then crashes at eboot offset 0x2156ec:
`blx` to a thunk at 0xb827dc that returns its argument, then `str r7,[r0]` with
r0=8. Next investigation: what that thunk is (looks like an il2cpp delegate /
`std::function` slot) and why it returns 8 instead of a pointer.

### Notes
- `libc.suprx` is autoloaded by default now; the host SceLibc exports take
  priority for the functions we implement, the guest module fills the rest
  (SceLibstdcxx).
- Import tracing is off by default; use `VITA2NATIVE_TRACE_IMPORTS=1`.

## 2026-11-07 (later) — remainder of the IL2CPP bring-up

### Fixed since the last entry
- `module_start` runs on its own pthread; Sony's `libc.suprx` ends its start
  routine with `sceKernelExitThread`.
- The main executable is now loaded **before** its user modules (firmware order),
  its imports are re-patched once the modules register their exports, and
  `SceProcessParam` is taken from the executable's own `NID_PROCESS_PARAM`
  export instead of a synthetic block.
- `sceKernelAllocMemBlock` aligns a block to the largest power of two dividing
  its size and maps it inside the Vita user range (0x8A000000-0xB0000000)
  instead of wherever the host allocator puts it.
- `SceLibc` allocator wrappers gained `VITA2NATIVE_TRACE_ALLOC` tracing.
- Debug helpers: `VITA2NATIVE_PATCH_BKPT=<offset>` plants `udf #0`, and
  `VITA2NATIVE_PATCH=<off>=<hex16>,...` patches instructions in the loaded
  image (the JIT RX mapping is not writable, so gdb breakpoints cannot be used).

### Remaining blocker (CrossyRoadsClone)
Unity's main thread crashes at eboot offset 0x2156ec:
`str r7,[r0]` with `r0 = 8`.

Chain: a 21-bucket, string-keyed hash table (FNV-1a) inserts {value, code
pointer} pairs. Readable bucket layout is `{data, tag, count, capacity}` at
`base+4+i*16`. Bucket 4 has `count=1`, `capacity=2`, `data=NULL`, so the second
insert computes `data + count*8 = 8` and stores there.

The first insert into that bucket grew the array through Unity's tagged
allocator (`Alloc(old, size, align, label, ...)` at eboot 0x18311a -> 0x182d2a)
and got NULL; the caller tolerates a NULL result (`cmp r0,#0; beq`) and only
records `count`/`capacity`, which is what makes the second insert fault.

The failure is **not** one of the two NULL-return paths we can instrument
(0x182f0c, 0x182f8c) and not `malloc` from SceLibc. Next things to try:
- trace the allocator's return value for every call (counter, not a trap), or
- dump Unity's heap block bookkeeping at the failure, or
- check `MemLabel` registration: the failing request carries label `0x44`
  while the startup requests carry `0` / a pointer, so the label table may be
  uninitialised.

### X11
The ARM build presents through SDL over forwarded X. The window opens
(960x544, or 480x272 with `VITA2NATIVE_SDL_SIZE=480x272`), shows nothing
because of the crash above, and closes. Captured frame:
`build/evidence/hw/crossy-x11.png`.

### 2026-11-07 (evening) — allocator failure narrowed down
- `VITA2NATIVE_CRASH_DUMP=<offsets>` dumps guest memory (relative to the main
  image) at crash time; used to read Unity globals.
- The Unity type-name table starts at image+0x146a714: `{0x44, 0x46, "AABB",
  "Animation", "Culling", "AnimationCurve", ...}`. 0x44/0x46 are counts, and the
  failing allocation carries 0x44 as its fourth argument.
- Two different allocator entry points are used:
  - image+0x18202e: `Allocate(size, align, label, 0, label_ptr, line)`; the
    0x154-byte hash table allocation goes through it and **succeeds**.
  - image+0x18311a -> image+0x182d2a: `Allocate(old, size, align, label, ...)`
    used for the bucket arrays; returns NULL for an 8-byte request.
- The NULL does not come from the two instrumented failure paths
  (image+0x182f0c / +0x182f8c), and `sceKernelTryLockLwMutex` is never
  rejected (VITA2NATIVE_TRACE_SYNC recorded zero failures).
- Forcing the bucket-array label to 1 moves the crash to a second, int-keyed
  hash table (image+0x1929e0, `str r7,[r0]` with r0=4) — same NULL-array symptom.

Next: instrument the allocator's return value per call (needs a call-counting
trampoline, not a trap), or dump the memory-manager bookkeeping right before the
first failing allocation.

### 2026-11-07 (late) — call tracer with return values; realloc failure isolated

Built `VITA2NATIVE_TRACE_TARGET=<vita addr>[,<addr>...]`, a call-site tracer:

- Every `movw ip,#lo / movt ip,#hi` pair in the image whose 32-bit value equals
  the requested target is repointed at a shared stub placed after the last
  executable segment (`TRACE_STUB_SIZE` = 80 bytes: entry stub + return
  trampoline). The pairs hold **relocated host addresses** at runtime, not the
  link-time values from the file, so the scan compares against
  `segment->vaddr + offset`, and `LoadedSegment` gained `vita_vaddr` to translate.
- No `bl`/`b` from the image works: the eboot's text is ~21 MiB and a Thumb
  `bl` reaches only ±16 MiB, and the call sites are indirect (`blx ip`) anyway.
  `movw/movt` carry the full 32-bit address, so retargeting them has no range
  limit.
- The stub saves `{r0-r3,r12,lr}`, records `frame[0..5]` plus `frame[6..7]`
  (stack args 5 and 6), calls the original target with **lr pointing at a
  return trampoline**, and the trampoline records `r0` and resumes the guest.
  Recording is per-thread (`__thread`, 16-deep) and lands in a 32-entry ring
  that `trace_dump_ring()` prints from the signal handler, so a normal run
  stays silent.
- Stub code is generated as raw Thumb words. Two bugs worth remembering:
  16-bit `push` cannot save `r12` and 16-bit `pop` cannot restore `lr` (bit 8
  is "PC" there), and the trampoline literal must be the **RX** alias, not the
  RW one. The host build is ARM, not Thumb (`-mthumb` never reaches the
  compiler, see below), so the stub must use the real `&function` bit0.

**Result.** With `VITA2NATIVE_TRACE_TARGET=0x8118311b,0x8118202f` the crash is
preceded by:

```
trace image+0x18311b: r0=0 r1=8 r2=4 r3=0x44 ... lr=...757 -> e62448b0
... 7 x (size 8, label 0x44) -> e62448b0, d0, f0, 10, 30, 50, 70
trace image+0x18311b: r0=e6244970 r1=0x10 r2=4 r3=0x44 ... -> 00000000
```

So the failing request is a **realloc of an 8-byte block to 16 bytes**
(`label 0x44`, align 4) from the hash-table grow path at image+0x215754; the
seven preceding fresh 8-byte allocations all succeed. The caller stores the
NULL over `bucket->data` after it already bumped `capacity`, which is exactly
the `{count=1, capacity=2, data=NULL}` state seen in the fault.

- `mspace_*` tracing showed Unity calls `mspace_create(base=0x8a0004a8,
  size=0xffb58)` for each of its three 1 MiB memblocks, but our
  `mspace_malloc` is a host `malloc` shim and the returned handle is opaque to
  Unity (the state stays zero), so that path is **not** the one that fails.
- The realloc that fails runs in the eboot's own dlmalloc-like heap
  (image+0x182d6e -> vtable[6] `ContainsPointer` at image+0xb81d78 ->
  vtable[3] `Reallocate` at image+0xb81b14). The chunk header in front of the
  old block is consistent (`prev_size=0x0c, size=0x11`), and the block sits at
  the very top of its ~1 MiB arena, so the arena looks exhausted.
- `[mgr+0xc1e] == 1` (label path, not the default allocator), `img+0x182e66`
  and the two other explicit NULL paths are never reached, and the arena's
  provenance is still unknown: it is an anonymous RW mapping, **not** a
  memblock (those are at 0x8a000000) and **not** a thread stack (guest stacks
  are 1 MiB and separately accounted).
- `VITA2NATIVE_TRACE_PROCESS_PARAM=1` dumps the eboot's own `SceProcessParam`
  (size 0x34, magic "PSP2", version 6); it carries no memory budget, so Unity
  sizes its heaps from elsewhere.

**Next:** find what backs that ~1 MiB arena (who passes its base to Unity's
heap constructor) and whether the arena is legitimately exhausted or its
bookkeeping is corrupt. Candidate: `sceKernelGetFreeMemorySize` /
`sceKernelGetMemoryInfo` budget, or the `SceLibcParam` the eboot exports at
process-param word 11.

### Build note
`build/zig-arm.cmake` force-sets the cache variable
`VITA2NATIVE_ARM_ARCH_FLAGS` to `""`, so `-march=armv7-a -mfpu=neon
-mfloat-abi=hard -mthumb` in `CMakeLists.txt` is never applied: the host binary
is built in **ARM mode** with Zig's defaults. Everything works, but the flags
should be plumbed through properly (and this is why the trace stub has to take
the real function-pointer bit).

### 2026-11-07 (night) — the failing heap is a host arena created after launch

More instrumentation and what it showed:

- `VITA2NATIVE_TRACE_TARGET` also works on the allocator's *internal* helpers
  because they are reached through `movw/movt` too. Hooking
  `0x8118311b,0x81183295,0x81b819bd` gave the exact failing sequence:
  `0x81b819bd(heap, old, 0x1630, 0x720, 0xf, heap+0x48) -> 1`,
  `0x183295(e6744960, 0x23, 0x1b) -> 0`, then the realloc returns 0.
  `0x183294` is a thunk that calls `0x818b08d8(g_alloc_selector, a, b, 0x10)`,
  and `0x818b08d8` is the eboot's statically linked newlib dispatcher
  (`cmp r0,#0x3e / #0xf / #3`).
- The dispatcher's heap-init path (`image+0x8b08f8`, mirrored at `+0x8b0a4a`)
  builds a `SceLibcHeapSetup`-style descriptor at `0x826ae670` with a
  **hardcoded `0x100000` block size** and memblock type `0xc20d060`, i.e. the
  same "HeapMalloc" block Unity asks for.
- `VITA2NATIVE_FREE_MEM=<hex>` now overrides `sceKernelGetFreeMemorySize`
  (the eboot imports it). Raising it from 96 MiB to 448 MiB changes nothing:
  the three 1 MiB memblocks are requested first and the same realloc fails, so
  the budget is not the sizing input.
- `VITA2NATIVE_TRACE_PROCESS_PARAM` dumps the eboot's own `SceProcessParam`
  (`size=0x34`, magic "PSP2", version 6) and word 11, which points at a 0x38
  byte block whose entries 2 and 3 are relocated function pointers. That block
  is almost certainly the executable's `SceLibcParam` with allocator hooks, not
  a string table as first assumed.
- `VITA2NATIVE_DUMP_MAPS` snapshots `/proc/self/maps` and the first words of
  every anonymous RW mapping ≥ 1 MiB, both before `launch()` and in the fault
  handler. Result: the failing heap lives in an anonymous `rw-p` mapping of
  **0x247000 bytes** that does **not** exist before launch and appears while
  the guest runs. It is not a memblock (those stay at `0x8a000000`), not a
  guest thread stack (both are 1 MiB and separately reported), and not the
  eboot data segment (`0xe27bf040` in the same run).
- Skipping `libc.suprx` (`VITA2NATIVE_SKIP_MODULES=libc.suprx`) crashes much
  earlier with no allocator calls at all, so Sony's module is load-bearing.

Open question: which call creates that 0x247000 mapping. Everything we export
that can hand out host memory (`malloc`, `calloc`, `memalign`, `mspace_*`,
`sceKernelAllocMemBlock`) has been logged and none of them allocates it, so it
is either a guest-side `sbrk`/`mmap` path that bypasses our exports or a host
library allocation that Unity then adopts. Next steps to try:
- log `mmap`/`mprotect` callers with `strace -f -e trace=mmap` on the device
  and correlate the 0x248000-byte request with our log timeline;
- or dump the mapping's first words and walk the dlmalloc chunk chain back to
  its arena start, then match that base against every pointer we ever returned
  to the guest.

### 2026-11-08 — root cause of the NULL realloc: two allocators are mixed

The failing allocations come from **`mspace_memalign`**, which is one of ours:

```
mspace_memalign(mspace, 16, 27) -> 0xe61448a0   (x7, 0x20 apart)
```

`27` is Unity's 8-byte request plus dlmalloc's 19-byte overhead, and those
returns line up exactly with the traced `Reallocate` results. So Unity allocates
through the imported `mspace_memalign`, which our `SceLibc` backs with the
**host heap** (`posix_memalign`), ignoring both the mspace handle and the region
the guest reserved.

The realloc, however, runs in the eboot's **own dlmalloc** (`image+0xb81b14`,
reached through `vtable[3]`) and interprets that host pointer as one of its own
chunks. `mspace_realloc`/`mspace_reallocalign` are **never called** by this
title (0 hits), so the two implementations never agree and the realloc fails.

Supporting facts:
- The eboot's import header for `SceLibc` really does carry our library NID
  (`0xBE43BB07`), so the import resolution is correct; the problem is that our
  mspace shim is not a real mspace.
- `VITA2NATIVE_NO_HOST_MSPACE=1` (new, opt-in) routes the whole family to
  `libc.suprx` instead. That gets much further — the guest now creates a fourth
  memblock (`Heap`, `0x8a600000`) and the allocator operates on guest memory —
  but crashes inside `libc.suprx` at +0x11396 (`fault=0x1bc`, `r0=0`), so
  Sony's module is not usable as-is either in this configuration.

**Conclusion:** `SceLibc`'s `mspace_*` must become a real newlib `mspace`
(`malloc_state` over the caller's `base`/`size`, same chunk layout as
dlmalloc) so that the imported calls and the title's own dlmalloc agree. That
is the next implementation task; the win is large because every IL2CPP title
uses this allocator.

### 2026-11-07 — CrossyRoadsClone (Unity/IL2CPP): startup unblocked, blocked on game data

The mspace rework landed and the title now gets through IL2CPP and Unity module
startup. The crash chain, in the order it was peeled off:

1. **Test bug, not allocator bug.** `mspace_smoke` assumed an 8-byte dlmalloc
   overhead and that PINUSE is always set. On 32-bit the head word is 4 bytes
   and PINUSE only describes the previous physical chunk. Fixed; the suite
   passes on real ARM32 and x86-64.
2. **`mspace_reallocalign` argument order.** The guest passes
   `(mspace, ptr, size, alignment)`; we had `(alignment, size)`. Unity asked
   for 35/51 bytes with 16-byte alignment and instead got 16-byte blocks with
   64-byte alignment, so it wrote past the end and smashed the next chunk
   header. This was the "corrupt bin" abort in dlmalloc.
3. **Softfp ABI.** GUEST_SOFTFP_ABI was gated behind an off-by-default CMake
   option, but Vita binaries always use base AAPCS. All by-value float/double
   SceLibm exports were compiled hard-float, so `_FSinx` read the float bits as
   the `sign` pointer. The macro is now unconditional on ARM and every such
   export uses EXPORT_SOFTFP.
4. **Thread callbacks on blocking waits.** The Vita runs queued thread
   callbacks when a thread enters a blocking syscall. Unity's pthread.suprx
   registers the user-mutex unlock as a callback before waiting on a condvar;
   without dispatch the two threads deadlocked (one held the mutex, the other
   waited for it). `SceKernelThreadMgr_run_pending_callbacks()` is now called
   from sema waits, event-flag waits and thread joins.
5. **SceKernelForMono.** Unity's pthread.suprx imports
   `sceKernel{Suspend,Resume,GetThreadContext,SetThreadContext}ForMono`.
   They now delegate to the ForVM implementations.
6. **Lwmutex registry had 64 fixed slots.** Unity creates more; creation #65
   failed and the work area was used uninitialised. The registry is now a list
   and `from_work` validates the pointer against it.
7. **Case-insensitive paths.** Vita filesystems are case-insensitive; the
   eboot opens `Il2CppAssemblies.suprx` while the VPK ships
   `il2CppAssemblies.suprx`. `host_fs_resolve_path` now fixes the case of each
   component (keeping the last one untouched so new files can be created).
8. **newlib globals are variables.** `_Stdout`, `_Stderr`, `_Stdin`, `_Ctype`,
   `_Touptab`, `_Tolotab` are imported as variables, not functions. They are
   published as cells in `module.c` and filled by a SceLibc constructor.
9. **Guest newlib FILE streams.** libc.suprx reassigns `_Stderr` to its own
   FILE at startup, and Unity's pthread error path then handed that pointer to
   glibc, which aborted with "invalid stdio handle". Our stdio wrappers now
   track host FILEs and route untracked streams to the host stderr instead.
10. **`sceKernelCancelSema(uid, -1, ...)`** means "keep the count". We rejected
    -1, and Unity's pthread treated ILLEGAL_COUNT as fatal.

Where it stops now: Unity probes `app0:/archive.psarc` and
`app0:/archive_patch.psarc`, finds neither, then insists on opening
`/archive/mount/point/Media/data.unity3d`. That file is **not in the VPK** (the
VPK ships loose Unity data files: `level0`, `sharedassets0.*`, …). With the
file absent Unity null-derefs; with an empty file it calls `exit(0)` during
startup. This looks like an incomplete game dump rather than an emulator bug.
Verify against another Unity title or the same title installed on hardware
before spending more time here.

Runtime knobs added this session (all env-gated, safe to keep):
`VITA2NATIVE_TRACE_SYNC` (semaphores, lwmutexes, sema cancellation),
`VITA2NATIVE_TRACE_FILEIO` (stdio), `VITA2NATIVE_TRACE_FIOS` (Fios calls with
guest return addresses), `VITA2NATIVE_MSPACE_DEBUG` (chunk headers and frees).

### 2026-11-07 (later) — CELP, Fios module preference and SceFiber

- **CELP audio decoder** (silent): the last codec without an in-tree decoder
  now accepts the full library/decoder lifecycle and emits one frame of
  silence per decode (8 kHz mono, 320 samples). `audiodec_smoke` covers it.
- **Guest libfios2 takes precedence.** `libfios2.suprx` exports 168 Fios
  functions under the same library NID as our host shim (0x3ad4d630), but the
  host only implements 38 sync entry points. Library resolution now prefers
  the module when the title ships it and falls back to the host. SceLibc stays
  host-only (its module_start aborts without a real Vita kernel). With this,
  CrossyRoadsClone loads `globalgamemanagers`, `globalgamemanagers.assets`,
  `level0`, `sharedassets0.*` and the built-in resources through the module.
  `VITA2NATIVE_PREFER_GUEST_EXPORTS=1` forces the same policy for every
  library except SceLibc.
- **SceFiber implemented** on host `ucontext`: initialize (with or without a
  caller-supplied context stack), run, switch, return-to-thread, get-self,
  get-info, rename, finalize, attach-context variants, markers. This removes
  the seven unresolved fiber imports IL2CPP had. `fiber_smoke` passes on
  ARM32 and x86-64.

Status of the CrossyRoadsClone blocker is unchanged: the VPK has no
`Media/data.unity3d`. With a zero-length placeholder Unity parses past the
first checks, loads all loose files and then underflows a length while parsing
that empty archive. The emulator side is no longer the limiting factor.

### 2026-11-07 (late) — X11 run, black-screen regression and the float ABI
SuDokuL over forwarded X11 presented a black window while VitaShell and
PixelChess rendered. Bisect with clean builds in a separate worktree:
`ec79cfa` renders, `281e72f` is black, HEAD is black. Reverting the commit's
sync/threadmgr, libc/mspace or Fios halves individually did not restore colour,
which pointed at something layout/timing-shaped rather than a code path.

Instrumenting `sceGxmEndScene` settled it: the `glReadPixels` result was all
zeros, and the batch state showed a corrupted viewport:

    ec79cfa: vp=480.0,272.0,480.0,-272.0   clip=0,0..959,543
    HEAD:    vp=0.0,-0.0,-0.0,1060.0       clip=0,0..959,543

`gxm_set_viewport` (NID 0x3EB3380B) takes six floats. `281e72f` made
`GUEST_SOFTFP_ABI` unconditional on ARM, so this export is now compiled with
`pcs("aapcs")` while VitaSDK-built titles (SuDokuL, PixelChess, VitaShell) call
it with the hard-float ABI. The title passes 480/272 in VFP registers; the host
reads the core registers instead and gets garbage, collapsing every triangle.

The softfp change was needed for SceLibm math (Sony modules / IL2CPP call
those softfp), not for the GXM float entry points. Fix: hard-float stays the
default for ordinary exports; only `EXPORT_SOFTFP` carries `pcs("aapcs")`.
Removed the attribute from `SceGxm` (viewport, depth background, w-clamp),
`SceMotion`, `ScePgf`, `SceNgs` and `SceLibKernelAeabi`.

Validated on opiz3 over SSH X11 forwarding, 960x544 software renderer:
SuDokuL menu, PixelChess title screen and VitaShell's file browser all visible
(`build/evidence/hw/*-x11-window.png`, captured with `screencapture -l`).
Smoke tests pass: aeabi, mspace, sync, threadmgr, fios2, audiodec, fiber, pgf,
ngs, texture. x86-64 build is clean.

Debt: the float ABI is per title, not global. Unity's IL2CPP eboot carries
`e_flags` 0x05001000 while VitaSDK titles carry 0x05000000, and it imports both
`sceGxmSetViewport` and libm floats. If a softfp title ever renders, its calls
into the GXM float entry points will need the softfp variant; the way to do it
is a per-title ABI flag plus dual entry points selected during import
resolution (`module_get_func_export` already knows the importer).

Two debugging aids were added and kept: `VITA2NATIVE_GXM_FRAME_TRACE` logs the
readback checksum for every 60th frame, and `VITA2NATIVE_GXM_STATE_TRACE` dumps
viewport/clip/depth/texture/uniform state for the first draws.

### 2026-11-07 (night) — birdunity (Unity 2017) and the mspace handle
`birdunity.vpk` is a complete Unity 2017.4.2 IL2CPP title (eboot 34 MB,
`Media/Modules` with pthread + il2cpp, loose globalgamemanagers/level0/
sharedassets0). It loads all seven user modules, starts IL2CPP, initialises
FMOD and GXM, and reads every asset file with correct sizes and preads, then
crashes before presenting a frame.

Two real bugs were found and fixed on the way:

1. **SceKernelForMono library NID.** Unity's `pthread.suprx` imports the four
   thread-context wrappers from library NID `0x5feea076`, not from SceLibKernel.
   They were registered only under SceLibKernel, so all four resolved to the
   error-stub. Register them under both libraries (the NID is not in db/360).

2. **mspace handle semantics.** `mspace_create` returned a host-allocated
   `VitaMspaceImpl *`. A dlmalloc mspace handle is the `malloc_state` at the
   start of the guest region (`create_mspace_with_base` returns it), and Unity
   casts the handle back to `mstate` in its own linked dlmalloc. With a host
   pointer, Unity's chunk bookkeeping read zeros; gdb showed
   `memcpy(dst, src, 0xfffffff8)` inside dlmalloc's realloc path
   (`oldsize - CHUNK_OVERHEAD` with `oldsize = 0`) reading past the end of a
   1 MB "Heap" memblock. `vita_mspace_*` now take the raw mstate and keep the
   region/live metadata in a side registry; an unregistered handle (created by
   the title's own dlmalloc) is passed through unchanged.

After both fixes birdunity gets past the asset loads and dies later in an
abort from Unity's preload thread, still before the first
`sceDisplaySetFrameBuf`. The remaining suspect is dlmalloc configuration drift
between the vendored 2.8.6 (ONLY_MSPACES, no locks, no mmap) and the copy
inside Unity 2017's eboot — a title built against a different malloc_state
layout or chunk alignment. Next step if this title matters: dump the guest's
`malloc_state` (top/topsize/seg fields) for a freshly created mspace and
compare it with our `mstate` struct, or fetch the exact dlmalloc revision from
the Unity 2017 tree.

Regressions after the change: SuDokuL, PixelChess and VitaShell still render;
mspace/threadmgr/sync smoke tests pass; x86-64 builds clean. The file-I/O trace
now also covers pread/lseek/getstat/close.

## Session: thread-local TLS, libm ABI, and Unity's first frames

The abort after "GC_push_all_stacks: Collecting from unknown thread" was not a
GC problem. `vita_tls_get_addr` returned one block per module for the whole
process, so every guest thread read the same `__thread` variables. IL2CPP
stores "this thread is attached" there, so every new Unity thread saw the main
thread's Thread object, skipped `GC_register_my_thread`, and the first
collection on a worker thread aborted. TLS blocks are now `_Thread_local`.
GDB (attached after startup, since software breakpoints cannot be inserted in
the RX JIT pages and hbreak fails on this kernel) showed the GC table at
`0xe76c888c`, hashed by the low byte of the pthread_t pointer, holding exactly
one node: the main thread.

With the abort gone the title next crashed in `vita_modf` with a NULL
out-pointer. Its eboot calls libm with the hard-float ABI (`vorr d0, d8, d8`
before the call), not the softfp ABI `EXPORT_SOFTFP` had assumed, so every
SceLibm entry is a plain EXPORT now. `_fsinx`/`_sinx` are not "sin plus a
sign out-pointer": they take `(float x, int flags)` where bit 0 selects cosine
and bit 1 negates. Verified against the guest libc.suprx implementation.

Rendering then stalled twice:
- `sceGxmMidSceneFlush` dropped its notification; Unity's GfxDevice worker
  waits on that fence from another thread. EndScene's publisher is now used
  there too.
- The built-in mask-update fragment program had no GL shader, so linking a
  program with only a vertex shader failed. It gets a minimal `#version 310
  es` shader.

Compressed textures (UBC1/2/3) decode, but a 4x2 UBC3 texture with ten mips
failed at the 1x1 level because the block layout used `layout_width / 4`
(truncating to zero blocks). Rounding up fixed it.

Current state: birdunity runs indefinitely, presents frames at ~5 fps, and no
longer aborts, but the framebuffer readback is still only the clear colour
(`VITA2NATIVE_GXM_FRAME_TRACE`: `nz=0`). Three GL draws with twelve vertices
execute per frame with sane pixel-space vertices, so the next suspects are the
default uniform block contents/placement and the translated vertex program
(the shader's secondary program may be running unconditionally). Separately,
the depth force-load path in `sceGxmBeginScene` is extremely slow — skipping
it restores 5 fps, so it needs to be made O(rows) with memcpy instead of a
per-pixel loop.

### Follow-up: what the draw trace says now

- The green screen is not an empty framebuffer: Unity's first draw (GXP
  0xe3132c6c, no texture, blend off, viewport disabled) is a full-surface
  background quad and it paints the colour. Rasterisation, attribute binding
  and the pixel->NDC path work.
- The UI draws (GXP a07d8387 / e3caf88c with 9 default uniform vec4s) have the
  viewport enabled, so the shader's `gl_Position = outs[0]` branch runs with
  `outs[0].x = dot(sa[4], v)`, `sa[4] = (0.0036,0,0,0)` and
  `sa[5] = (0,0.0064,0,0)`: pixel vertices (-76..84) map to NDC (-0.27..0.30),
  so the quads are on screen. They are invisible because the vertex colour
  comes out with alpha 0: `outs[1] = (sa[1].z, sa[1].w, sa[0].x, sa[0].y) *
  v.color`, and `sa[0].y` is 0 (identity-ish matrix), or the secondary program
  overwrites `sa[0].xy`/`sa[1].zw` from a half-unpack of `sa[8]` (1.0f), which
  also lands on alpha 0. Blending is ONE/ONE_MINUS_SRC_ALPHA, so alpha 0
  leaves the background untouched.
- Shifting the uniform container by 1..3 vec4s does not change the result, so
  it is not a simple offset error. Next: compare this GXP's register mapping
  with Vita3K (run the same program through its GL backend), or check whether
  the secondary program should run only under its GXP predicate.

Depth force-load is now a strided word copy (~29 ms/frame) instead of a
per-pixel loop with an allocation, which brought the title back to ~5 fps.

## The nine-second blank was a full shader cache

The UI disappearing after ~9 s and the frame trace turning flat green was not
a scene change or a stalled thread. `gxm_get_linked_program` keyed its 128-slot
table on `SceGxmVertexProgram *` / `SceGxmFragmentProgram *`. Unity recreates
those patcher objects every frame (new pointers, same GXP bytes), so the table
added four entries per frame and filled at frame ~34; every later draw then
failed the lookup and was dropped without a word. The clear passes had been
cached already, which is why the screen went to a clean green.

Fix: key both the linked-program table and the new GL shader cache on
`gxm_shader_cache_hash` (GXP bytes + stage + vertex attribute layout), and evict
a slot instead of starving if a title really has more variants than the table
holds. `VITA2NATIVE_GXM_DRAW_ERRORS` now reports rejected draws, which is how
the table was caught at 128/128.

With that fixed the menu stays up indefinitely, holds 7 draws / 534 vertices,
and reacts to input.

### Input

The ctrl script only existed in the null pad backend while the SDL build uses
the linux one, so scripted runs could not press anything. It is now a shared
module (`SceCtrl_script.c`) used by both backends, and `VITA2NATIVE_TOUCH_SCRIPT`
does the same for the front touch panel (`40:960/544,60:`), which is what this
title actually reads for UI clicks. A tap on the menu changes the highlight and
the draw count (7/534 -> 8/1044), so the game logic is alive.

### Still open

- The UI renders as chunky black blocks. The font atlas decodes correctly
  (256x512 U8_R000, glyphs opaque, background transparent) and the text mesh
  UVs vary per glyph, but the glyph quads sample large atlas cells; needs a
  comparison against hardware/Vita3K before calling it a bug.
- The menu is a list of rows (some highlighted on tap); we have not yet found
  the row that starts a run.

## Half uniforms from Unity are written as floats

With the cache fixed the menu stayed up but every sprite drew green. The raw
texture dump proved the atlas, the ground/sky sheet (pvrt?) and the bird were
decoded correctly. The colour came out as `(0,1,0,1)`: the vertex program
0x93846180 declares `_RendererColor` and `_Color` as **F16** vec4 uniforms at
SA registers 32/34, and Unity writes plain 32-bit floats there (0x3F800000 per
word). Unpacking those words as half pairs gives `(0,1)` for every component,
so the renderer kept only the green channel.

Fix: in the USSE translator, an F16 operand loaded from the uniform bank
(`RegisterBank::SECATTR`) is now expanded as `vec2(v, v)` instead of
`unpackHalf2x16(v)`, which is what the guest data actually means. Varyings and
attributes keep the packed interpretation (VitaShell, SuDokuL and PixelChess
still render unchanged). Set `VITA2NATIVE_PACKED_HALF_UNIFORMS=1` to force the
old behaviour.

The title now runs: menu, tap to start, flapping bird, sky/cloud/city
background, grass ground and the green pipes all render (the pipe texture is
52x320 and decodes correctly; earlier white boxes were UI buttons, not pipes).
What is still wrong is the UI text: glyph quads come out as white blocks with
black outlines instead of the font atlas' black letters. The atlas itself is
correct (256x512 U8_R000, black glyphs on transparent) and the death-panel
buttons and pipes are right, so the remaining suspect is the text fragment
program 4973cf18, which adds a fragment-uniform vector to the texture sample.

Useful new traces: `VITA3K_TRACE_UNIFORMS` dumps GXP containers and parameter
types, `VITA2NATIVE_GXM_VERT_TRACE=<frame>` dumps a frame's batches, vertex
layouts and default uniform words.

### Text: what is left

The death/start panels show their button images but no glyphs. Numbers from the
trace: the text program `e3caf88c` + `4973cf18` has default uniforms
`u8 = (1,1,1,0)` (three `0x3F800000` and one `0x00000000`), and the shader
multiplies the two unpacked colours (`_RendererColor` at SA 32, `_Color` at 34),
which collapses `v_Color0` to `(0,0,0,0)` either way the words are read
(packed halves give `(0,1,0,1)` and `(0,1,0,0)`). Overriding `v_Color0` with the
raw vertex colour does not bring the glyphs back either, so the remaining fault
is upstream of the colour maths.

Useful probes added: `VITA2NATIVE_POISON_UNIFORMS` fills a reserved default
uniform buffer with 0xCD so you can see which words the guest really writes
(it writes all 36 here), and `VITA2NATIVE_GXM_PROGRAM_TRACE` now dumps the whole
default uniform buffer passed to `sceGxmSetVertexDefaultUniformBuffer`.

## The Unity half-uniform mystery was an API bug

The green sprites and the opaque text blocks shared one root cause, and it was
not in the shader translator: `sceGxmSetUniformDataF` takes a `const float *`
and converts it to the parameter's declared type, but the emulator copied
`component_count * sizeof(F16)` bytes straight from the float array. For a
half4 that is 2 bytes per float, so Unity's `_Color`/`_RendererColor`/
`_TextureSampleAdd` values landed in the buffer as the first two raw 32-bit
floats (e.g. `0x3F800000 0x3F800000`). The translator then unpacked those words
as half pairs and got `(0,1,0,1)` -> green sprites. The `_TextureSampleAdd`
half4 wants `(1,1,1,0)`; as raw floats it became either `(0,1,0,1)` (packed
read) or `(1,1,1,1)` (the `vec2(v,v)` workaround), and the latter saturates
glyph alpha, which is exactly why text quads were opaque white rectangles.

Fix: `sceGxmSetUniformDataF` now converts every source float to IEEE half with
round-to-nearest-even and packs two halves per 32-bit word for F16 parameters.
The `vec2(v, v)` uniform hack in the vendored USSE translator is reverted, so
GXP data is once again interpreted as packed halves, and
`VITA2NATIVE_PACKED_HALF_UNIFORMS` is gone. Result: the Unity title's
`Flappy Bird` title and `Play` text render correctly with the standard
translator and no cached-GLSL patching; the sprite background is correct too
(it was already the regression case for the green bug).

Verified under qemu/softpipe on the x86_64 VM: birdunity frame 0 (menu text,
title, button panel), PixelChess' start screen, `shader_smoke` and
`gxm_draw_smoke`. opiz3's sshd stopped answering after the previous run, so the
ARM32 hardware re-check of the other titles is still pending.

Debug aids kept from the investigation: `VITA2NATIVE_GXM_SHADER_DUMP=<dir>`
writes the USSE disassembly (`.dsm`) and SPIR-V per translated shader;
`VITA2NATIVE_GXM_UNIFORM_TRACE=<frag hash>` logs the default uniform words and
texture of distinct draws; `VITA2NATIVE_UNIFORM_PADDING=<bytes>` pads each
default-uniform reservation, and with `VITA2NATIVE_POISON_UNIFORMS` shows
whether a title writes past the declared size (Unity does not: it writes only
the declared 2 words for a half4 fragment uniform).

### ARM32 hardware re-check after the F16 fix

Ran the fixed build on opiz3 (Mali-G31/Panfrost) inside the VNC session on
`DISPLAY=:1`, window at 42,170, 960x544, SDL x11 presentation. The Unity title
now renders correctly end to end: "Flappy Bird" title and "Play" text, the
Game Over panel ("Game Over", "Points: 0", "Replay", "Return"), and the
gameplay bird/pipes/background all have their intended colours. Mouse clicks
inside the SDL window are forwarded to the front touch panel, so the game is
playable there without `VITA2NATIVE_TOUCH_SCRIPT`.

### Regression pass after the F16 fix (opiz3, VNC :1)

VitaShell 2.02, SuDokuL 1.5, PixelChess and birdunity all render correctly on
the Mali-G31 board with the fixed build. The Game Over panel, the Flappy Bird
title/Play screen and gameplay (bird, pipes, background) are correct, and
VitaShell's file list and SuDokuL's colour banner/checkerboard match their
pre-fix screenshots.

Capture caveat: `xwd` output from the VNC display contains a 256-entry
PseudoColor colormap chunk between the header and the pixels. Convert those
files with ImageMagick (`convert shot.xwd shot.png`); reading the raw pixels
without the colormap produces misleading grey/rotated colours. The earlier
"SuDokuL went grey" observation was this artifact, not a renderer regression.

### CrossyRoadsClone (Unity 2018.2 IL2CPP): loader findings

Trying `CrossyRoadsClone.vpk` exposed two real loader bugs and one remaining
wall:

- Vita paths are case-insensitive. `fix_path_case()` only handled absolute
  paths, while `app0` is a relative directory, so the title's
  `Media/Modules/Il2CppAssemblies.suprx` lookup missed the extracted
  `il2CppAssemblies.suprx`. Relative paths are now case-fixed too.
- `vpk_extract()` left the previous title's files in `vita2native/app0`.
  Running VitaShell after CrossyRoads autoloaded CrossyRoads' IL2CPP and
  pthread modules into VitaShell and segfaulted it (this was the suspected
  "VitaShell regression"). The destination is now cleared when the VPK
  changes, and the stamp is versioned so existing polluted directories are
  refreshed once.

With those fixed, CrossyRoads loads its bundled libc/pthread/IL2CPP, starts
them, creates the Unity worker threads and then SIGSEGVs inside IL2CPP
initialization with the PC at a data value (`0x654b6562`). Experiments
(module deduplication, deferring `Media/Modules` starts until the title asks,
registering the main executable for `sceKernelGetModuleInfoByAddr`) changed the
failure (PC became a small link-time offset) but did not fix it, and were
reverted to keep the working titles safe. The remaining work is IL2CPP /
module-loader debugging, not a quick fix.

### CrossyRoadsClone boots to the title screen

Two fixes unlocked the title:

- `sceKernelGetModuleInfoByAddr` returned a zeroed `SceKernelModuleInfo`, so
  libc's C++ unwinder had no EXIDX/EXTAB tables to walk. `LoadedModuleInfo` now
  keeps the module's `exidx_top/exidx_btm/extab_top/extab_btm` (relative to the
  module-info segment) and the TLS fields, and `module_get_info` copies them.
  The old `pc=0x654b6562` crash during IL2CPP init is gone.
- The title's gather4.uv shader makes SPIRV-Cross emit `textureQueryLOD` plus
  `#extension GL_EXT_texture_query_lod`, which Panfrost does not expose.
  `spirv_to_glsl_es` now rewrites that call to the equivalent derivative-based
  helper (2D/3D/cube/array overloads) and drops the extension directive. The
  shader cache version is bumped to 3.

Unity also loads `Media/Modules/pthread.suprx` and
`Media/Modules/Il2CppAssemblies.suprx` itself after the application loader
already started them. `module_load` now dedups by resolved host path (same
file, not same library name) and `sceKernelLoadStartModule` treats an
already-started module as success, matching the firmware's one-image-per-file
behaviour. This did not change the remaining crash but removes a second 25 MB
IL2CPP image and a second module_start.

The title screen renders correctly (credits panel, logo, three buttons,
voxel background) and animates on Mali-G31 with the software SDL renderer.

### Remaining wall: pressing a UI button

Pressing Play crashes deterministically at `pc=il2cpp+0x80d86c`
(`fault=0x8`); pressing the gear button crashes at `pc=0` through a null
class vtable slot (`[klass+0xe4]`). Disassembly of the dumped module
(`VITA2NATIVE_DUMP_MODULE_CODE=<dir>` writes the RX image) shows:

- Play: `Runtime`-style invoker chain (0x649bc -> 0x3ba05e -> 0x83b402 ->
  0x818370 -> 0x80d790). The last function interprets `arg0` as an
  `Il2CppClass*` (`name` at +8, `namespaze` at +0xc); `arg0` is null. The null
  came from an array element fetched with a bounds-checked getter (0x4d3826).
- Gear: the handler iterates a list and performs an interface cast, then calls
  `[klass+0xe4]` as a virtual method; the slot is null.

Both look like IL2CPP type/vtable data that was never initialized, rather than
a bad input event or a missing file API. Background taps do not crash, so
input delivery itself is fine. The next step is to find why those specific
classes/vtables are missing (candidates: generic/RGCTX initialization, class
init interrupted by an earlier managed exception, or code-registration data
in the module's data segment). Module dedup, host mspace and the shader fix
were all ruled out as causes by testing.

### Regression pass (opiz3, VNC :1) after these changes

birdunity (Game Over panel and gameplay colours), VitaShell 2.02, SuDokuL 1.5
and PixelChess all render correctly. `shader_smoke` (clear + texture) and
`gxm_draw_smoke` pass on the x86_64 VM with llvmpipe.

Commits: exidx/EXTAB/TLS + dedup, texture-query-LOD lowering, crash-time
diagnostics (SIGABRT handler, guest abort logging, crash code dump).

## Root cause of the CrossyRoadsClone button crash (fixed)

The button crash was a **Boehm GC stack base bug caused by our
`sceKernelGetThreadInfo`**. Chain:

1. Unity's IL2CPP links Boehm; the Vita port supplies
   `psp2_get_stack_bottom()` (`il2cpp+0x87fa6e`), which GC_init uses as
   `GC_stackbottom`.
2. That function calls `sceKernelGetThreadInfo()` and returns
   `info.stack + info.stackSize`.
3. Our implementation set `stackSize` but never `stack`, so the result was
   `0 + 0x100000 = 0x100000`, not a stack address.
4. `GC_push_all_stacks` then pushed the range `0x100000 .. current_sp` for the
   main thread. The live stack sits *above* the current SP, so it was never
   scanned.
5. The first GC raised during the button press (one collection, verified with
   a watchpoint on `GC_stackbottom`) therefore collected managed objects that
   were only referenced from the main thread's stack: the `MethodInfo[]`
   array and the reflection method objects cached by
   `il2cpp::vm::Reflection::GetMethodObject`.
6. `RuntimeType.GetMethodsByName` then read a freed object whose
   `RuntimeMethodHandle` was zero and called
   `MethodBase.GetMethodFromHandleNoGenericCheck(0, ...)`, which reached
   `Reflection::GetMethodObject(NULL, klass)` and faulted at
   `pc=il2cpp+0x80d86c` (`fault=0x8`).

Evidence: reading `GC_stackbottom` gave `0x100000`; setting it to the real
stack top (`0xe0a01000`) from gdb made Play work and produced gameplay.
Capturing the write with a hardware watchpoint showed GC_init storing the
value returned by `psp2_get_stack_bottom`.

Fix: record each guest thread's host stack bounds in `thread_entry_wrapper`
(new `threadGetStackBounds` in `host_thread.c`) and report
`SceKernelThreadInfo.stack` such that `stack + stackSize` is the real top of
the host stack. `GC_stackbottom` is now `0xe0a01000`; pressing Play enters the
gameplay scene with no guest signal.

Verification so far: Play -> gameplay on opiz3 (screenshot
`build/debug_frames/crossy_fixed.png`); `threadmgr_smoke`, `sync_smoke`,
`fiber_smoke`, `mspace_smoke`, `fios2_smoke` pass on opiz3. The full
title-regression pass and the x86_64 smoke tests remain to be rerun (the VNC
display and the builder VM were unavailable at the end of the session).

Diagnostics added while tracking this down and kept for future guests:
`VITA2NATIVE_MODULE_BKPT` / `VITA2NATIVE_MODULE_PATCH` (module-relative
breakpoints/patches), `VITA2NATIVE_DUMP_MODULE_CODE` (RX and data segments),
`VITA2NATIVE_CRASH_DUMP_VEC` (growable pointer vectors) and
`VITA2NATIVE_CRASH_DUMP_ARRAY`.

### Full regression after the stack-base fix (opiz3, VNC :1, no VM needed)

CrossyRoadsClone: title screen, Play -> gameplay, in-game gear -> OPTIONS,
close -> gameplay resumes, title gear -> OPTIONS (previously crashed at
`pc=0` through the null `[klass+0xe4]` slot), Characters, Powerups and Prize
Machine screens all open. No guest signals in any run.

birdunity: title screen and Play -> gameplay.
VitaShell 2.02: device/file list renders.
SuDokuL 1.5: title screen renders.
PixelChess: "TOUCH SCREEN TO START" renders.

Screenshots in `build/debug_frames/reg_*.png`. The x86_64 `shader_smoke` /
`gxm_draw_smoke` run on the builder VM and were skipped this time (VM
unreachable); the ARM build and all title tests ran on opiz3.

Bonus: xwd screenshots can be converted on the Mac with ffmpeg
(`ffmpeg -i shot.xwd -frames:v 1 shot.png`), so the VM is no longer needed for
image conversion.

## Session: shadow-map / colour investigation (reference build on scratchvm)

`ssh scratchvm` works. A full Linux x86_64 build of the same game was
sparse-cloned from `MRKane05/CrossyRoadClone_Unity` into `~/crossy-ref`
in 60 MB and now runs on the **TigerVNC display `:1`** (1024x768, window
960x544 at 42,170) with `LIBGL_ALWAYS_SOFTWARE=1`. This is the reference
render: roads are light grey asphalt with white dashes, grass is a soft
yellow-green, water is cyan, and shadows are soft grey blobs. Our VPK
build on opiz3 does none of that: roads/log tops are pure black, grass is
neon, water is flat cyan.

### What was measured (all env-gated in the tree)

* **Materials are colour-only.** `Assets/Materials/*.mat` have no
  `_MainTex`; `_Color` is: Road 0.5377 grey, RoadDark 0.3113, Grass
  (0.502,1,0.251), GrassDark (0.374,0.753,0.189), Water (0.251,1,1),
  WaterDark (0.189,0.748,0.753), Dirt (0.519,0.413,0.184),
  TreeTrunk (0.502,0.268,0), Skin (1,1,0.753).
* **`_Color` is fed per draw and is correct.** One program
  (`8b3f592b`, Unity's Standard variant) draws every ground material;
  `VITA2NATIVE_DRAW_COLOR_TRACE` logged Road 0.5376, RoadDark 0.3113,
  Grass, GrassDark, Water, WaterDark, Dirt, ... in the same frame. The
  values are **packed fp16 pairs** (`0x3C4A...`), not floats, which is why
  a naive float print looks like garbage. Forcing `_Color` to red with
  `VITA2NATIVE_FORCE_COLOR=FF0000` turns the road bright red, so the
  uniform does reach the fragment shader.
* **Samplers**: unit 0 = `fragTex_ShadowMapTexture`, bound to the
  512x512 F32M texture; unit 1 = `fragTex_MainTex`, bound to the material
  texture (256x256 UBC1 for textured rows, otherwise a 4x4 white
  U8U8U8U8). `VITA2NATIVE_SKIP_PROGRAM` / `VITA2NATIVE_CLEAR_COLOR=FF00FF`
  proved every pixel is covered by geometry (no magenta anywhere), i.e.
  the black is drawn, not a clear.
* **SH ambient is populated**: `unity_SHAr/Ag/Ab` decode as
  (-0.0098,-0.017,0,0.1575) / (-0.016,0.027,0,0.196) /
  (-0.030,0.112,0,0.275) for the Standard program, i.e. a bluish ambient.
  `_LightColor0` is 0.9 grey.
* **The shadow map is the 512x512 F32M texture at 0x8e17d380.** It is
  filled with `0x3f800000` (1.0f) and re-uploaded many times per frame.
  Forcing it to 1.0 with `VITA2NATIVE_FORCE_SHADOW_LIT=1` turns the black
  road bright/white and removes the black bands, so **the black is the
  shadow attenuation**, not the albedo.
* Our render targets are always **GL_RGBA8**, so the guest's float shadow
  surface is quantised to 8 bits on writeback; the dumped map has only
  ~58 distinct levels over 0.67..1.0. Vita3K uploads F32M as R32F.
  `VITA2NATIVE_FLOAT_TEXTURES=1` implements that path
  (`gxm_texture_decode_level_f32`), but on its own it turns the black
  bands into fine per-pixel shadow acne stripes: the depth comparison is
  systematically offset, and quantisation only hid that by coalescing the
  acne into large blocks. `VITA2NATIVE_MASK_REPLICATE=1` (R001 -> RRR1)
  brightens the whole scene but does not fix the road.
* `VITA2NATIVE_DEBUG_PROGRAM_COLORS=1` paints each fragment program a
  unique flat colour (alpha preserved). It showed the `vec4(0)` programs
  (13c3fe41, 16159aff, 5f9b0a3a, 90031d40, bbd7c433, c5ce60d7) are
  invisible, so their colour masks are honoured (shadow-caster passes).
  The PlanarShadow shader is not used by this build.

### Next step for the shadow bug

The shadow lookup is offset: full-float depth makes the acne visible, so
the fragment's computed shadow depth and the map disagree by more than
Unity's bias. Check, in order: (1) `unity_WorldToShadow` /
`unity_LightShadowBias` values in the uniform block for the Standard
program, (2) whether the shadow map's Y orientation or half-texel offset
is flipped relative to the lookup, (3) the `z_offset`/`z_scale` values in
`renderVertInfo` (they feed the clip-space depth used by the compare).
A glTF-style test is to patch the GLSL `_670` (shadow UV) by half a texel
and by a Y flip at translation time and see which removes the acne.

Reference screenshots: `build/debug_frames/ref/vnc*.png`, our frames:
`build/debug_frames/base2.png`, `draw1.png`, `shadowlit1.png`,
`f32_1.png` (float acne), `repl1.png`, `forcered1.png`.

### Root cause found and fixed: texture-format hints for the shader recompiler

The black roads were never a texture, material-uniform or lighting-value
problem. The GXP recompiler chooses the shape of every texture fetch from
the **format hint** handed to it: `get_texture_component_count()` returns
1 for `F32M_R` but 4 for `U8U8U8U8_ABGR`, and the translator emits one
`textureGather` for a 1-component format and four (all channels of each
texel) for a 4-component one. Our C interface hardcoded
`U8U8U8U8_ABGR` for all 16 units (the upstream "TODO: Fill this
properly"), so the 512x512 F32M shadow map was fetched as RGBA and the
PCF compare in Unity's Standard shader was compiled from the wrong
register layout. The ground then evaluated as shadowed: roads and log
tops came out pure black, while grass (whose `_Color` is bright) stayed
saturated. Forcing `_Color` red or the shadow map to 1.0 changed the
symptom, which is what made this look like a lighting bug.

Fix (`9ef581c`):

* `convert_gxp_to_spirv_c_hinted()` / `convert_gxp_to_glsl_c_hinted()`
  take the bound fragment/vertex texture formats and colour format.
* Shader translation is deferred from `sceGxmCreate*Program()` to the
  first draw (`gxm_ensure_shaders()`), where the guest has bound its
  textures. A per-stage signature of `(component_type, component_count)`
  decides when to re-translate; it is part of the shader cache key. Raw
  formats are not hashed, so BC1 and U8U8U8U8 albedo (both UINT8x4) do
  not cause churn.
* Mask-update fragment programs have no GXP header and are skipped.

Result: CrossyRoadsClone now matches the reference Linux build - grey
roads with rails, brown textured logs, natural green grass, cyan water,
soft shadows. Regression: Flappy Bird, VitaShell 2.02, SuDokuL 1.5 and
PixelChess render unchanged (`build/debug_frames/reg2-*.png`).
Screenshots: `build/debug_frames/hint1.png`, `hint2.png`.

Remaining loose ends:

* The F32M shadow map is still uploaded as RGBA8 (8-bit depth). It looks
  correct now, but Vita3K uses R32F; the opt-in
  `VITA2NATIVE_FLOAT_TEXTURES` path exists and should be retested with
  the hint fix in place.
* The 512x512 shadow map used to be re-uploaded ~55x/frame at ~140 ms
  each; the hint fix does not change that. The emulator logs
  `render target 512x512`, so the map really is an offscreen pass.
* `VITA2NATIVE_MASK_REPLICATE` (R001 -> RRR1/RRRR) was only a diagnostic
  and is unnecessary now.

## Perf investigation: where a CrossyRoads frame actually goes

Measured on opiz3 with `VITA2NATIVE_PROFILE=1` during gameplay
(960x544, software GL, after the hint fix):

```
6 frames in 2.5 s (~417 ms/frame)
  ds        149.1 ms   (store ~107, load 41.9 for 1 call)
  finish     88.0 ms   (glFinish at EndScene; this is the GPU render)
  draw       68.5 ms   (uniform 29.5, tex 25.5, gldraw 6.1, flush 8.5)
  read       52.0 ms   (glReadPixels of the 960x544 colour surface)
  resolve     4.0 ms
  convert     2.6 ms
  158 GL draws, 6.5k vertices
```

The earlier note about the shadow map ("55+ uploads/frame at ~140 ms")
is **not** what the current build does: `VITA2NATIVE_TRACE_F32M` shows
**one** 512x512 F32M upload every ~3 frames. After `5c21ab3` (R32F) it
costs ~6 ms decode + ~2 ms upload instead of ~31 + 2, i.e. ~2-3 ms/frame
on average. It was never the dominant cost.

Real next targets, in order:

1. **`ds` 149 ms/frame.** `gxm_ds_store_surface()` writes the whole
   960x544 depth/stencil surface back to guest memory every frame, and
   `gxm_ds_load_surface()` reads it back at BeginScene (42 ms). Check
   whether the guest needs the store at all (some titles only read it
   after a mid-scene flush), and whether load+store can be skipped when
   the depth surface is the same one the previous frame ended with.
2. **`read` 52 ms + `finish` 88 ms.** The display path does
   `glFinish()` then `glReadPixels()` into guest memory, then converts.
   A PBO readback or reading straight into the guest surface (the ABGR
   fast path already exists after the read) would overlap some of this.
3. **`draw` 68 ms**, of which uniform 29.5 ms and tex 25.5 ms for 158
   draws. Uniforms are re-uploaded with `glBufferSubData` per draw;
   textures are rebound through all 32 units per draw. Batching more
   draws or checking the uniform/texture pointer identity before the
   upload would cut this.

`VITA2NATIVE_TRACE_F32M` also logs the decode/upload split per upload
now, and `VITA2NATIVE_PROFILE` prints the breakdown above every 2 s.

### Depth/stencil readback optimisation (`f4a09a0`)

After the R32F change a CrossyRoads frame spent **149 ms** in the ds path:
~100 ms reading the 960x544 main depth surface back to guest memory,
~50 ms for the 512x512 one, and ~42 ms re-loading it at BeginScene.

Key observation from `VITA2NATIVE_DS_DEBUG=1` plus the texture trace:

* the 960x544 depth surface (0x8d400000) is **never** sampled as a
  texture (0 uploads point at it), so its readback was pure waste;
* the 512x512 "shadow map" texture (F32M at 0x8e17d380) **is** the
  DF32M depth surface of the 512x512 render target - it is sampled, so
  that store is required.

The GL depth attachment is persistent across scenes, so:

* `gxm_ds_note_texture()` latches `ds_sampled` when an uploaded texture
  points at a render target's depth surface; EndScene only stores when
  that latch is set.
* `gxm_ds_load_surface()` returns "attachment already current" (and
  suppresses the BeginScene clear) when the same surface is reused.
* `sceGxmTransferFill/Copy` latch `ds_sampled` when reading depth memory
  and clear `ds_gl_valid` when writing it.
* a row-copy fast path handles linear packed S8D24 stores.

Result: **ds 149 -> 31 ms/frame**, ~2.4 -> ~4 fps (6 frames/2.5 s ->
8 frames/2.1 s). Flappy Bird pixel-identical, VitaShell/SuDokuL/
PixelChess unchanged.

Remaining frame budget (~230 ms at 4 fps):

```
finish   88 ms   glFinish at EndScene (GPU render; software llvmpipe)
read     53 ms   glReadPixels of the 960x544 colour surface
draw     60 ms   uniform 31, tex 13, gldraw 7, flush 10
ds       31 ms   the 512x512 shadow-map depth store
resolve   5 ms
convert   2.6 ms
```

Next candidates:

1. **`finish` + `read` (141 ms).** Use a PBO for the colour readback and
   avoid the full `glFinish` (the read is already a sync point). On
   llvmpipe this may not overlap much, but the finish is redundant with
   the read.
2. **512x512 depth store (31 ms).** Bind the render target's GL depth
   texture as the shadow map instead of round-tripping through guest
   memory (skip both the store and the R32F upload). Needs a check that
   `textureGather` works on a DEPTH24_STENCIL8 texture on Panfrost.
3. **`draw` 60 ms.** Uniform re-upload per draw (31 ms) and rebinding
   all 32 texture units per draw (13 ms).

### Frame-time work, round 2 (`59009b6`, `92e61e1`, `2cc3e22`)

Starting point for this round: ~262-275 ms/frame (~3.6-4 fps) during
gameplay. End point: **~200 ms/frame (~5 fps)** with the same visuals,
and ~170 ms with `VITA2NATIVE_NO_MSAA`.

Measured breakdown before -> after:

```
finish   95   -> 95    glFinish at EndScene (GPU; includes MSAA)
read     54   -> 54    glReadPixels of the 960x544 colour surface
draw     58   -> 24    uniform 28->8.5, tex 16->2.5, flush 9->8
resolve   5   -> 5
ds       31   -> 0.1   shadow-map depth round trip, now GPU-aliased
convert   2.6 -> 2.6
```

What worked:

* **Uniform buffer orphaning.** `glBufferSubData` into a UBO that queued
  draws still reference makes Panfrost copy or stall, and it ran once
  per batch (~150 times/frame). `glBufferData(..., NULL)` first:
  uniform 28 -> 12 ms.
* **Texture state memoisation.** Sampler state was re-applied on every
  bind of every unit (64 units x 150 draws). The state now lives on the
  cache entry and is skipped when the descriptor is unchanged; the
  per-unit descriptor is also memoised per frame so unchanged units
  never rescan the cache. tex 16 -> 2.5-5 ms.
* **Uniform skip.** Many consecutive draws reuse byte-identical default
  uniform blocks; compare against a shadow copy and skip the upload.
  uniform 12 -> 8.5 ms.
* **Depth aliasing** (`VITA2NATIVE_DEPTH_ALIAS`, on by default). The
  512x512 shadow map is the depth attachment of a render target and is
  sampled as F32M. Instead of glReadPixels -> guest floats -> decode ->
  R32F upload every frame, one fullscreen pass exports the depth
  attachment into an R32F texture at EndScene (1.0 ms on the GPU,
  sampled with a flipped v to match the uploader's orientation), and
  the guest's texture bind uses that texture directly. Guest memory is
  still written when a guest transfer reads it. ds 31 -> 0.1 ms,
  tex 14.5 -> 2.5 ms.

What did not work:

* **PBO colour readback.** `glReadPixels` into a pack buffer and
  `glMapBufferRange` costs **106 ms** vs 54 ms for client memory on
  this Panfrost build. Rejected.
* **Renderbuffer colour attachment.** Identical readback cost; no
  reason to change the attachment type.
* **EGL window presentation.** X11 EGL on this device resolves to
  **llvmpipe** (no DRI3), so presenting from a texture would move all
  rendering to software. The SDL software presenter (guest memory ->
  SDL_UpdateTexture -> XShm) remains the only viable path, so the
  glReadPixels cannot be removed outright.

`VITA2NATIVE_SKIP_READBACK` measures the frame without the colour
readback: 10 frames/2.0 s vs 8/2.2 s, i.e. ~85 ms of the frame is the
readback plus the copies it feeds.

`VITA2NATIVE_NO_MSAA` shows the 2x MSAA cost: finish 95 -> 66 ms and
resolve 5 -> 0 ms, ~30-35 ms/frame total.

Next candidates:

1. **Overlap the colour readback (~54 ms).** Blit the resolved colour
   attachment to a ping-pong texture at EndScene (~1 ms), then let the
   SDL presenter thread read it back through a shared GL context while
   the guest starts the next frame. Needs a second EGL context, a
   fence, and two present textures; costs one frame of latency unless
   the presenter finishes before the next EndScene.
2. **GPU cost (95 ms).** Mostly the game's own draw load. MSAA is the
   only knob found so far.
3. **Draw path (24 ms).** flush 8 ms for ~150 GL draws (Mesa
   submission overhead), attrib 0.8, state 0.8.

## Session: maintainability review and skills (2026-11-11)

Context: the build presets were just rewritten (`bc26c6a`) so that
`cmake --preset arm-rel|arm-debug|arm-minsize|x64-syntax` describes the builds
that are actually used. This section is the survey that came out of asking
"what else is rotting", with evidence, so the work survives context loss.

### Measured state

- 338 tracked files; 60 module sources vs 20 module headers under
  `include/modules`.
- 178 `getenv()` sites, **120 distinct `VITA2NATIVE_*` variables**, 84 of them
  in `SceGxm.c` alone; 6 modules each hand-roll a `*_debug()` getenv cache.
- 411 `LOG()` sites vs 139 raw `fprintf(stderr, ...)`, the latter without
  level/timestamp discipline.
- 39 test executables; 31 repeated `target_include_directories(... PRIVATE
  include ...)` blocks; 15 of 40 tests define their own `CHECK` macro with
  subtly different semantics (varargs vs `message`).
- `CMakeLists.txt` is 858 lines in one file (deps + sources + 39 tests +
  install).
- `source/modules/SceGxm.c` is 10,392 lines: state, batching, texture upload,
  readback/present, depth export, probes and frame dumps in one TU. Three of
  the last four rendering fixes landed there.
- 109 ad-hoc `.sh`/`.gdb` scratch files in the gitignored `build/`.
- No CI. The `x64-syntax` preset builds the whole tree, so a GitHub workflow
  could compile-test every push and run the portable smoke tests on Linux x64.

### Dead weight (was item 1, done in `c3c6550`)

- [x] `source/modules/SceAudio_null.c` was not referenced by any target ->
      deleted (`SceAudio_sdl.c` dlopens SDL in both builds).
- [x] ~~`SceGxmContext.viewport_flip[4]` is unused~~ — stale item: that field no
      longer exists. The live one is `GXMRenderVertUniformBlock.viewport_flip`
      (SceGxm.c:74), the first member of the vertex uniform block, set from
      `flip_y` in `gxm_flush_state`. Do not delete it.
- [x] Duplicate `install(TARGETS vita2native ... bin)` removed; the rule at the
      top of the file (with `${CMAKE_INSTALL_BINDIR}`) is the only one.
- [x] `tools/opiz3-vnc/README.md` + `v2n.conf.example` are title-agnostic now,
      and `xstartup` refuses to start without `V2N_APP` instead of defaulting to
      one title.
- [x] Reusable gdb script moved to `tools/gdb/live-attach.gdb`. The remaining
      `build/*.sh|*.gdb|*.py` scratch is untracked local data, left alone.

### Structure (was item 2, done in `93afe54` + `1b107bc`)

- [x] Split `CMakeLists.txt` into `cmake/deps.cmake`, `cmake/sources.cmake`,
      `cmake/tests.cmake`, `cmake/install.cmake`.
- [x] Add `vita2native_add_smoke(<name> [SOURCES ...] [LIBS ...] [INCLUDES ...])`
      helper in `cmake/tests.cmake`; it removes 31 boilerplate blocks.
- [x] Add `tests/test.h` with `CHECK` / `REQUIRE`, migrate the 15 hand-rolled
      copies. (`CHECK_EQ` / `CHECK_STR` turned out unnecessary: the message
      form `CHECK(cond, "%d != %d", a, b)` covers them.)
- [x] `tools/v2n` CLI (see below).

### Configuration surface (item 4, medium effort)

- [x] Single `settings.c`: table of `{ VITA2NATIVE_*, kind, description }`
      plus a cached lookup (`5939dc0`). 117 knobs; typo detection with the
      closest match, and `VITA2NATIVE_ENV_DUMP=1` as the generated docs.
      `settings_smoke` scans the sources both ways so the table cannot drift.
- [x] Migrate the call sites to the accessors (`37727d4`). Zero raw
      `getenv("VITA2NATIVE_...")` remain; `settings_smoke` now fails if one
      comes back. Only third-party variables (SDL_*, DISPLAY, PAN_MESA_DEBUG,
      VITA3K_TRACE_UNIFORMS) are still read with `getenv()`.
- [ ] Standardize diagnostics on `LOG` (retire the raw `fprintf(stderr, ...)`
      sites in modules; 139 of them when this was measured).
- [ ] Document the `V2N_*` (kiosk) -> `VITA2NATIVE_*` (emulator) mapping in one
      place; today it only exists inside `xstartup`.
- [ ] Standardize diagnostics on `LOG`/a level macro (retire the 139 raw
      `fprintf(stderr, ...)`); every `*_trace()` helper must cache the getenv
      like the older ones (the new `VITA2NATIVE_TRACE_VIEWPORT` check runs on
      every state application).
- [ ] Document the `V2N_*` (kiosk) -> `VITA2NATIVE_*` (emulator) alias mapping
      in one place; today it only exists inside `xstartup`.
- [ ] Split `SceGxm.c` (10.4k lines) into `gxm_state.c`, `gxm_draw.c`,
      `gxm_texture.c`, `gxm_present.c`, `gxm_diag.c`.
- [x] ~~Add CI: `arm-rel` + `x64-syntax` builds, `ctest` on x64, device-only
      tests skipped.~~ Built (`18e4cb9`) and then removed on request: this
      repository does not run Actions. The portability findings from doing it
      are kept below; re-adding the workflow later is a copy of that commit.

### Skills + CLI (item 3, done in `7dd6abf` + `3bf169b`)

Decision: put the *commands* in one tracked script (`tools/v2n`), and the
*judgment* (checklists, what to look at, pitfalls) in skills that call it.

`tools/v2n` subcommands:

| Command | Replaces |
| --- | --- |
| `deploy [opiz3\|r36s]` | build `arm-rel`, scp, restart kiosk (OPi) / login-shell run (ArkOS) |
| `title <ID>` | back up `v2n.conf`, sed `V2N_APP`, restart, tail log |
| `shot` | `xwd -root` -> PNG on the host |
| `drive '<xdotool expr>'` | press/hold timing rules (a tap shorter than a frame is lost at 4-20 fps) |
| `log [n]` / `crash` | tail the live log / newest preserved crash log |
| `test` | scp the smoke suite to the device and run it |
| `dump-frame <n>` | headless `VITA2NATIVE_DUMP_FRAME` capture + PNG conversion |
| `gdb` | live-crash attach (SIGSEGV, regs, guest bt) |

Skills to author (project scope):

1. `v2n-crash-triage` — `TRACE_ABORT` caller trace, live gdb over cores,
   `set arm force-mode thumb` guest disassembly, stub->NID resolution with
   `VITA2NATIVE_TRACE_IMPORTS`, and the "missing API vs title bug" decision
   table. This is the loop that solved Shakedown, Fort Defense and Iconoclasts.
2. `v2n-render-debug` — frame dumps, orientation/viewport work
   (`TRACE_VIEWPORT`, letterbox clips, `y_scale` sign, readback vs direct
   present), texture/draw tracing, and the pitfalls (`direct=0` on the SDL
   build, `VITA2NATIVE_DUMP_FRAME` only in the headless build).
3. `v2n-run-title` — new-title bring-up: decrypted SELF from the Vita3K cache,
   staging into `~/tests/apps/<ID>`, kiosk switch, first-boot checklist
   (unresolved NIDs, asserts in `PostInitApplication`, aborts, black screen).

### Pitfalls worth remembering (learned this session)

- The deployment build is **HEADLESS=ON**, so presentation goes through
  `SceDisplay_null.c` -> `host_video_sdl.c`, never `SceDisplay_linux.c`, and
  `gxm_direct_present_enabled()` is 0. Frame dumps come from the null display.
- A title that renders into an offscreen target and presents it directly has
  its orientation decided by the **viewport Y scale sign**, not by the readback
  (Iconoclasts: `+180`; Chasm: `-272`).
- `sprintf("%ls")` on a Vita `SceWChar16` needs the UTF-16 formatter in
  `SceLibc.c`; glibc reads 32-bit `wchar_t`.
- Touch-driven engines may use `SceSystemGesture` and ignore `SceTouch` entirely.

### Progress log

- `c3c6550` dead weight: `SceAudio_null.c` deleted, duplicate install rule
  removed, `tools/opiz3-vnc` made title-agnostic, `tools/gdb/live-attach.gdb`
  added. The `viewport_flip[4]` item was stale (see above).
- `7dd6abf` `tools/v2n` + `tools/img2png.py`. Verified on opiz3: `title`
  (switch and switch back), `shot`, `drive click|key|cmd`, `log`, `crash`,
  `test` (25/25 pass), `dump-frame` (2 frames), gdb attach (`info threads`).
  Not exercised live: `gdb` waiting on a real SIGSEGV.
- `3bf169b` three project skills under `.agents/skills/`.
- `93afe54` CMake split into `cmake/{deps,sources,tests,install}.cmake` +
  `vita2native_add_smoke()`. Same executables, same 35 ctest entries.
- `1b107bc` `tests/test.h`: `CHECK` (accumulate) and `REQUIRE` (return 1),
  15 hand-rolled macros removed, 25/25 on the device.
- `18e4cb9` CI: `arm-rel` + `x64-syntax` build matrix, smoke suite on x64 with
  `needs-gl` / `not-64bit` labels. **Removed again** in `ci: drop the workflow`:
  the repo does not run Actions, so the workflow and its labels were dead
  configuration. The 64-bit findings survive as a comment in `cmake/tests.cmake`.
- `d05f47a` `v2n clean tmp|all`, so device cleanup is one command instead of
  three ssh invocations.

### Cleanup pass (same session)

- Local `build/`: 1.6 GB -> 837 MB. Removed the scratch dumped by earlier
  sessions at the top level (395 files: one-off `.sh`/`.gdb`/`.py`, frame
  PNGs/BMPs/XWD stills, dumps, stray test binaries) and the scratch trees
  (`evidence/`, `crossy-*`, `debug_frames/`, `guest/`, `real-homebrew/`,
  `sdl-*`, `syntax/`, `tmp/`, `ctrl-test/`, `fs_root/`, `arm/`, `cmake-a64/`).
  Kept the three preset build trees and the zig wrappers, which the toolchain
  files point at. Everything removed is regenerable: `build/guest` and
  `build/fs_root` come back from `tests/fixtures/build_guest_elf.py` and
  `host_smoke` respectively.
- opiz3: 16 GB rootfs went from 59% to 32% used. `v2n clean cores` deleted ten
  cores (~3 GB), `~/.vnc`/`v2n-run` kept (savedata lives in
  `v2n-run/vita2native/`), crash logs kept (3 MB), `/tmp` 371 MB -> 1 MB.

Observation worth a follow-up: the smoke tests leave their `mkdtemp`
projects (`v2n-appmgr-*`, `v2n-fios-*`, `v2n-sqlite-*`) in `/tmp` on every
run, and two tests had 40 MB trace dirs there. Cleaning them up in the test
epilogue would be tidier than `v2n clean tmp`.

Still open: the configuration surface (`settings.c`) and the `SceGxm.c` split.
Both are larger than the rest of this pass put together and both touch the
render path that the four working titles depend on, so they want a device in
reach and one commit per behaviour change.

### 64-bit test findings (from the CI work)

`x64-syntax`/any 64-bit host cannot run three tests, because they emulate guest
structs by storing *host* pointers in 32-bit fields:

- `dialog_smoke` — `sceSaveDataDialogInit` reads `*(uint32_t*)(p+0x58)` as a
  pointer; truncates to `0xfffffffff750` and faults.
- `texture_smoke` — `gxm_texture_decode_level_rgba4*` takes truncated stack
  pointers (`0xffffffffb9a0`).
- `avplayer_smoke` — faults in `main` after `sceAvPlayerAddSource`.

They are tagged `not-64bit` rather than deleted: the device (32-bit ARM) still
runs them. Fixing them means making those code paths 32-bit-agnostic, which is
only worth doing if a 64-bit *executable* build ever becomes a goal.

To run the 64-bit suite without CI: build a scratch aarch64 tree and copy the
binaries to the Orange Pi (it is aarch64, so it runs them natively):

```sh
# wrappers: zig-{a64}-{cc,cxx,ar,ranlib} calling `zig cc -target aarch64-linux-gnu`
cmake -S . -B build/cmake-a64 -G Ninja \
      -DCMAKE_C_COMPILER=$PWD/build/zig-a64-cc \
      -DCMAKE_CXX_COMPILER=$PWD/build/zig-a64-cxx \
      -DCMAKE_AR=$PWD/build/zig-a64-ar -DCMAKE_RANLIB=$PWD/build/zig-a64-ranlib \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVITA2NATIVE_HEADLESS=ON \
      -DVITA2NATIVE_ALLOW_64BIT=ON -DVITA2NATIVE_ARM_ARCH_FLAGS=
```

`CMAKE_RANLIB` is the part that bites: the host `ranlib` silently rewrites the
ELF archives to 96 empty bytes.

Remaining: CMake split done; config surface (`settings.c`), `SceGxm.c` split.

### Trim pass 2 (docs + dead shims, `1d2fe80`)

Surveyed the tree for anything unreferenced rather than guessing:

- All 92 `source/` translation units are in a CMake target; all 52 headers are
  included somewhere; none of the 754 `static` functions is unreferenced (that
  is also what `-Wunused-function` would have said, since `-Wall` has been on).
- Modules with no `EXPORT()` are helpers (audio backend, ctrl script parser,
  AEABI, NGS renderer, dialog/`fps` overlays) and are referenced by CMake.
- `external/` has nothing to cut: `dlmalloc/malloc.c` is `#include`d by
  `source/mspace.c`, `patches/` is applied at configure time by
  `external/CMakeLists.txt`, and the rest is submodules (Vita3K alone is 526 MB
  but is the shader recompiler).
- Two shims *were* dead (`include/vita3k/microprofile.h`,
  `include/vita3k/util/optional.h`); confirmed with `ninja -t deps` that no TU
  ever included them.

Doc rot found and fixed: `-DVITA2NATIVE_SOFTFP=ON` (option does not exist),
the "Remote testing over X11" section still used the `/tmp/v2n-*` layout, and
AGENTS.md still pinned Chasm as the configured title.

Left alone on purpose: `TODO.md` (a real feature backlog, not a status page),
`tests/fixtures/build_guest_elf.py` (132 KB because it writes the ELF fixtures
by hand), `.github/FUNDING.yml` (predates this work).

#### Settings, part 1 (`5939dc0`) — what was learned

- The two-direction source scan paid for itself immediately: it found
  `VITA2NATIVE_SKIP_READBACK` missing from the table (117 knobs, not 116).
- A cached `getenv()` value must be **copied**: the returned pointer is into the
  environment block and `setenv()` may move it. The smoke test asserts the cache
  survives a `setenv()`, which is how the bug surfaced.
- Flags keep presence semantics (`getenv(...) != NULL`), including `"0"` being
  set, because that is what all 100+ existing sites do. Only `""` counts as
  unset. Documented in `settings.h` so the migration does not subtly change
  behaviour.
- `VITA2NATIVE_ENV_DUMP=1` exits after printing (118 lines), so it doubles as a
  CLI for regenerating docs.

##### Settings migration (`37727d4`) — notes

- 96 sites across 33 files plus the 84 in `SceGxm.c`; `grep getenv(` in
  `source/` + `include/` now only finds third-party variables. Flags are
  `vita2native_env_flag`, values `env_str`, numbers `env_int`/`env_double`;
  sites with a custom parse (hex colour, program hash, frame list) keep
  `env_str` and parse the cached string, so their behaviour is byte-identical.
- `include/settings.h` needed `extern "C"`: `gxm_texture_pvrtc.cpp` reads
  `VITA2NATIVE_NO_PVRTC_FAST`, and without it the C++ TU calls a mangled
  symbol. The link error only showed up in the ARM build.
- `vita2native_add_smoke()` now appends `source/settings.c` to every test
  target. Modules that tests compile (SceGxm, SceLibc, SceFios2, host_*) read
  knobs, so per-target listings would have been 30 edits.
- `settings_smoke` now asserts **zero** raw `getenv("VITA2NATIVE_...")` sites
  rather than ">100 seen", which turns it into a regression guard.
- Post-migration counts: 141 `fprintf(stderr, ...)` in `source/` vs 411 `LOG()`.
  That is the next item, and it is mostly mechanical: the raw ones are the
  early-boot paths (main.c, load.c) where `LOG` may not be initialised yet, so
  the split needs care rather than sed.

#### Regression round after the settings migration (2026-11-11)

Build `37727d4` + `d05f47a` lineage, deployed to opiz3 (kiosk, `V2N_RENDER_SCALE=1`).
Each title: switch, 12-15 s, screenshot, then scan the log.

| Title | ID | Screenshot | fps | unresolved NIDs | signals | unknown settings |
| --- | --- | --- | --- | --- | --- | --- |
| Chasm | PCSE01031 | title + main menu | 3.4 | 39 (NpTus/Ulobj/Razor) | 0 | 0 |
| Shakedown: Hawaii | PCSE01125 | autosave overlay | 8.5 | 28 | 0 | 0 |
| Fort Defense | PCSE00321 | title screen | 4.5 | 179 | 0 | 0 |
| Angry Birds Trilogy | PCSE00322 | copyright splash (boot only) | 6.6 | 19 | 0 | 0 |
| Iconoclasts | PCSE01177 | intro (restored) | 18 | 25 | 0 | 0 |

- Input: Cross on Chasm's "PRESS ANY BUTTON" advanced to the menu, which
  exercises SceCtrl + host_video_sdl after their migration.
- Touch: Fort Defense polls `SceTouch` and the Play tap still does not register
  — the documented `SceSystemGesture` gap, unchanged by this work.
- Audio: manual run with `VITA2NATIVE_AUDIO_DEBUG=1` shows peak ~5-6k, samples
  played climbing, no underruns.
- `tools/v2n test`: 26/26. No new `crash-*.log`. fps matches the recorded
  baselines for Chasm (3.5) and Fort Defense (~5).
- Screenshots: `build/shots/regress-*.png` (local, gitignored).

### v2n-extract (new tool, 2026-11-11)

The user supplied `~/Downloads/<hash>.pkg` + `work.bin`:
`UP0002-PCSE00294_00-ANGRYSTRWARS13VT` (Angry Birds Star Wars, 243 MB).

What the file taught us:

- It is a **PFS-style package**: `sce_pfs/{files.db,unicv.db,pflist}` are in the
  TOC and the payload files are PFS-encrypted. pkg2zip (built here for ground
  truth) extracts them as garbage — `eboot.bin` starts `54 cb e4 7c`, not
  `SCE\0` — and reports success anyway. Any extractor that stops at the pkg
  layer silently produces an unusable tree.
- Layers and keys: pkg container = three public constants, no license; PFS =
  klicensee from `work.bin` (zRIF); SELF = same klicensee plus keys compiled
  into `sce_utils.cpp`.
- Two offsets that look alike are not: the info block's `sfo` offset is
  **absolute**, the TOC offsets are **relative to the data area**.
- `rif2zrif()` sizes the license from `tellg()`, so the stream must be opened
  with `std::ios::ate` (Vita3K does this).

Tool: `tools/v2n-extract/` — C pkg reader (public-domain AES layout follow
pkg2zip), psvpfstools `execute()` for PFS, Vita3K `decrypt_fself()` for SELF,
with `compat/util/log.h` replacing spdlog so only that TU is linked. Host build
via `tools/v2n-extract/build.sh` (OpenSSL 3 + zlib).

Verified end to end on opiz3: `PCSE00294` boots to the Angry Birds Star Wars
title screen at ~5.8 fps with 37 unresolved NIDs and no signals — a title the
emulator has never run before, with no emulator changes.

### "Frozen" Angry Birds Star Wars — it was leaked processes (2026-11-11)

Symptom: title screen, log line count static (1524 -> 1524 over 4 s), input
arriving (`SceTouch down`) but nothing reacting, ~113 %CPU.

Cause: **three `v2n-arm` instances were running at once** (172601, 174293,
180361), from three session restarts. 3 x ~100 %CPU on four cores starved the
one that owned the window. `tools/v2n shot` showed a correct title screen the
whole time; only the CPU and log-rate check exposed it.

Why the old ones survived:

- TigerVNC runs `xstartup` inside a **logind PAM session scope**
  (`session-cNNN.scope`), which is not part of the unit's control group, so
  `KillMode=control-group` cannot reach the game. Ubuntu ships
  `KillUserProcesses=no`, so the scope is not torn down either.
- `xstartup`'s trap sent `SIGTERM`, and **SDL installs a SIGTERM handler that
  only pushes an `SDL_QUIT` event**. A guest busy rendering and not polling
  events never reads it: `kill -TERM` is a no-op for exactly the instance that
  needs killing. Verified: `pkill -x v2n-arm` reaped two healthy instances and
  left the wedged one; only `kill -9` worked.

Fix in `tools/opiz3-vnc/xstartup` (three guards, all verified on opiz3):

1. sweep: `pgrep -x v2n-arm` -> TERM, 1 s, `pkill -KILL -x v2n-arm` before
   starting the new instance;
2. trap escalates TERM -> 3 s grace -> KILL;
3. watchdog subshell polls `xdpyinfo` every 2 s (2 consecutive misses) and runs
   the same stop, because `systemctl stop` signals Xtigervnc, never this shell.

Verified lifecycle: start -> 1 instance; restart -> 1 instance; stop -> 0
instances (was 1 -> 2 -> 3 before the fix).

Non-issue while investigating: an `SceDisplay:` log line is `8 frames in 1.1 s
(7.4 fps)`, so `grep -c SceDisplay` per second is ~0.9, not the fps. The title
runs at its usual 7.4 fps; do not read the line count as a frame rate.
Play tap works and the intro crawl plays.

### Angry Birds Star Wars: the real freeze is a guest infinite loop (2026-11-11)

After the leaked-process cleanup the title still wedges, so that was a separate
bug. Reproduced twice, both times right after a Play press:

- `g21:<main>` burns exactly one core (+5 CPU-s per 5 s) while every other
  thread, including all four `llvmpipe-*`, is asleep at 0 growth, and the log
  stops dead. That triple (core busy + nobody else + no log) is the signature.
- PC sits in one 28-byte loop, ARM mode (the guest is ARM here, *not* Thumb):

      812aeab0  ldr  r12, [r4, #4]      ; r12 = next block, offset from base
      812aeab4  cmp  r12, r5            ; r5 = 0xffffffff ("not found")
      812aeab8  mov  r6, r4
      812aeabc  beq  812aead4           ; found -> free/coalesce path
      812aeac0  add  r4, r3, r12        ; r4 = base + offset
      812aeac4  cmp  r4, r1             ; r1 = expected end pointer
      812aeac8  ldr  r12, [r4, #4]
      812aeacc  bne  812aeab4

- r3 (base) = `0xb9c00000`, the game's own 95 MB `sceKernelAllocMemBlock`
  (`name "]"`, size `0x5f00000`), so this is the *game's* allocator, not ours.
- r1 (the end pointer it walks towards) = `0xbd82ab2c` in **both** wedges, and
  the chain cycles between `0xb9c00000` and `0xb9f40004` instead of ever
  reaching it -> infinite loop on a cyclic block chain.
- eboot module base is `0x81000000`, so the loop is `eboot+0x2AEAB4`.
- Code after the loop (`0x812aead4`: read header word, test bit 2, `ldrb
  r3,[r0,-1]; sub r0,r0,r3` to step to the block start) is a free/coalesce
  path, so the game is in its memory manager during what is almost certainly
  the level-load path.

Ruled out, with evidence:

- leaked instances (the earlier "freeze") - fixed in xstartup, see above;
- save-data contamination - a *fresh empty* `savedata/` still wedges;
- "it is just a static title screen" - no: log rate, per-thread CPU and the
  frame md5 all stay frozen; the healthy title screen differs in all three;
- input delivery - `mouse` + `SceTouch down/up` reach the guest right before
  the spin, and the same tap advances the game fine in other runs;
- missing assets - the last things the guest does are successful
  `sceIoLseek`/`sceIoRead` of `app0:data/audio/stereo44100/music/
  black_main_title_final.at9`; no `sceIoOpen ... failed` near the wedge.

Not yet explained: the wedge is **intermittent** (~2 of 6 Play presses).
Between wedges the same build plays the intro crawl and loops title music for
15+ minutes on the same boot.

Also characterised, and a separate (harmless) trap while debugging: the game
blocks on system dialogs (`MsgDialog`/`SaveDataDialog`) which the emulator
answers on its own after ~1 s (`dialog_timed_out`); while one is up the title
screen ignores input but the log keeps growing and CPU stays around 50 %, so
do not confuse it with the spin. The PSN-leaderboard prompt sits in that state
for minutes on this title.

#### Third reproduction (same day, after a clean kiosk restart)

Not a crash: PID alive, `vnc-game.log` frozen at 674 lines, `g21:<main>` at
100 % of one core, all other threads idle, no `crash-*.log` written. Same loop,
same registers as the first two: `pc 0x812aeabc`, `r3=0xb9c00000`,
`r4=0xb9f40004`, `r12=0x0898003c`, `r1=0xbd82ab2c`, `r5=0xffffffff`.

New detail: `r0 = 0xc0109200` is the allocator context, and it reads
`{ 0x3, 0x05f00000, "GPU_", "Heap", ... , base 0xb9c00000 at +0x38 }` — so this
is the game's **GPU heap** allocator (95 MB block), not guest RAM.

The chain it walks (`[node+4]` is a 32-bit offset from `r3`, and the only
successful exit is a node whose offset is `0xffffffff`):

    base 0xb9c00000  --0x00340004-->  0xb9f40004
    0xb9f40004       --0x0898003c-->  0xc258003c   (zeroed memory, not a node)
    0xc258003c       --0x00000000-->  base  -> cycle

So one link points at memory that is no longer a block header, the `0xffffffff`
terminator is never reached, and the walk spins forever instead of reporting
"not found". `r1 = 0xbd82ab2c` (the block it is looking for) was identical in
all three wedges, i.e. the state is deterministic, not random scribble.

Next experiment if this is worth chasing: hardware watchpoint on `0xb9c00004`
and on the `0xb9f40004+4` link, then reproduce; gdb's stop stack tells us
whether the bad link is written by guest code (allocator bug) or by an
emulator/GL write landing out of bounds (our bug, and the likely reason the
R36S survives the same path).

### Angry Birds Star Wars: the corrupt link is written by the guest, and the block gets dirty heap memory (2026-11-12)

PLAN.md's own next experiment was "hardware watchpoint on `0xb9c00004` and on
the `0xb9f40004+4` link". Done. The result overturns the standing hypothesis.

**Who writes the bad link.** The free list is walked from the allocator context
at `0xc0109200` (`+0x34` head offset, `+0x38` base). Watching the node that ends
up corrupt (`base+0x4355590`) from early boot, the watchpoint fires on **guest
code**, in the game's own free/split path:

    0x812aeb3c  str r2, [lr, #4]     ; remainder->next = splitblock->next
    r0 = r1 = 0xbdf2aafc  (block being split, header 0x002aa90d = size 0x2aa90c)
    r3 = 0x2aa94          (requested size)
    r2 = 0xff000000       (the value that becomes the bad link)

`lr = r0 + r3 = 0xbdf55590`, so the remainder node is exactly the corrupt one.
`0xff000000` is not a valid offset: `base + 0xff000000` wraps to `base -
0x1000000`, below the block, so the walk leaves the block and never sees the
`0xffffffff` terminator.

**The emulator's GL path is innocent.** Reproduced with the null GPU backend
(armhf libs unloaded, so EGL and SDL fail to load, nothing is rasterised): same
fault, `pc=0x812af1bc`, `fault=0xa9100000`. Note `base=0xaa100000` there, and
`0xaa100000 + 0xff000000 = 0xa9100000` - the fault address is the same corrupt
link, with no graphics involved. The watchpoint hit lands on a guest PC, so this
is not "an emulator/GL write landing out of bounds".

**The list is healthy before it breaks**, which kills the "state was never
valid" reading. Sampling every 500 ms from process start:

    9.5s   0003e894 -> 00360018 -> 0068000c -> 03c2ab2c -> ffffffff   VALID
    42.6s  0003e894 -> 00360018 -> 0087ffa8 -> 03c2ab2c -> 04355590 -> ff000000  BROKEN

The title was still rendering at 42.6 s, so the corruption precedes the spin.
This also explains the intermittency: whether the bad link lands on unmapped
memory (SIGSEGV at `0x812aeac8`) or on mapped zeroes (silent spin) depends only
on where the block landed that boot.

**Correction: the dirty-memory theory does not hold.** An earlier draft of this
note blamed the block landing outside `VITA_MEMBLOCK_LIMIT` (`SceSysmem.c:31`)
and therefore falling back to `posix_memalign` on dirty `malloc` memory. Two
recorded crashes have the block fully inside the mmap range and therefore in
fresh zeroed `MAP_ANONYMOUS` memory, and they crash identically:

    crash-20261112-030203  base=0x8a100000  (end 0x90000000, inside limit)
    crash-20261112-024344  base=0xa8000000  (end 0xadf00000, inside limit)

So the block contents are not the trigger. Checked directly: the 1024x1024
texture logged at `data=0xba280010` spans `0xba280010-0xba680010`, which does
not overlap the split block at `0xbdf2aafc`; the `0x15df15df` payload there is
unrelated data that merely looks like pixels. The allocator treats that region
as a free block and writes the remainder header `0x2aa94` further on, and it
does so at the same relative offsets in every boot - `0x340004`, `0x898003c`,
`0x8650864` - even when the block sits in fresh zeroed memory, which is what
rules out "leftover garbage" as the cause.

Next: find out why block `0xbdf2aafc` is on the free list at all when its
payload is live texture data. Prime suspect is a double free of the same
regions in the game's allocator; log every free of the block whose header is
`0x002aa90d` and see whether it is freed twice.

#### The range fix does not help (same day)

Raised `VITA_MEMBLOCK_LIMIT` from `0xb0000000` to `0xc0000000` (it contradicted
the comment two lines above it, which says user memory ends at `0xBFFFFFFF`)
and added a wrap-around to `alloc_vita_range`, because the shared bump pointer
runs off the end of the range once a title allocates enough blocks and every
later block then falls back to `posix_memalign`, which places guest memory
outside the range entirely (`0x65a00000`, `0x4cd20000`, `0x69dc0000` were all
observed).

Effect: real. Every block now lands inside `0x81000000-0xC0000000`:

    before: 0xba100000? no -  0xb9c00000 / 0xa8000000 / 0x8a100000, plus
            VertexMem 0x67a00000, UsseMem 0x69dc0000, VdmMem 0x4cd20000
    after : 95MB block 0xba100000 (end 0xc0000000, inside), and every other
            block in 0x8a600000-0x8c780000

Outcome: **the crash is unchanged.** Same guest pc `0x812aeac8`, same relative
link offsets, only the fault address moved because the base moved:

    crash-20261112-094744  base=0xba100000  fault=0xc2750868

Compare the four base values on record - `0x8a100000`, `0xa8000000`,
`0xb9c00000`, `0xba100000` - all inside the range, all crash identically. So
block placement is not the cause and the limit change is not a fix. It was
reverted rather than kept, to avoid carrying an unrelated behaviour change
(where guest blocks land) through a bug hunt. It is worth re-landing on its own
merits, since placing guest memory outside the range the titles validate
against is wrong regardless of this crash.

What this narrows to: the corruption is position-independent and comes from
guest code in the title's own allocator (watchpoint evidence above). The block
at `0xbdf2aafc` is on the free list while holding non-header payload
(`0x15df15df`), and splitting it writes a remainder node whose "next" is
`0xff000000` - below the block, so the walk never terminates. Next step is to
log every free of that block inside the title's allocator and see whether it is
released twice, which would make the split operate on live texture memory.

## 2026-11-14 — Shakedown: Hawaii on the R36S with direct present

- Build `0.4.0-dc30730-dirty`; the deployed `/home/ark/v2n-run/v2n-arm` md5
  matches `build/cmake-arm-rel/vita2native` (no rebuild needed).
- The title lives directly in `/home/ark/v2n-run/PCSE01125` — this device has no
  `tests/apps` tree, so there is nothing for `tools/v2n title` to list.
- Recipe (`/home/ark/v2n-run/run-shake.sh`, started from a login shell with
  EmulationStation stopped): `SDL_VIDEODRIVER=kmsdrm
  VITA2NATIVE_KMS_PRESENT=1 VITA2NATIVE_FPS_OVERLAY=1`.
- Result: 30.0 fps (the title's own cap), 0 guest signals, 0 unknown settings,
  no `GL error`, no rejected draws. 46 unresolved NIDs on boot (28 distinct, the
  recorded Np*/Ulobj set).
- Direct present verified on the panel, not just in the log:
  `VITA2NATIVE_SCANOUT_VERIFY` + `VITA2NATIVE_SCANOUT_DUMP` read the GBM buffer
  through its DMA-BUF fd — 846-906 of 1200 sampled pixels lit on every flip,
  `black_scans=0`, alpha 255, and the dumped PPM shows the game plus the FPS
  graph. The intermittent black frame seen in the 2026-11-13 session did not
  recur in this run.
- Boot sequence: autosave warning for ~10 s (the save lands in
  `vita2native/savedata/SAVDATA0000.bin`), then the title screen at
  "Press x Button".
- Access note: the device is not routable from outside the LAN; `~/.ssh/config`
  now carries `ProxyJump root@pve` under `Host r36s`.

## 2026-11-14 (later) — Shakedown: Hawaii goes black under direct present

Symptom: with `VITA2NATIVE_KMS_PRESENT=1` the boot screens (autosave warning,
legal, title "Press x Button") render, then the panel goes black - reliably once
the weapon menu is up. The FPS graph keeps updating, so the presenter is alive.
Frame rate rises from 30 to 60 when it goes black.

Measured, on the WIP build unless noted:

- `PRESENT_SOURCE_PROBE` reads the display render target at EndScene:
  `lit=441/576` while the title screen is up, `lit=0/576` from then on. The
  render target really is empty, not just mis-presented.
- The guest keeps uploading its composite source: a 512x320 texture at
  `0x8ae00000`, every frame, with a **different content hash each time**.
  `VITA2NATIVE_DUMP_TEXTURES` shows that texture holding the weapon menu -
  guest-side content is correct.
- `DEBUG_PROGRAM_COLORS` paints the whole panel one flat colour during boot, so
  the full-screen composite quad does reach the display target then.
- Profile in the black state: `2 gl draws, 12 vertices, 0 uploads` per frame at
  60 fps. Scenes keep arriving (`scenes` advances with `presents`).

Ruled out, with a test each:

- **Not the WIP present-texture copy.** Built HEAD (`a1c5b54`) in a clean
  configure and ran it: identical black screen at the weapon menu, 60 fps.
- **Not the skipped colour writeback.** Made the direct-present branch fall
  through to the readback as well (so guest memory is current every frame):
  still black, and a clean run then went black at boot instead of at the menu.
  Reverted.
- **Not the skipped drain.** `VITA2NATIVE_NO_PRESENT_OVERLAP=1` on HEAD (forces
  the EndScene `glFinish`) dies in a guest signal 11 during boot.
- Present/CRTC plumbing is fine when it works: `SCANOUT_VERIFY` read the GBM
  buffer through its DMA-BUF fd and showed 850-900/1200 sampled pixels lit,
  `black_scans=0`, alpha 255, with `crtc verify` reporting
  `scanning == flipped` on every flip.

Current picture of the failure: the title presents a *static* frame - it stops
producing new content for the display target but keeps presenting - and
direct present has no guest-surface copy to fall back on, while the readback
presenter keeps presenting the last frame it wrote into guest memory.

Workaround in place: the readback presenter
(`SDL_VIDEODRIVER=kmsdrm VITA2NATIVE_FPS_OVERLAY=1`, no `KMS_PRESENT`) renders
the title correctly at 28-30 fps, including the weapon menu.

Separate finding: this title has an **intermittent boot crash** in both
presenters. `guest signal 11`, with the faulting call a host `memcpy` of
`r0=0`, `r1=0x2f5e1`, length 11 (destination NULL), and the crash handler then
faults itself in `guest_signal_handler` so only a partial dump is produced. It
is not deterministic: the same binary boots on the next attempt.

Access note: `~/.ssh/config` now carries `ProxyJump root@pve` under `Host r36s`.

### Root cause of the black screen (fixed, `224d8c5`)

The KMS presenter blits from the guest's own GL context and unbinds its own
texture on unit 0 afterwards. `g_bound_texture[]` still recorded the game's
texture, so the next bind was a no-op and the draw sampled an unbound sampler -
GL returns black. A title that rotates textures every frame recovers by itself,
which is why the boot screens were fine; Shakedown: Hawaii samples one texture
once the picture settles, so it stayed black.

Evidence: `PROBE_TEXDUMP` in the black state showed the sampled descriptor with
no GL name (`gl=0`) while the unit's memo said `bound=6`, and the same scene's
guest side was correct (`DUMP_TEXTURES` had the menu in the 512x320 upload).

Fix: invalidate the unit memo in `SceGxm_get_present_frame` when direct KMS
presentation is in use. Verified on the R36S: weapon menu and gameplay stay up.

### Why 30 fps (measured, not a cap)

`sceDisplayWaitVblankStart` is called once per frame against the 60 Hz grid, and
the title does ~20.7 ms of work in the intro (guest 9.2 + GXM draw 6.9 + present
4.7), so every frame misses the next boundary and lands on the one after: two
periods = 33.3 ms = exactly 30.0 fps. Cutting the work moves it, so it is a
deadline miss:
`RENDER_SCALE=1` 30.0 fps, `0.6667` 58-60, `0.5` 59-60, `0.25` 58-60.
The FPS overlay itself costs a few ms (0.6667 reads 34-38 with it on and 58-60
with it off), so benchmark with it disabled.

## Chasm: sampling a colour render target (colour alias)

Chasm renders into intermediate targets (384x216 and 960x544) and composites by
creating a texture over the same memory. That texture went through guest memory:
a full readback plus a decode and re-upload per scene. On the R36S that was
`read 18.6 + convert 2.6 + tex 9.5 ms` per scene, about four scenes per presented
frame - the whole 6.8 fps. Under direct presentation nothing is written back at
all, so the sample returned stale black and the panel stayed black with only the
FPS overlay drawn.

Fix: when a texture is created over a colour surface this emulator rendered
into, bind the render target's own GL texture (`gxm_color_alias_find`), the same
idea as the existing depth alias. Guarded by:
- the target must not be the one currently being drawn into (feedback loop);
- no gamma decode and no mips, where the cached decode path is still needed;
- width/height must match the surface bound at `sceGxmBeginScene`.

The render target now records that surface (`color_data`/`color_width`/
`color_height`), since the params do not carry it.

Measured, Chasm on the R36S, direct present + 0.1x-1x scale unset:
- readback presenter, correct: 6.8 fps
- direct present + forced writeback, correct: 6.7 fps
- direct present + colour alias, correct: 18-27 fps depending on scene
- `VITA2NATIVE_NO_COLOR_ALIAS=1` restores the old path

Shakedown: Hawaii (which samples its own 512x320 target) unchanged at 59 fps,
no signals; its earlier black screen was the texture-unit memo, not this.

## 2026-11-14 — input is SDL only

The headless controller backend (`SceCtrl_null.c`) opened `/dev/input/event*`
once at `SceCtrl_init` and let evdev own the buttons. Three problems showed up
on the R36S in one session:

- a pad plugged in later is never opened (no rescan; the poll thread only
  watches the fds it started with);
- Xbox-style triggers arrive as `ABS_Z`/`ABS_RZ`, which nothing mapped, and
  SDL's button fallback was disabled whenever evdev found a pad;
- "is this a pad" was a case-sensitive substring test on the device name.

The decisive case: an emulated pad (`R36S Remote Gamepad`, presenting as
"Xbox 360 Controller" to SDL) sat next to the built-in `GO-Super Gamepad`.
Only controller 0 was ever opened, so it produced no input at all -
`SDL sample buttons=0x0` with `pads=1`.

Input is now SDL only: `host_video_sdl.c` opens **every** pad SDL reports,
rescans once a second (so hot-plug and unplug both work), unions their buttons
and takes the sticks from whichever pad is in use. Pads with no database entry
keep the positional raw-joystick fallback.

Note for the R36S: SDL's built-in mapping for `GO-Super Gamepad` has no
`back`/`start`/`leftstick`/`rightstick`, so Select/Start/L3/R3 were dead.
ArkOS ships a complete entry for the same GUID in several of its emulators;
point SDL at one:

```sh
SDL_GAMECONTROLLERCONFIG_FILE=/opt/inttools/gamecontrollerdb.txt
```

`/opt/{hypseus-singe,inttools,linapple,openmsx,mvem}/gamecontrollerdb.txt` and
duckstation's copy have `back:b12,start:b13,leftstick:b14,rightstick:b15`; the
ppsspp and scummvm copies additionally bind `guide:b16` and `rightstick:b17`.

`VITA2NATIVE_INPUT_DEBUG=1` now also prints `SDL sample buttons=... pads=N`
from the sampling thread, which is what showed the two-pad problem.

## Iconoclasts clamps its timestep

`VITA2NATIVE_TIME_TRACE=1` shows Iconoclasts reading the process clock 5 times per
frame (Shakedown and Chasm: 2), so it is not ignoring time. But with the clock
stretched 2x (`VITA2NATIVE_TIME_SCALE=2`) the motion is unchanged, which means it
steps a fixed 1/60 s per frame whatever delta it measures: min(dt, 1/60), or a
one-step-per-frame accumulator.

Consequence: at 30 fps the game runs at half of real speed - slow motion, not
skipped frames - and no emulator-side clock trick can fix that, because the cap
is in the title. Only 60 guest frames/s does. On the R36S that is
`VITA2NATIVE_RENDER_SCALE=0.8` (512x294, 58-60 fps); 0.9 drops to 52-57 and 1.0
to 42 in the intro and 30 in game.

Vita3K shows the same behaviour whenever it runs below 60; it simply does not on
a PC.

This is also why "frame skipping" is not an option here: the guest owns the draw
calls, so there is no logic-only frame to run ahead - each guest frame costs the
same work whether it is presented or not.

## Shader-side blending (step 1 landed, merging still to do)

Iconoclasts alternates two blend states per sprite — `ADD` with
`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`, and blending off — so the batcher breaks on
every draw (`breaks 2/0/0/100`, all `blend 1`). That leaves 103 GL draws/frame at
~0.15 ms each (draw 16.5 ms: flush 9.1 [commit 4.4, gldraw 4.3]), which is why
the title sits at 30 fps in game whatever the render scale: it is CPU-bound on
draw submission, not GPU fill.

`VITA2NATIVE_SHADER_BLEND=1` now applies the guest's blend equation in the
fragment shader instead of GL state: `spirv_glsl_add_shader_blend()` appends
`out_color = v2n_blend(out_color);` after the guest's epilogue, reads the
destination from `gl_LastFragColorARM`, and covers all 12 factors and 6 functions
with separate colour/alpha paths. `SceGxm.c` uploads the guest's factors and
functions per draw and disables GL blending for programs that took the rewrite.

Verified: the same frame dumped with the flag off and on is byte-identical
(md5 f2cb9eb8…, Iconoclasts' logo screen with alpha blending).

NOT yet a speed win: the factors are plain uniforms, so a merged draw would
apply one blend to every vertex and batching still splits on blend. To merge,
the parameters have to ride in the per-draw channel the vertex shader already has
(`v2nDrawIndex`) — flat varying into the fragment stage, params in a std140 block
indexed by it — and `blend_info` has to leave the batch-state comparison. A
first cut of that was written and reverted: the vertex and fragment stages are
translated independently, so the varying has to be gated on the *pair* (the
vertex program's `draw_uniform_location >= 0`), otherwise a fragment that reads
it can be linked with a vertex shader that does not write it.

**Considered and rejected (2026-11-16):** scaling only the render targets that
are at least as wide as the panel, so small UI targets stay native (Chasm's
384x216 HUD is unreadable at 0.6667 because it becomes 256x144). Rejected as a
hack: it trades image quality per target for frame rate instead of removing
work, and the right answer for a title that is too slow is to make the frame
cheaper, not to draw part of it smaller. Do not re-propose it.

**Status (2026-11-16):** none of this is in the tree. The merge attempt was
discarded, and the shader-blend pass it built on
(`spirv_glsl_add_shader_blend`, commit `f20e51e`) went with it: it delivered no
speed on its own and its API had to change for the merge. The notes above are
the design to resume from, not a description of the code.
