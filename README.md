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
`SceSqlite`, `ScePgf`, `SceTouch`, `SceCommonDialog`, a synthetic camera and the
kernel/thread surface are implemented. AAC uses the bundled FAAD2 decoder;
CELP decoding and encrypted SELF files are not supported.

Guest code runs natively in the host process; there is no sandbox or isolation.

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
