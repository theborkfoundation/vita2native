# vita2native

A PlayStation Vita to Linux ARM32 translation layer — **_not_** an emulator.

A fork of [vita2hos](https://github.com/xerpi/vita2hos) retargeting the host
from Horizon OS to 32-bit ARM Linux. Vita executables (ARMv7) run natively,
their module imports redirected to host routines backed by POSIX, SDL2 and
GLES 3.x. **No CPU emulation**, so an ARMv7-A armhf userland is required.

## Building

Needs CMake, Ninja, pkg-config and [Zig](https://ziglang.org) (the presets
cross-compile with `zig cc`; no system cross toolchain).

```sh
tools/zig-toolchain.sh     # once
tools/debian-sysroot.py    # once: the armhf sysroot
cmake --preset release
cmake --build --preset release
```

| Preset | Use |
| --- | --- |
| `release` | the deployment artifact |
| `debug` | symbols for on-device `gdb` |
| `minsize` | size experiments; backtraces lose names |

On a 32-bit ARM host, `cmake -S . -B build/native-arm -G Ninja` works with the
distribution's `libsdl2-dev` instead. SDL2 and EGL/GLESv2 are `dlopen`ed, so
the binary links neither.

## Running

```sh
./build/release/vita2native ./vita2native/executable
```

The binary must **not** be named `vita2native` — that is the runtime's
filesystem root. Deploy as `v2n-arm`.

`ux0:/` and `app0:/` map to `vita2native/ux0/` and the executable's directory,
`savedata0:/` to `vita2native/savedata/<title-id>/`.

## Video output

SDL owns the window; EGL owns the display, context and surfaces. SDL picks the
driver and the GL layer builds EGL on the handle it reports. On KMSDRM it
creates a `gbm_surface` on SDL's `gbm_device` and page-flips it.

Presentation is GPU-side: the guest render target is blitted straight to the
window surface, so no `glReadPixels` runs on the normal path. A software
compositor (Weston's Pixman backend) yields a software GL device instead, and
its own swap copy then dominates the frame.

## Status

Homebrew runs end to end: ELF loading, relocations, import patching, threads,
TLS, file I/O, VPK assets. GXM is translated to GLES 3.x — GXP shaders via
Vita3K's recompiler, render targets, textures, depth/stencil, MSAA, transfers.
Audio, `SceNet`, `SceSqlite`, `ScePgf`, `SceTouch` and `SceAvPlayer` work.
Encrypted SELF, CELP decoding and `sceGu` do not.

Guest code is native code in the host process. **There is no sandbox.**

VitaShell, PixelChess, Pong, Minesweeper, VitaSudoku and ElevenMPV load with
zero unresolved imports. Angry Birds Trilogy and Iconoclasts run on real ARM32
hardware (H618 + Mali-G31/Panfrost; RK3326 + Mali-G31).

## Diagnostics

Knobs live in `include/settings.h`; `VITA2NATIVE_ENV_DUMP=1` lists them.

| Variable | Effect |
| --- | --- |
| `VITA2NATIVE_FPS=1` | frame rate |
| `VITA2NATIVE_PROFILE_FRAME=1` | present / vblank / GXM / guest split |
| `VITA2NATIVE_GL_CHECK=1` | per-draw `glGetError` checks |
| `VITA2NATIVE_DUMP_FRAME` | guest framebuffers as BMP |
| `VITA2NATIVE_RENDER_SCALE=0.5` | render 3D at half resolution |

```sh
VITA2NATIVE_DUMP_FRAME=/tmp/f-%04u.bmp VITA2NATIVE_DUMP_FRAME_AT=0,120 \
VITA2NATIVE_CTRL_SCRIPT="30:down,32:,34:cross,40:" \
SDL_VIDEODRIVER=dummy ./v2n-arm ./VitaShell.vpk
```

`tools/v2n-extract` turns a `.pkg` plus `work.bin` into a runnable directory.

## Credits

[vita2hos](https://github.com/xerpi/vita2hos) by xerpi — the original.
[Vita3K](https://vita3k.org/) for its GXP shader recompiler. Khronos for the
vendored EGL/GLES headers. [FAAD2](https://github.com/knik0/faad2) for AAC
(GPLv2). [stb](https://github.com/nothings/stb) for the `SceJpeg` codec.

## Disclaimer

**PlayStation Vita** is a trademark of **Sony Interactive Entertainment**.
