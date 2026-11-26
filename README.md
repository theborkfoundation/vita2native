# vita2native

A PlayStation Vita to Linux ARM32 translation layer — **_not_** an emulator.

A fork of [vita2hos](https://github.com/xerpi/vita2hos) that retargets the host
from Horizon OS to 32-bit ARM Linux. Vita executables (ARMv7) run natively:
their module imports are redirected to host routines backed by POSIX, SDL2 and
GLES 3.x. **There is no CPU emulation** — Vita code executes directly on the host
CPU, which is why an ARMv7-A armhf userland is required.

## Building

Needs CMake, Ninja, pkg-config, [Zig](https://ziglang.org) and the submodules.
The presets cross-compile through [zig-cross](external/zig-cross) and `zig cc`,
so no system cross toolchain is needed.

```sh
git submodule update --init --recursive
tools/debian-sysroot.py    # populate or update the locked armhf sysroot
cmake --preset release
cmake --build --preset release
```

| Preset | Use |
| --- | --- |
| `release` | the deployment artifact |
| `debug` | symbols for on-device `gdb` |
| `minsize` | size experiments only; crash backtraces lose names |

Rerun `tools/debian-sysroot.py` after the sysroot lock changes. These presets
target the ARM32 runtime. The launcher primarily targets ARM64 Linux and links
package extraction into its own binary; see [launcher build instructions](source/launcher/README.md).

On a 32-bit ARM host the build also works without a cross toolchain, using the
distribution's SDL2:

```sh
sudo apt install libsdl2-dev libssl-dev zlib1g-dev
cmake -S . -B build/native-arm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native-arm
```

SDL2 and EGL/GLESv2 are `dlopen`ed, so the binary never links against them.

## Running

```sh
./build/release/vita2native ./vita2native/executable
./build/release/vita2native ./MyHomebrew.vpk
```

The binary can keep its own name. The guest filesystem root is `storage/` in the
working directory — or whatever `paths.storage` names in a `launcher.toml`
beside it — so `vita2native` no longer collides with anything.

### Options

```
vita2native [--headless] [title.vpk|eboot.bin]
```

| Option | Effect |
| --- | --- |
| `-h`, `--help` | show the usage summary and exit |
| `--help-settings` | list every `VITA2NATIVE_*` setting with its description |
| `-V`, `--version` | print the version and exit |
| `--headless` | run without a window |

With no title, `vita2native/executable` is loaded from the working directory.
`--` ends option parsing, so a title may begin with a dash. Exit status is 0 on
success, 1 for a runtime failure, and 2 for a usage error — an unknown option, a
second title, or an empty argument.

Vita device paths are remapped below `vita2native/`:

| Vita path | Linux path |
| --- | --- |
| `ux0:/...` | `vita2native/ux0/...` |
| `app0:/...` | executable's directory, or `vita2native/app0/` for a VPK |
| `savedata0:/...` | `vita2native/savedata/<title-id>/...` |
| `os0:`, `vs0:` | `vita2native/system/...` |

## Video output

SDL owns the window; EGL owns the display, context and surfaces. SDL picks the
driver (X11, Wayland, KMSDRM, offscreen) and the GL layer builds its EGL display
on the native handle SDL reports. Nothing overrides that beyond an explicit
`SDL_VIDEODRIVER`. On KMSDRM the GL layer creates a `gbm_surface` on SDL's
`gbm_device` and page-flips it.

Presentation is GPU-side: the guest render target is blitted straight to the
window surface, so no `glReadPixels` runs on the normal path. A CPU frame (video
decode, guest-side composite) is uploaded as a texture and blitted the same way.
The aspect-fitted game frame is aligned to the top of the screen. The remaining
space is filled by the default border artwork, `source/host/overlay-landscape.png`
(a 640×117 pixel-art banner, embedded at build time), which is the size that
matches the area below a 960×544 Vita frame on a 640×480 panel. To use your own
image instead:

```sh
cmake --preset release -DVITA2NATIVE_OVERLAY_PNG=/absolute/path/to/overlay.png
cmake --build --preset release
```

The image is embedded in the executable; the file is not needed on the device.
Other image sizes are stretched to fit, and transparent pixels are blended
against the blue-gray base colour. Reconfigure with `-DVITA2NATIVE_OVERLAY_PNG=`
for the generated blue-gray placeholder instead.

Under a software compositor — for example Weston on its Pixman backend — the GL
device is software too, and the compositor's own swap copy then dominates the
frame. On a GPU-composited Wayland or X11 session it is the hardware driver.

### Frame pacing

Vblanks are a wall-clock 60 Hz grid, so a title that waits on
`sceDisplayWaitVblankStart` keeps time even when a frame costs more than 16.6 ms.
Titles that instead treat every drawn frame as 1/60 s do not: when the host
cannot finish a frame in time, the game itself plays back slow. Frame skipping
drops the render work of some frames so the guest loop keeps running at 60
frames per second, at the cost of presenting fewer of them:

```sh
VITA2NATIVE_VIDEO_FRAME_SKIP=auto ./vita2native game.vpk   # skip as needed
VITA2NATIVE_VIDEO_FRAME_SKIP=2    ./vita2native game.vpk   # draw one frame in two
```

`auto` measures a drawn frame and a skipped one, then picks the smallest ratio
that fits the loop into 60 Hz: it does not skip while the title already holds
60, and it pauses itself when skipping stops saving time, which means the guest
rather than the renderer is the limit. A fixed `N` draws one frame in N, for A/B
comparisons. It is off by default, because a skipped frame keeps the previous
image on screen and that is wrong for a title that reads its own framebuffer.

A skipped frame runs no draws at all, so anything a frame *produces* is missing
from it: a render target the title builds and samples in the same frame keeps
its previous contents, which shows up as stale or blank textures on titles that
compose their own atlases per frame. The first frames after launch are never
skipped, and neither is the first draw into a render target that has not been
painted yet, so a fresh target is never sampled uninitialised.

## Status

Homebrew runs end to end: ELF loading, relocations, import patching, threads,
TLS, file I/O, VPK assets and process lifetime.

GXM is translated to GLES 3.x: GXP shaders through Vita3K's recompiler, render
targets, textures, depth/stencil, MSAA and the transfer calls. Audio
(`SceAudioOut`/`SceAudioIn`, `SceNgs`, ATRAC9/HEVAG/MP3/AAC), `SceNet`/`SceHttp`/
`SceSsl`, `SceSqlite`, `ScePgf`, `SceTouch`, `SceCommonDialog`, a synthetic
camera and `SceAvPlayer` are implemented. Encrypted SELF, CELP decoding and
`sceGu` are not.

Guest code is native code in the host process. **There is no sandbox or
isolation.**

VitaShell, PixelChess, Pong, Minesweeper, VitaSudoku and ElevenMPV load with
zero unresolved imports and render end to end. Angry Birds Trilogy and
Iconoclasts run on real ARM32 hardware: an Allwinner H618 with Mali-G31 through
Panfrost, and a Rockchip RK3326 with Mali-G31 through the vendor blob.

## Diagnostics

Every knob lives in `include/settings.h` and is cached at startup.
`--help-settings` lists them with their descriptions; `VITA2NATIVE_CLI_ENV_DUMP=1`
prints the same table with the current values.

| Variable | Effect |
| --- | --- |
| `VITA2NATIVE_DEBUG_FPS=1` | frame rate, once per second |
| `VITA2NATIVE_DEBUG_PROFILE_FRAME=1` | present / vblank / GXM / guest split |
| `VITA2NATIVE_DEBUG_PROFILE=1` | GXM draw breakdown |
| `VITA2NATIVE_DEBUG_GL_CHECK=1` | per-draw `glGetError` checks |
| `VITA2NATIVE_DEBUG_DUMP_PRESENTED_FRAMES` | write presented frames as BMP (GPU readback when a window is used) |
| `VITA2NATIVE_VIDEO_READBACK=1` | copy every frame back to guest memory (only titles that read their own framebuffer need it) |
| `VITA2NATIVE_VIDEO_NO_DMABUF_READBACK=1` | use `glReadPixels` instead of blitting into a linear dma-buf |
| `VITA2NATIVE_VIDEO_RENDER_SCALE=auto` | draw at the display's resolution (or a fixed 0.25–1.0 ratio) |
| `VITA2NATIVE_VIDEO_RENDER_SCALE_SCOPE=present` | scale only the target that is presented (default `all`) |
| `VITA2NATIVE_VIDEO_STRETCH=1` | fill the display instead of letterboxing |

Headless capture and scripted input, which is how titles are validated without a
panel:

```sh
VITA2NATIVE_DEBUG_DUMP_PRESENTED_FRAMES=/tmp/frame-%04u.bmp \
VITA2NATIVE_DEBUG_DUMP_PRESENTED_AT=0,120,240 \
VITA2NATIVE_INPUT_PAD_SCRIPT="30:down,32:,34:cross,40:" \
SDL_VIDEODRIVER=dummy ./vita2native ./VitaShell.vpk
```

`..._AT` is a comma-separated list of swap indices; a `%u` in the path becomes
the frame number. Script entries are `<frame>:<button>[+<button>...]` and apply
until the next entry. `VITA2NATIVE_INPUT_TOUCH_SCRIPT` drives the touch panel the same
way, as `<frame>:<x>/<y>`.

A frame is copied out of the render target only when something needs it in guest
memory: a capture (just the frames you asked for), a window that cannot present
the texture, or `VITA2NATIVE_VIDEO_READBACK=1`. The copy measured 62 ms per frame at
960×544 on the Orange Pi, so it is not paid by default.

When it does run, the render target is blitted into a linear dma-buf and the
guest surface is filled from its CPU mapping: 5.9 ms of blit plus ~19 ms of copy
on that board, against 62 ms for `glReadPixels`, whose cost on a tiled GPU is the
driver walking the surface. Software GL keeps the direct read (its readback is
already a memcpy), and `VITA2NATIVE_VIDEO_NO_DMABUF_READBACK=1` forces it anywhere else.

Signals grab a frame from a running session, whatever the backend:

```sh
kill -USR2 <pid>   # next presented frame -> screenshot-<frame>.bmp in the cwd
kill -USR1 <tid>   # sample one thread's PC and stack (VITA2NATIVE_DEBUG_SAMPLE=1)
```

The handler only raises a flag; the write happens on the present thread, so the
image is a complete frame and no GL or file work runs in signal context. A
requested screenshot resolves the colour surface for that frame even when the
copy is otherwise off.

## Tools

In `tools/`:

- `debian-sysroot.py` — populates `build/sysroot-armhf` from Debian, pinned by
  `sysroots/armhf.lock` so a point release cannot change what gets linked.
- `v2n-extract` — turns a `.pkg` plus its `work.bin` license into a runnable
  directory, peeling the pkg, PFS and SELF layers.
- `ctrl_test` — a guest executable that draws the live pad state, for checking
  controller mapping end to end.
- `fps_chart.py` — serves a live FPS chart from `VITA2NATIVE_DEBUG_METRICS_FILE` on
  `127.0.0.1:8099`.
- `img2png.py` — converts `VITA2NATIVE_DEBUG_DUMP_PRESENTED_FRAMES` BMPs and XWD screenshots to
  PNG, with no dependencies beyond the standard library.
- `gdb/live-attach.gdb` — attaches to a running process and stops on the guest's
  fatal signal, dumping registers and its stack.

## Credits

- [vita2hos](https://github.com/xerpi/vita2hos) by xerpi — the original Horizon
  OS translation layer this fork is based on.
- [Vita3K](https://vita3k.org/) — its GXP shader recompiler, and portions of its
  code. Consider [donating](https://vita3k.org/#donate).
- The Khronos Group for the vendored EGL/GLES headers.
- [FAAD2](https://github.com/knik0/faad2) — AAC decoding (GPLv2; see
  `external/faad2/COPYING`).
- [stb](https://github.com/nothings/stb) — image decode/encode used by
  `SceJpeg`.
- [argparse](https://github.com/cofyc/argparse) — the command-line parser
  (MIT).

## Disclaimer

**PlayStation Vita** is a trademark of **Sony Interactive Entertainment**.
