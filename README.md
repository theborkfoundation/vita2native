# vita2native

A PlayStation Vita to Linux ARM32 translation layer (**_not_** an emulator).

This is a fork of [vita2hos](https://github.com/xerpi/vita2hos) that changes the
host platform from Horizon OS to a typical 32-bit ARM Linux system. It runs
PlayStation Vita executables (ARMv7) natively by redirecting their
[module](https://wiki.henkaku.xyz/vita/Modules) imports to host routines backed
by POSIX, SDL2, and GLES 3.x.

There is no CPU emulation: Vita ARMv7 code executes directly on the host CPU.
That is why a 32-bit ARMv7-A userland is required.

## Requirements

- 32-bit ARMv7-A userland with NEON and hard-float (`armhf`), e.g. a Raspberry
  Pi 4/5 with a 32-bit OS.
- CMake, Ninja, pkg-config, and SDL2 development files (SDL2 is not needed in
  headless mode), for example on Debian/Raspberry Pi OS:
  `sudo apt install cmake ninja-build pkg-config libsdl2-dev`.
- A Vita executable built with the default hard-float VitaSDK toolchain.
- Optional at runtime: EGL and GLESv2 (Mesa or vendor GLES 3.x driver). Both are
  loaded with `dlopen`, so the build does not link against them.

## Building

The supported configurations are CMake presets that cross-compile from a 64-bit
host with [Zig](https://ziglang.org): it bundles clang, LLD and libc, so no
system cross toolchain is needed.

| Preset | Target | Use |
| --- | --- | --- |
| `arm-rel` | 32-bit ARM, RelWithDebInfo | the deployment artifact |
| `arm-debug` | 32-bit ARM, Debug | symbols for `gdb` on the device |
| `arm-minsize` | 32-bit ARM, MinSizeRel + LTO | size experiments only; crash backtraces lose symbol names |
| `x64-syntax` | 64-bit x86 | compile check and smoke tests; cannot execute Vita code |

Generate the compiler wrappers once. They are written to `build/`, which is not
tracked:

```sh
 tools/zig-toolchain.sh arm
 tools/zig-toolchain.sh x64   # only needed for x64-syntax
```

Then configure and build the preset you want:

```sh
cmake --preset arm-rel
cmake --build --preset arm-rel
```

On a 32-bit ARM host the build also works without a cross toolchain:

```sh
cmake -S . -B build/native-arm -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DVITA2NATIVE_HEADLESS=ON
cmake --build build/native-arm
```

Guest exports that pass or return floats use the host hard-float ABI, matching
the default VitaSDK toolchain (`-mfloat-abi=hard`). For homebrew built with the
old base-AAPCS/softfp profile, configure with `-DVITA2NATIVE_SOFTFP=ON`.

## Running

Run with the default layout (`vita2native/executable` relative to the current
working directory), or pass a path explicitly:

```sh
./build/cmake-arm-rel/vita2native ./vita2native/executable
```

A VPK can be passed directly. `eboot.bin` is loaded from memory and the
remaining entries are extracted to `vita2native/app0/` (skipped when the archive
size and mtime are unchanged), so `app0:` reads work:

```sh
./build/cmake-arm-rel/vita2native ./MyHomebrew.vpk
```

Vita device paths are remapped below `vita2native/`:

| Vita path | Linux path |
| --- | --- |
| `ux0:/...` | `vita2native/ux0/...` |
| `app0:/...` | directory containing the executable, or `vita2native/app0/` for a VPK |
| `savedata0:/...` | `vita2native/savedata/...` |
| `os0:`, `vs0:` | `vita2native/system/...` |

On KMS/DRM targets set `SDL_VIDEODRIVER=kmsdrm`; presentation then attaches EGL
directly to the scanout plane, with no extra flag. On X11/Wayland the SDL
renderer presents the frame instead.

`SceNetCtl` reports the console offline by default so titles take their offline
fallback. Set `VITA2NATIVE_NETWORK=1` to report the host's real network state.
Guest `mspace_*` imports automatically prefer the title's own `libc.suprx` when
it exports them, falling back to the host implementation otherwise.

## Current status

CPU-rendered homebrew runs: ELF loading, relocations, import patching, threads,
TLS, file I/O, VPK assets, and process lifetime are implemented and tested.

A GLES 3.x GPU device (EGL surfaceless + pbuffer) is created at startup when the
libraries are available; otherwise a null backend is used. GXM is translated to
GLES 3.x: GXP shaders through Vita3K's recompiler, render targets, textures,
depth/stencil, MSAA and the transfer calls. `SceAudioOut`/`SceAudioIn`,
`SceNgs` (ATRAC9 voices, the PCM/HEVAG player and the master buss mixer),
MP3/ATRAC9/AAC decoding, JPEG encode/decode, `SceNet`/`SceHttp`/`SceSsl`,
`SceSqlite`, `ScePgf`, `SceTouch`, `SceCommonDialog` (with an on-screen
overlay), a synthetic camera and `SceAvPlayer` (a static black 30 fps I420
frame and silent audio) and the kernel/thread surface are implemented. AAC uses
the bundled FAAD2 decoder; CELP decoding and encrypted SELF files are not
supported.

Guest code runs natively in the host process; there is no sandbox or isolation.

### Tested homebrew

VitaShell, PixelChess, Pong, VitaSudoku, Minesweeper, ElevenMPV and the VitaSDK
AvPlayer sample load with **zero unresolved imports**. VitaShell, PixelChess,
Pong, Minesweeper and VitaSudoku have been rendered end-to-end under QEMU with
Mesa softpipe; the other titles reach their main loop and start GXM work.
VitaSudoku expects `ux0:/data/VitaSudoku` to exist (its Lua script does not
handle a missing directory), so create that directory before testing it.

### Real ARM32 hardware

The ARM32 build runs natively on a 32-bit ARM Linux userland (armhf on an
aarch64 kernel works too) with no emulation. Verified on an Orange Pi Zero 3
(Allwinner H618, Cortex-A53, Mali-G31 via Panfrost, Ubuntu 24.04 armhf
multiarch):

- all GXM, texture, EGL and non-graphics smoke tests pass unchanged;
- VitaShell renders its home screen with real device capacities;
- the backend reports `GLES 3.1 Mesa (Mali-G31, Panfrost)`.

`PAN_MESA_DEBUG=linear` is set by default when the caller has not set it:
Panfrost's tiled/AFBC layouts make `glReadPixels` about twice as slow, and the
port reads the whole frame back every frame for `sceDisplaySetFrameBuf`.

Performance on that board: VitaShell runs at ~4.8 fps. Consecutive GXM draws
that share program, textures and render state are merged into one GL draw
(847 guest draws become ~119 GL draws per frame); triangle strips are expanded
to triangle lists when merging, because concatenating two strips would add
bridging triangles. Draws with different vertex uniforms merge as well: each
one appends its uniform container to a flat uniform buffer and the shader
indexes it through a per-vertex draw slot. `VITA2NATIVE_PROFILE=1` prints the
GXM breakdown, `VITA2NATIVE_PROFILE_FRAME=1` prints the whole-frame split
(present, vblank, GXM and guest time), `VITA2NATIVE_FPS=1` prints the frame
rate, `VITA2NATIVE_TRACE_TEXTURES=1` logs texture uploads,
`VITA2NATIVE_NO_BATCH=1` disables draw batching, and `tests/gpu_bench.c`
measures the driver in isolation. `VITA2NATIVE_NO_DRAW_UNIFORMS=1` and
`VITA2NATIVE_NO_PRESENT_OVERLAP=1` disable the per-draw uniform and direct
present overlap paths for A/B measurements. `VITA2NATIVE_RENDER_SCALE=0.5`
renders the 3D scene at half resolution and upscales it on present; on the
Mali-G31 that roughly halves GPU time, which is the dominant cost at the
native 960x544 resolution. RGBA4444 textures upload as `GL_RGBA4` by default
on the Mali blob, which cuts a 2048x2048 atlas upload from ~79 ms to ~49 ms;
Panfrost and software renderers keep the RGBA8 path because their `GL_RGBA4`
upload is several times slower. `VITA2NATIVE_RGBA4=1` and
`VITA2NATIVE_NO_RGBA4=1` force either path for A/B measurements. PVRTC-I 4bpp
textures use a specialised decoder (the PowerVR SDK reference implementation
stays for 2bpp and PVRTC-II): a 1024x1024 texture drops from ~425 ms to ~78 ms
on the R36S. `VITA2NATIVE_NO_PVRTC_FAST=1` forces the reference decoder.

The same ARM32 build also runs on an R36S handheld (Rockchip RK3326, Mali-G31
through the proprietary Bifrost blob, 640x480 DSI panel). Output goes through
SDL's KMSDRM driver, which is what the handheld's other titles use;
`source/host/host_video_sdl.c` only uploads the guest frame as an ABGR8888
texture and lets SDL scale, letterbox and present. `tests/present_probe.c`
cycles colours and a moving marker on the panel without starting the emulator.
On this board VitaShell renders at ~1.3 fps; the blob's per-draw overhead is the
bottleneck (about 5 ms per `glDrawElements`).

Input follows the same split. While SDL video is running, its dedicated video
thread samples input too: keyboards, plus pads that SDL recognises through its
controller database, with a raw-joystick fallback for unknown devices. When
evdev found a pad, SDL contributes the keyboard and stick axes only; pad
buttons come from evdev, because SDL's controller database is label-based and
can disagree with the Vita's positional layout (on the R36S the same face
button would otherwise be reported as both square and triangle). When evdev
found no pad, SDL's buttons are used instead. Keyboard mapping is arrows for
the D-pad, `Z`/`Enter` = cross, `X`/`Backspace` = circle, `A` = square,
`S` = triangle, `Q`/`E` = L1/R1, `1`/`2` = L2/R2 and `Tab`/`Space` =
Select/Start. Presses are latched until the guest polls, so a tap is not lost
on a title running at 1 fps.
`VITA2NATIVE_CTRL_SWAP_AB` swaps cross/circle on all sources, and
`VITA2NATIVE_INPUT_DEBUG=1` logs the mapping (`SDL mapping ...` shows the
mapping SDL picked for a controller). The same video-thread poll reads the
pointer: the front touch panel follows the mouse, mapped through the letterbox
rectangle into the Vita's 1920x1088 active area, and a click is latched until
the guest reads it. Clicks on the black bars are ignored. `sceCtrlSetActuator`
feeds the attached controller's rumble motors through SDL, and closing the SDL
window (or SDL_QUIT) exits the emulator cleanly.

The headless build always reads the pad directly from evdev
(`/dev/input/event*`), which is the reliable path on the R36S: face buttons,
D-pad, shoulders, the Select/Start/L3/R3 buttons (reported as
`BTN_TRIGGER_HAPPY1..4`) and both sticks. The Fn key (`BTN_TRIGGER_HAPPY5`) is
a firmware hotkey modifier and is ignored. When evdev found a pad its buttons
win over SDL's; otherwise SDL's buttons are ORed in. Axes come from SDL only
when a controller is attached, so a keyboard-only session cannot overwrite the
evdev stick values. Scripted input still works and
is ORed with both. `tools/ctrl_test` is a guest-side probe for this path: build
it with `python3 tools/ctrl_test/build.py` and it draws the live pad state on a
960x544 pane while logging every controller state change.

Video output goes through SDL, loaded with `dlopen` so the headless ARM binary
keeps no SDL build dependency. SDL chooses the platform driver itself: KMSDRM
on handhelds, X11 or Wayland on desktops, and offscreen/dummy on headless
machines (`SDL_VIDEODRIVER=dummy` forces a no-op output). The guest frame is
uploaded as `SDL_PIXELFORMAT_ABGR8888` — the guest's A8B8G8R8 byte order — so
SDL does the scaling and letterboxing without a CPU conversion. Frames are
handed to a dedicated video thread, so SDL's own GL context never touches the
guest's rendering context.
`VITA2NATIVE_STRETCH=1` fills the display and `VITA2NATIVE_SDL_WINDOWED=1` keeps
a window instead of going fullscreen.

### Remote testing over X11

Launch XQuartz, then connect with a local `DISPLAY` so ssh requests forwarding:

```sh
open -a XQuartz            # once
DISPLAY=:0 ssh -X opiz3
```

On the device:

```sh
cd /tmp/v2n-run
LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf \
SDL_VIDEODRIVER=x11 VITA2NATIVE_SDL_WINDOWED=1 \
/tmp/v2n-arm/vita2native-rel /tmp/VitaShell.vpk
```

`VITA2NATIVE_SDL_SIZE=480x272` uses a smaller window. The guest's GLES context
stays offscreen (surfaceless with Panfrost or the vendor blob); only
presentation goes through SDL's X11 driver. That split is deliberate: if the GL
context is created on the X11 EGL platform, Mesa uses llvmpipe over the
forwarded connection, which is roughly 5x slower and can crash in LLVM.
Forwarded X11 is also the easiest way to drive a title from a desktop keyboard.

### Headless capture and scripted input

Headless builds can dump guest framebuffers as BMP and drive the pad from a
frame-keyed script, which is how real homebrew is validated under QEMU:

```sh
VITA2NATIVE_DUMP_FRAME=/tmp/frame-%04u.bmp \
VITA2NATIVE_DUMP_FRAME_AT=0,120,240 \
VITA2NATIVE_CTRL_SCRIPT="30:down,32:,34:down,36:,38:cross,40:" \
GALLIUM_DRIVER=softpipe ./vita2native ./VitaShell.vpk
```

`..._AT` is a comma-separated list of swap indices; a `%u` in the path is
replaced with the frame number. Script entries are
`<frame>:<button>[+<button>...]`, applied until the next entry; an empty button
list releases everything. Script frames refer to presented frames, not host
time, so they do not depend on how slow software rendering is.

`kill -USR1 <tid>` with `VITA2NATIVE_DEBUG_SAMPLE=1` set dumps a thread's PC and
call stack, which is how guest/host hotspots were located on the device.
`VITA2NATIVE_GL_CHECK=1` re-enables the per-draw `glGetError` checks.

## Building from source

```sh
git clone --recurse-submodules <this-repository>
```

`CMakePresets.json` defines the supported configurations. The build does not
need devkitPro or any Horizon OS tooling.

## Credits

- [vita2hos](https://github.com/xerpi/vita2hos) by xerpi — the original
  Horizon OS translation layer this fork is based on.
- [Vita3K](https://vita3k.org/) — its shader recompiler (and portions of its
  code) are used here. Consider [donating](https://vita3k.org/#donate) and
  [contributing](https://vita3k.org/#contribute).
- The Khronos Group for the EGL/GLES headers vendored under
  `external/khronos/`.
- [FAAD2](https://github.com/knik0/faad2) — AAC decoding, vendored under
  `external/faad2/` (GPLv2; see `external/faad2/COPYING`).
- [stb](https://github.com/nothings/stb) — image decode/encode used by
  `SceJpeg` (public domain / MIT).

## Disclaimer

**PlayStation Vita** is a trademark of **Sony Interactive Entertainment**.
