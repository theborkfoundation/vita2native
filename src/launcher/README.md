# V2N Launcher

A 640×480 SDL2 and NanoVG launcher with a mock library and a working package import flow.
The Recent Games and All Games views use placeholder entries; games are not scanned or started.
The startup progress is simulated.

## Build

Initialize the submodules with `git submodule update --init --recursive`.
The launcher needs an OpenGL ES 2.0 context on Linux (OpenGL 3 on macOS).

The primary target is 64-bit ARM Linux (aarch64). On the target, install SDL2,
OpenGL ES 2, OpenSSL 3 and zlib development files (including the static
libcrypto archive), pkg-config, CMake 3.18+, a C/C++ compiler and Python 3:

```sh
cmake -S src/launcher -B build/launcher-arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher-arm64
```

The resulting executable is `build/launcher-arm64/v2n-launcher`. The same
standalone build also works natively on macOS, using a separate build directory.

The repository's `release` preset targets 32-bit ARM for the vita2native
runtime. It can also build a 32-bit launcher for compatible systems, but it is
not the launcher's primary target:

```sh
cmake --preset release
cmake --build --preset release --target v2n-launcher
```

Inter and the sample cover artwork are embedded in the executable. The font
license is in `assets/INTER-LICENSE.txt`. The four original placeholder images
were generated with OpenAI image generation for this launcher:

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
extract = "./games"
loader = "./v2n"

[appearance]
# logo = "./assets/logo.svg" # PNG and SVG are supported
```

Relative paths are resolved against the directory containing `launcher.toml`,
or against the startup working directory when it is absent. `extract` is the
directory for imported title-ID folders. It is created when an import begins.
`loader` is retained for a future game-launching feature; the current mock games
cannot be started. The optional logo replaces the built-in mark on the splash and
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
inside `extract` and move the completed title there only after success. Existing
title directories are never intentionally replaced. On failure or if the
destination exists, the staging directory and `import.log` are kept for
inspection. Pipeline diagnostics go to `import.log`; the underlying PFS engine
also writes diagnostics to the application console.

## Controls

- D-pad or arrow keys: navigate. Analog sticks are also supported.
- A / Enter / Space: select. B / Escape / Backspace: back; on Games, exit.
- On the home screen, left/right cycle through recent games. Down moves to the two action cards; left/right select a card and up returns to recent games.
- In All Games, navigate the grid with the D-pad or arrows; B or Escape returns to the home screen.
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
- `ui.cpp` draws the screens with NanoVG; `library.h` holds the mock games.

The launcher is excluded from the project test suite.
