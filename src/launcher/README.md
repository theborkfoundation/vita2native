# V2N Launcher

A 640×480 SDL2 and NanoVG launcher with a mock library and a working package import flow.
The Recent Games and All Games views use placeholder entries; games are not scanned or started.
The startup progress is simulated.

## Build

Initialize the NanoVG submodule with `git submodule update --init --recursive`.
The launcher needs an OpenGL ES 2.0 context on Linux (OpenGL 3 on macOS).

The normal ARM build includes `build/release/src/launcher/v2n-launcher`:

```sh
cmake --preset release
cmake --build --preset release --target v2n-launcher
```

For a native build, install SDL2 development files, pkg-config, CMake and Python 3:

```sh
cmake -S src/launcher -B build/launcher-host
cmake --build build/launcher-host
ctest --test-dir build/launcher-host --output-on-failure
```

Inter and the sample cover artwork are embedded in the executable. The font
license is in `assets/INTER-LICENSE.txt`. The four original placeholder images
were generated with OpenAI image generation for this launcher:

- `cover-0.jpg`: cloaked explorer, twilight valley and luminous arch.
- `cover-1.jpg`: silver-armored explorer and floating ruins in space.
- `cover-2.jpg`: neon racing vehicle in a rainy nighttime city.
- `cover-3.jpg`: traveler in a teal bioluminescent forest.

## Run and import

Run the launcher from the directory where you want imported title folders:

```sh
cd ~/v2n
./v2n-launcher
```

Import needs a `v2n-extract` executable built for the **same machine** and its
OpenSSL 3 and zlib runtime libraries. Place it beside the launcher or on `PATH`,
or set `V2N_EXTRACT` to its path. On an ARM device, build the extractor natively
with `tools/v2n-extract/build.sh` after installing CMake, C/C++ compilers,
OpenSSL 3 and zlib development packages. The existing ARM cross sysroot does
not include OpenSSL; the normal ARM build does not build the extractor.

Select a `.pkg` and its `work.bin`. Imports run in a hidden staging directory
in the current working directory and only move the completed title there after
success. Existing title directories are never intentionally replaced. On failure
or if the destination exists, the staging directory and `import.log` are kept
for inspection.

## Controls

- D-pad or arrow keys: navigate. Analog sticks are also supported.
- A / Enter / Space: select. B / Escape / Backspace: back; on Games, exit.
- On the home screen, left/right cycle through recent games. Down moves to the two action cards; left/right select a card and up returns to recent games.
- In All Games, navigate the grid with the D-pad or arrows; B or Escape returns to the home screen.
- The picker shows directories and matching files; select `..` to go up.

SDL game controllers are supported, including hot-plugging. Unmapped joysticks
use hat, first two axes, buttons 0/1 and shoulder buttons 4/5. Actual device
mappings vary; a platform SDL game-controller mapping may be needed.
