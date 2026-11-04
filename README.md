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
libraries are available; otherwise a null backend is used. GXM command
translation is not implemented yet, so 3D homebrew will hit unresolved `SceGxm`
imports. Audio and encrypted SELF files are also not supported yet.

Guest code runs natively in the host process; there is no sandbox or isolation.

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

## Disclaimer

**PlayStation Vita** is a trademark of **Sony Interactive Entertainment**.
