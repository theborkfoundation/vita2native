# V2N Launcher

A 640×480 SDL2 and LVGL launcher with directory scanning (`*/eboot.bin`), optional `v2n.toml` game metadata, a Game Info screen, and a working package import flow.
Launching games from `PLAY` is still a placeholder; startup progress is simulated.

## Build

Initialize the submodules with `git submodule update --init --recursive`.
The launcher renders with LVGL's software renderer and presents through an SDL2 renderer.

The primary target is 64-bit ARM Linux (aarch64). On the target, install SDL2,
OpenSSL 3 and zlib development files (including the static
libcrypto archive), pkg-config, CMake 3.18+, a C/C++ compiler and Python 3:

```sh
cmake -S source/launcher -B build/launcher-arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher-arm64
```

The resulting executable is `build/launcher-arm64/v2n-launcher`.

## Native macOS build

`CMakePresets.json` also has a `macos` preset, handy for working on the UI locally.
SDL2 and OpenSSL 3 must be installed (`brew install sdl2 openssl@3`):

```sh
cmake -S source/launcher --preset macos
cmake --build build/launcher-macos
```

The result is `build/launcher-macos/v2n-launcher`. Run it from a directory of its
own: the launcher creates a `vita2native/` guest filesystem tree on startup.

## Cross-build from macOS

`CMakePresets.json` here has an `arm64-cross` preset that needs no ARM host and
no system cross toolchain. It uses the `external/zig-cross` submodule and a
bookworm/arm64 Debian sysroot:

```sh
tools/debian-sysroot.py --arch arm64   # populate build/sysroot-arm64; --verify checks it
cmake -S source/launcher --preset arm64-cross
cmake --build build/launcher-arm64-cross
```

The result is `build/launcher-arm64-cross/v2n-launcher`, an aarch64 ELF targeting
glibc 2.36. The preset links with `-s`, because `zig cc` emits DWARF even at
`-O3`; stripping takes the binary from ~26 MB to ~6 MB. The ARM32 runtime sysroot
is separate and untouched.

For a symbolized build to debug on a target, configure with
`-DCMAKE_EXE_LINKER_FLAGS=` and a separate build directory.

The repository's `release` preset targets 32-bit ARM for the vita2native
runtime. It can also build a 32-bit launcher for compatible systems, but it is
not the launcher's primary target:

```sh
cmake --preset release
cmake --build --preset release --target v2n-launcher
```

Inter and the Lucide icon font (`lucide.ttf`) are embedded in the executable;
their licenses are in `assets/INTER-LICENSE.txt` and `assets/LUCIDE-LICENSE.txt`.
The four sample images in `assets/` were generated with OpenAI image generation:

- `cover-0.jpg`: cloaked explorer, twilight valley and luminous arch.
- `cover-1.jpg`: silver-armored explorer and floating ruins in space.
- `cover-2.jpg`: neon racing vehicle in a rainy nighttime city.
- `cover-3.jpg`: traveler in a teal bioluminescent forest.

## Run and import

Run the launcher from the directory containing `launcher.toml`:

```sh
cd ~/v2n
./v2n-launcher
```

A configuration file is optional. Copy `launcher.toml.example` to
`launcher.toml` and edit it to change the defaults:

```toml
[paths]
games = "./games"
loader = "./v2n"

[appearance]
# logo = "./assets/logo.svg" # PNG and SVG are supported
```

Relative paths are resolved against the directory containing `launcher.toml`,
or against the startup working directory when it is absent. `games` is the
directory for imported and scanned title-ID folders: any subdirectory matching
`*/eboot.bin` is added to the library with its folder name as the Title ID and
default display name.
If a title folder contains an optional `v2n.toml`, the launcher reads `name`,
`description`, `category`, `rating`, `art` (or `hero`), `boxart` (or `cover`),
and `screenshots` (string or array of strings), either as top-level keys or
under `[meta]` / `[game]` and `[media]`. Image fields accept glob patterns
(`*`, `?`, `**`) relative to the title directory.
`loader` is retained for launching titles from Game Info (currently a placeholder). The optional logo replaces the built-in mark on the splash and
header, retaining its aspect ratio. If it cannot be loaded, the launcher shows a
warning and uses the built-in mark. LunaSVG supports many SVG features, though
complex SVGs may not render identically to a browser. An invalid or unreadable
configuration displays an error; correct the file and restart.

Package, PFS and SELF extraction is linked into the launcher. Imports run in a
background thread, which sends progress and completion through a bounded channel
to the SDL thread. No separate extractor executable or OpenSSL installation is
needed at runtime. The ARM32 preset uses the locked sysroot's static libcrypto
and zlib archives; native ARM64 builds use the development libraries installed
on the build host.

Select a `.pkg` and its `work.bin`. Imports use a hidden staging directory
inside `games` and move the completed title there only after success. Existing
title directories are never intentionally replaced. On failure or if the
destination exists, the staging directory and `import.log` are kept for
inspection. Pipeline diagnostics go to `import.log`; the underlying PFS engine
also writes diagnostics to the application console.

## Controls

- D-pad or arrow keys: navigate. Analog sticks are also supported.
- A / Enter / Space: select. B / Escape / Backspace: back; on Games, exit.
- On the home screen, left/right cycle through recent games (up to 8 titles). Down moves to the two action cards; left/right select a card and up returns to recent games. Selecting a game opens Game Info.
- In All Games, navigate the 2-column scrollable grid with the D-pad or arrows; A opens Game Info and B or Escape returns to the home screen.
- In Game Info, Up/Down move focus between `PLAY` and the `Screenshots` strip, Left/Right cycle screenshots, A triggers `PLAY` (currently a placeholder notification), and B or Escape returns to the previous screen.
- The picker shows directories and matching files; select `..` to go up.

SDL game controllers are supported, including hot-plugging. Unmapped joysticks
use hat, first two axes, buttons 0/1 and shoulder buttons 4/5. Actual device
mappings vary; a platform SDL game-controller mapping may be needed.

## Source layout

- `main.cpp` owns the SDL window, graphics context and event loop.
- `config.cpp` loads and validates `launcher.toml`; `logo.cpp` loads PNG and SVG artwork.
- `state.h` and `state.cpp` define navigation, focus and picker state.
- `input.cpp` maps keyboard, controller and joystick events to navigation actions.
- `import.cpp` starts the import thread and receives its channel messages.
- `../package/` implements the package, PFS and SELF extraction pipeline.
- `library.h` and `library.cpp` scan `*/eboot.bin`, parse `v2n.toml`, and expand media globs.
- `ui.cpp` builds and updates the screens with LVGL.

The launcher is excluded from the project test suite.
