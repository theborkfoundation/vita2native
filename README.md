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

Build natively on the ARM32 host:

```sh
cmake --preset Linux-Debug
cmake --build --preset Linux-Debug
```

Or configure manually:

```sh
cmake -S . -B build/Linux-Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Linux-Debug
```

For QEMU or a machine without SDL2, build the headless variant:

```sh
cmake -S . -B build/Linux-Headless -G Ninja -DVITA2NATIVE_HEADLESS=ON
cmake --build build/Linux-Headless
```

## Running

Run with the default layout (`vita2native/executable` relative to the current
working directory), or pass a path explicitly:

```sh
./build/Linux-Debug/vita2native ./vita2native/executable
```

A VPK can be passed directly. `eboot.bin` is loaded from memory and the
remaining entries are extracted to `vita2native/app0/` (skipped when the archive
size and mtime are unchanged), so `app0:` reads work:

```sh
./build/Linux-Debug/vita2native ./MyHomebrew.vpk
```

Vita device paths are remapped below `vita2native/`:

| Vita path | Linux path |
| --- | --- |
| `ux0:/...` | `vita2native/ux0/...` |
| `app0:/...` | directory containing the executable, or `vita2native/app0/` for a VPK |
| `savedata0:/...` | `vita2native/savedata/...` |
| `os0:`, `vs0:` | `vita2native/system/...` |

## Current status

CPU-rendered homebrew runs: ELF loading, relocations, import patching, threads,
TLS, file I/O, VPK assets, and process lifetime are implemented and tested.

A GLES 3.x GPU device (EGL surfaceless + pbuffer) is created at startup when the
libraries are available; otherwise a null backend is used. GXM is translated to
GLES 3.x: GXP shaders through Vita3K's recompiler, render targets, textures,
depth/stencil, MSAA and the transfer calls. `SceAudioOut`/`SceAudioIn`,
MP3/ATRAC9/AAC decoding, JPEG encode/decode, `SceNet`/`SceHttp`/`SceSsl`,
`SceSqlite`, `ScePgf`, `SceTouch`, `SceCommonDialog` (with an on-screen
overlay), a synthetic camera and `SceAvPlayer` (generated 30 fps video and
silent audio) and the kernel/thread surface are implemented. AAC uses the
bundled FAAD2 decoder; CELP decoding and encrypted SELF files are not supported.

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
that share program, textures, uniforms and render state are merged into one GL
draw (847 guest draws become ~119 GL draws per frame); triangle strips are
expanded to triangle lists when merging, because concatenating two strips would
add bridging triangles. `VITA2NATIVE_PROFILE=1` prints the frame breakdown,
`VITA2NATIVE_FPS=1` prints the frame rate, `VITA2NATIVE_TRACE_TEXTURES=1`
logs texture uploads, `VITA2NATIVE_NO_BATCH=1` disables draw batching and
`tests/gpu_bench.c` measures the driver in isolation.

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
controller database, with a raw-joystick fallback for unknown devices. Keyboard
mapping is arrows for the D-pad, `Z`/`Enter` = cross, `X`/`Backspace` = circle,
`A` = square, `S` = triangle, `Q`/`E` = L1/R1 and `1`/`2` = L2/R2. Presses are
latched until the guest polls, so a tap is not lost on a title running at 1 fps.
`VITA2NATIVE_CTRL_SWAP_AB` swaps cross/circle on all sources, and
`VITA2NATIVE_INPUT_DEBUG=1` logs the mapping (`SDL mapping ...` shows the
mapping SDL picked for a controller). The same video-thread poll reads the
pointer: the front touch panel follows the mouse, mapped through the letterbox
rectangle into the Vita's 1920x1088 active area, and a click is latched until
the guest reads it. Clicks on the black bars are ignored. `sceCtrlSetActuator`
feeds the attached controller's rumble motors through SDL, and closing the SDL
window (or SDL_QUIT) exits the emulator cleanly.

When SDL is unavailable, the headless build reads the pad directly from evdev
(`/dev/input/event*`), which remains the reliable path on the R36S: face
buttons, D-pad, shoulders, `BTN_SELECT/START`, the F1–F5 buttons and both
sticks. SDL and evdev states are ORed together; axes come from SDL only when a
controller is attached, so a keyboard-only session cannot overwrite the evdev
stick values. Scripted input still works and is ORed with both.

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
