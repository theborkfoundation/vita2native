# vita2hos

A PlayStation Vita to Horizon OS (Nintendo Switch OS) translation layer (**_not_** an emulator)

## Overview

**vita2hos** is a translation layer that enables native execution of PlayStation Vita executables (ARMv7), to run natively on the Nintendo Switch's ARMv8 CPU in AArch32 (32-bit) mode.\
It achieves this by redirecting the [module](https://wiki.henkaku.xyz/vita/Modules) imports of the PlayStation Vita executable to corresponding routines that utilize native Nintendo Switch Horizon OS services, effectively replicating the behavior of the original PlayStation Vita OS modules.

## Linux ARM32 port (this fork)

This fork targets a typical **32-bit ARM Linux** system instead of Horizon OS.
The first milestone is a CPU-only runtime (no GXM/3D backend yet).

Requirements:

- 32-bit ARMv7-A userland with NEON and hard-float (`armhf`), e.g. Raspberry
  Pi 4/5 with a 32-bit OS.
- CMake, Ninja, pkg-config, and SDL2 development files (SDL2 is not needed in
  headless mode), for example on Debian/Raspberry Pi OS:
  `sudo apt install cmake ninja-build pkg-config libsdl2-dev`.
- A Vita executable built with the default hard-float VitaSDK toolchain.

Build natively on the ARM32 host:

```sh
cmake --preset Linux-Debug
cmake --build --preset Linux-Debug
```

Or configure manually:

```sh
cmake -S . -B build/Linux-Debug -G Ninja \
  -DVITA2NATIVE_LINUX=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Linux-Debug
```

For QEMU or a machine without SDL2, build the headless variant:

```sh
cmake -S . -B build/Linux-Headless -G Ninja \
  -DVITA2NATIVE_LINUX=ON -DVITA2NATIVE_HEADLESS=ON
cmake --build build/Linux-Headless
```

Run with the default layout (`vita2native/executable` relative to the current
working directory), or pass a path explicitly:

```sh
./build/Linux-Debug/vita2native ./vita2native/executable
```

A VPK can be passed directly as well. `eboot.bin` is loaded from memory and the
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

Current limitations: GXM command translation is not implemented yet (3D homebrew
will hit unresolved `SceGxm` imports), no audio, and encrypted SELF files are
still unsupported. A GLES 3.x GPU device is created at startup when EGL and
GLESv2 are available (both are loaded at runtime; otherwise a null backend is
used).

## Usage

### On a Real Nintendo Switch Console

1. **Transfer the NSP File:**
   - Copy `vita2hos.nsp` to your microSD card, e.g., `sd:/vita2hos/vita2hos.nsp`.

2. **Configure Atmosphère:**
   - Create an `override_config.ini` file in the `sd:/atmosphere/config/` directory with the following content:

     ```ini
     [hbl_config]
     override_any_app=true
     override_any_app_key=R
     override_any_app_address_space=32_bit
     ; Adjust the path according to the location of your file
     path=vita2hos/vita2hos.nsp
     ```

   - *Note:* With this configuration, the homebrew menu will be overridden by **vita2hos**. To restore the homebrew menu, rename or remove the `override_config.ini` file and restart your Switch. Currently, multiple `path` entries are not supported in `override_config.ini`.

3. **Add PlayStation Vita Executable:**
   - Place your PlayStation Vita executable (`.velf`, `.self`, or `eboot.bin`) in the `sd:/vita2hos/` directory and rename it to `executable` (without any file extension).\
   The resulting full path should be `sd:/vita2hos/executable`.

4. **Launch vita2hos:**
   - Boot or reboot your Switch, and start any game while holding down the `R` button.
     - *Caution:* Using **vita2hos** via applet mode (accessed through the album button) may result in a fatal error and is not recommended.

5. **Enjoy!**

### On an Emulator

1. **Prepare the PlayStation Vita Executable:**
   - Place your PlayStation Vita executable (`.velf`, `.self`, or `eboot.bin`) into the `sd:/vita2hos/` directory and rename it to `executable` (without any file extension).

     - **For Yuzu-based Emulators:**
       - Access the SD directory via _File_ → _Open yuzu Folder_ → `sdmc/`.

     - **For Ryujinx-based Emulators:**
       - Access the SD directory via _File_ → _Open Ryujinx Folder_ → `sdcard/`.

2. **Run vita2hos:**
   - Launch `vita2hos.nsp`.

3. **Enjoy!**

## Project Status and Compatibility

**vita2hos** is in its early development stages and currently supports running simple CPU-rendered PlayStation Vita homebrew applications. Additionally, initial 3D graphics support is available—including texturing, depth, and stencil—enabling the execution of vitasdk's GXM samples.

## Building from Source

1. **Clone the Repository with Submodules:**

   Ensure you clone the repository along with its submodules to include all necessary components:

   ```bash
   git clone --recurse-submodules https://github.com/xerpi/vita2hos.git
   ```

2. **Configure vita2hos:**
   - For a Debug build:

     ```bash
     cmake --preset Debug
     ```

   - For a Release build:

     ```bash
     cmake --preset Release
     ```

3. **Build vita2hos:**
   - For a Debug build:

     ```bash
     cmake --build --preset Debug
     ```

   - For a Release build:

     ```bash
     cmake --build --preset Release
     ```

4. **Output:**
   - The `vita2hos.nsp` file will be generated in the corresponding build directory upon successful compilation.

*Note:* The `CMakePresets.json` file defines the build configurations, including the generator (e.g., Ninja), build directories, toolchain file, and other cache variables. Ensure that your environment variable `DEVKITPRO` is set correctly, as it's referenced in the `CMAKE_TOOLCHAIN_FILE` path within the presets.

## Special Thanks

- **[Vita3K](https://vita3k.org/):**
  - **vita2hos** utilizes Vita3K's shader recompiler, and portions of its code are based on Vita3K's implementation. Consider [donating](https://vita3k.org/#donate) and [contributing](https://vita3k.org/#contribute) to Vita3K!

- **[UAM - deko3d Shader Compiler](https://github.com/devkitPro/uam):**
  - **vita2hos** employs UAM (deko3d's shader compiler) for shader compilation. Contributions and donations to this project are highly appreciated!

- **[Ryujinx](https://ryujinx.org/)**

- **[yuzu](https://yuzu-emu.org/)**

- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**

- **[Switchbrew](https://github.com/switchbrew/)**

Special acknowledgments to @PixelyIon and @SciresM for their assistance, and to all testers, especially @TSRBerry.

## Disclaimer

- **Nintendo Switch** is a trademark of **Nintendo Co., Ltd**.
- **PlayStation Vita** is a trademark of **Sony Interactive Entertainment**.
