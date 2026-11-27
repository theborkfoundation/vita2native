# AGENTS

How to build vita2native and get a binary onto a test bed.

**`ISSUES.md` is the issue tracker — read it before starting work and update it
as you go.** It lists every open defect, performance target and planned feature
with a stable `I-nn` id, a location to start from, and the evidence behind it.
It is git-ignored, so it will not appear in `git status`; it exists in the
working tree. What the emulator supports today is in `README.md`.

## Build

```sh
git submodule update --init --recursive   # once: zig-cross and the vendored libs
tools/debian-sysroot.py    # once: the armhf sysroot
cmake --preset release
cmake --build --preset release
```

The artifact is `build/release/vita2native`. For on-device debugging use
`debug`, which keeps symbols.

Cross builds link the target's SDL2 from `build/sysroot-armhf`, pinned by
`tools/sysroots/armhf.lock`. `tools/debian-sysroot.py --verify` checks the tree
against its lock.

Both targets share one toolchain mechanism: `cmake/toolchains/zig-armhf.cmake`
and `zig-arm64.cmake` include `external/zig-cross/cmake/zig-toolchain.cmake`,
which sets the Zig wrappers and `CMAKE_SYSTEM_*`. Only `ZIG_TARGET`, the sysroot
and the multiarch triplet differ. Note that Zig resolves `-L` relative to
`--sysroot` but `-I` absolutely, so the two files differ on `CMAKE_SYSROOT_LINK`:
the arm64 build clears it because its pkg-config paths are absolute, while the
armhf build keeps it because `cmake/deps.cmake` leaves `PKG_CONFIG_SYSROOT_DIR`
unset and passes sysroot-relative `-L`.

The launcher under `source/launcher` is its own ARM64 project. It cross-builds from
macOS with the `external/zig-cross` submodule and a separate bookworm/arm64
sysroot, leaving the armhf one alone:

```sh
tools/debian-sysroot.py --arch arm64      # once: build/sysroot-arm64
cmake -S source/launcher --preset arm64-cross
cmake --build build/launcher-arm64-cross
```

The artifact is `build/launcher-arm64-cross/v2n-launcher`, an aarch64 ELF for
glibc 2.36. The preset links with `-s`: `zig cc` emits DWARF even at `-O3`, and
stripping takes it from ~26 MB to ~6 MB, so a symbolized build needs a separate
directory and an empty `CMAKE_EXE_LINKER_FLAGS`. The `vm` bed runs aarch64
natively; it has no display, so use `SDL_VIDEODRIVER=offscreen` for a load check
and a display-backed bed for GL.

## Staging the binary

**Copy it as `vita2native`, into `~/vita2native/` on the test bed:**

```sh
scp build/release/vita2native <host>:~/vita2native/vita2native
```

That directory is the layout the loader and launcher expect:

```
vita2native/            launcher, launcher.toml, games/<ID>/, storage/
```

`launcher.toml` names the paths: `games` (imported titles), `loader` (the
binary below it) and `storage` (the guest filesystem — `ux0/`, `savedata/`,
`system/`, `cache/`, `log.txt`). The loader reads `paths.storage` from it in
the working directory, defaulting to `storage`, so run titles from that
directory; a run started elsewhere creates a fresh `storage/` beside it:

```sh
ssh <host> 'cd ~/vita2native && ./vita2native games/<TITLEID>/eboot.bin'
```

The binary keeps the name `vita2native`. That no longer collides with anything:
the guest root is `storage/`, not a directory named after the binary. Beds that
still use the older `~/v2n/v2n-arm` layout behave the same way — with no
`launcher.toml` the loader falls back to `storage/` in the working directory,
which is why the scratchvm commands below run from `~/v2n`.

A running binary cannot be overwritten (`text file busy` on ARM). Stop it first,
or upload to a temporary name and move:

```sh
scp build/release/vita2native <host>:~/vita2native/vita2native.new
ssh <host> 'pkill -x vita2native; sleep 1; mv ~/vita2native/vita2native.new ~/vita2native/vita2native'
```

Use `pkill -x`, which matches the process name only. `pkill -f vita2native`
also matches the shell running the command and kills your own SSH session.

## Staging a title from Vita3K

Vita3K decrypts a title the first time it runs it and caches the result beside
the installed copy, in its data directory (macOS:
`~/Library/Application Support/Vita3K/Vita3K/`):

```
fs/ux0/app/<TITLEID>/              installed title: encrypted eboot.bin,
                                   sce_module/, sce_sys/ and the assets
cache/decrypted_selfs/<TITLEID>/   decrypted eboot.bin, sce_module/*.suprx,
                                   sce_sys/about/*.suprx
```

A runnable directory needs the assets with the decrypted binary overlaid,
because the loader reads a decrypted ELF and the app directory's copy is still
encrypted:

```sh
V3K="$HOME/Library/Application Support/Vita3K/Vita3K"
ID=<TITLEID>
mkdir -p /tmp/stage-$ID
cp -R  "$V3K/fs/ux0/app/$ID" /tmp/stage-$ID/
cp -f  "$V3K/cache/decrypted_selfs/$ID/eboot.bin" /tmp/stage-$ID/$ID/
cp -Rf "$V3K/cache/decrypted_selfs/$ID/sce_module/." /tmp/stage-$ID/$ID/sce_module/
cp -Rf "$V3K/cache/decrypted_selfs/$ID/sce_sys/."    /tmp/stage-$ID/$ID/sce_sys/
```

Then copy the title into the install directory's `games/`:
`<host>:~/vita2native/games/<TITLEID>/`, so `eboot.bin` sits at
`~/vita2native/games/<TITLEID>/eboot.bin`. Beds still on the older layout use
`~/vita-games/<TITLEID>/` instead, as the scratchvm examples below do.

Notes that save time:

- `decrypted_selfs/<ID>` alone is not enough — it is only the executable and its
  modules (6 MB for Shakedown), while the assets are the bulk (125 MB).
- The directory name is the title ID and doubles as the savedata namespace
  (`host_fs_title_id()`), so do not rename it to the game's name.
- `sce_sys/about/right.suprx` comes from the decrypted tree as well; copying the
  whole decrypted `sce_sys` over the app's one covers it.
- If `fs/ux0/app/<ID>` is missing, the title is not installed in that Vita3K
  profile — check the GUI's game list rather than assuming a path.
- A decrypted `eboot.bin` starts with `SCE\0` followed by `0x000000c0` at
  offset 8; `0x00000540` means it is still encrypted and will not load.
- macOS `tar` ships AppleDouble `._*` files. Strip them, or use `rsync -a
  --exclude '._*'`, or `tar --disable-copyfile`.
- The stage dir already contains the title-ID directory, so copying it with a
  trailing slash into `games/<TITLEID>/` doubles the nesting. Move the inner
  directory up afterwards.

## Test beds

### scratchvm

x86_64 Ubuntu 24.04, qemu-user-static, **armhf multiarch**, no display and no
display manager — the VNC kiosk is the only output path. This is where the
ARM32 binary is exercised without real hardware.

```sh
scp build/release/vita2native scratchvm:~/v2n/v2n-arm
ssh scratchvm 'cd ~/v2n && ~/.local/bin/sway-vnc-kiosk run \
  qemu-arm-static -L /usr/arm-linux-gnueabihf ./v2n-arm \
  ~/vita-games/<TITLEID>/eboot.bin'
```

- **`sway-vnc-kiosk` is the display**: `~/.local/bin/sway-vnc-kiosk run <cmd>`
  injects `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY=wayland-kiosk`. It is a user
  service (`systemctl --user status sway-vnc-kiosk`) running sway on the wlroots
  headless backend with the GLES2 renderer, which is the host's GPU here, plus
  wayvnc, 640x480. The script is not on `PATH` for non-interactive SSH — call it
  by full path. It replaced the Weston kiosk on this bed; `sudo
  tools/sway-vnc-kiosk install --user <name> --no-auth` recreates it. Without
  auth, anything that can reach port 5901 gets the keyboard and pointer.
- **`virpipe` gives the guest a real GPU.** Wrap the binary with it and GL goes
  through VirGL to the host's Intel GPU instead of llvmpipe — measured at ~55 fps
  against ~1.6 fps, so it is the only way to tell a rendering bug from
  software-rasteriser slowness:

  ```sh
  ssh scratchvm 'cd ~/v2n && ~/.local/bin/sway-vnc-kiosk run \
    virpipe ./v2n-arm ~/vita-games/<TITLEID>/eboot.bin'
  ```

  One `virgl_test_server` backs every client: it is a user unit (`systemctl
  --user status virpipe`) listening on `$XDG_RUNTIME_DIR/virpipe/vtest.sock`,
  installed with `sudo tools/virpipe install --user <name>`. `virpipe run <cmd>`
  only sets `GALLIUM_DRIVER=virpipe` and `VTEST_SOCKET_NAME` and execs, so
  concurrent titles share the endpoint; `virpipe diagnose` probes it with
  `eglinfo` and reports which renderer wins. The unit starts the server in its
  default forking mode: a single-serving server (`--no-fork`) blocks the second
  vtest connection, and vita2native opens one per guest thread that gets a
  context — it hangs inside `eglInitialize` with no error. Restart the unit after
  a Mesa upgrade, or clients keep talking to the `virglrenderer` it started with.
- Everything still runs under `qemu-arm-static`, so absolute CPU speed is
  meaningless. With `virpipe` the GL side is not emulated, so frame-rate
  comparisons between code changes are meaningful there; they are not against
  the real handhelds.
- It cannot validate KMS/DRM: the i915 device has no matching armhf Mesa
  driver, SDL aborts kmsdrm init, and `DRM_IOCTL_SET_MASTER` needs the user in
  the `video`/`render` groups. Confirm no stray process holds DRM master
  (`cat /sys/kernel/debug/dri/0/clients`) before blaming the code — a blocked
  `head -c 4 /dev/dri/card0` will hold it.

### vm (this Mac, port-forwarded)

aarch64 Ubuntu 22.04 at `127.0.0.1:2222` (host alias `vm`). Useful for native
aarch64 builds and quick sanity checks; it has no armhf userland, so it cannot
run the ARM32 artifact.

### Orange Pi Zero 3

2 GB Allwinner H618 with Mali-G31 (Panfrost), `ssh opiz3`. Runs
`tools/sway-vnc-kiosk` (installed in `~/.local/bin`): sway on the wlroots
**headless** backend with `WLR_RENDERER=gles2`, plus wayvnc on 640x480. Run
titles with `sway-vnc-kiosk run …` so the Wayland environment is set. The wrapper
is not on `PATH` for a non-interactive shell, and the loader wants the working
directory that holds `launcher.toml`:

```sh
ssh opiz3 'cd ~/vita2native && ~/.local/bin/sway-vnc-kiosk run \
  ./vita2native games/<TITLEID>/eboot.bin'
```

- **This is the only way to get the GPU headlessly on this board.** Weston
  13's `vnc-backend` and `rdp-backend` are pixman-only: `--backend=vnc
  --renderer=gl` initialises the GL renderer on Mali and then dies with
  "failed to create compositor backend", so the guest fell back to llvmpipe and
  ran at 9-10 fps. Under sway/GLES2 the guest reports `Mali-G31 (Panfrost)` and
  holds 57-59 fps at 640x480.
- The headless backend has no DRM fd (`Failed to get backend DRM FD`), so the
  compositor's last hop to wayvnc is shm: one copy per frame on top of the GPU
  render. A real KMS output — a dummy HDMI plug, or `video=HDMI-A-1:640x480@60e`
  on the kernel cmdline — would make that hop zero-copy.
- Settings live in `~/.config/sway-vnc-kiosk/` (`sway.conf`, `wayvnc.conf`).
  The generated `wayvnc.conf` is `address`/`port` only, no auth. Never add
  `max_fps` to it: wayvnc 0.7.2 double-frees on that key. `grim` captures the
  composited output, which is the only way to see the presentation border —
  the guest's own frame dump does not include it.
- **This board hard-hangs on asset-heavy loads** (SSH banner times out, ping and
  TCP still fine, needs a power cycle). See `ISSUES.md` I-15: run with
  `VITA2NATIVE_DEBUG_FINISH_PER_FRAME=1`, a cgroup memory cap, and logs on disk rather
  than tmpfs. Do not assume a `dummy`-driver run is cheap — the guest still
  loads and decodes every asset.

### R36S

Rockchip RK3326 with Mali-G31 through the vendor blob, 640x480 DSI panel,
`ssh r36s`. Runs **sway**, so it is the first GPU-composited Wayland target and
the intended test bed for KMS/DRM presentation (`ISSUES.md` I-10). Deploy the
binary as described under *Staging the binary*.

## Rendering traps

- **A black panel with a healthy fps counter means every draw is being dropped.**
  Set `VITA2NATIVE_DEBUG_GL_CHECK=1`: `GL error 0x506` is
  `GL_INVALID_FRAMEBUFFER_OPERATION`, and GL silently discards the draw.
  `gxm_gl_check()` names the cause:

  ```
  SceGxm: GL error 0x506 at draw (fbo 1 status 0x8cd7)
  ```

  `0x8cd5` is `GL_FRAMEBUFFER_COMPLETE`; `0x8cd7` is
  `GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT`.

- **A framebuffer's attachment list may not follow its name across threads** on
  some vendor GLES blobs. A title creating its render target on one thread and
  drawing from another sees the same framebuffer with *no attachments* and every
  draw rejected, while still presenting at full rate. Shakedown: Hawaii did this
  (created on `<main>`, drew from `thread_render`): 4964 rejected draws, black
  panel, 60 fps. `sceGxmBeginScene` re-attaches colour/depth when the drawing
  thread changes (`SceGxmRenderTarget.attached_thread`).
  Angry Birds Trilogy never hits it — "works on title X" proves nothing here.

- **The `.dsm` shader dump is guest GXP disassembly, not what reaches GL.**
  `VITA2NATIVE_DEBUG_DUMP_SHADERS=<dir>` writes `.dsm` plus the translated
  `.vert`/`.frag`. Read the GLSL when a draw is black or wrong-coloured. It only
  writes on a cache *miss*, so clear `vita2native/cache/shaders` first.

- **Validate a diagnostic knob before trusting a negative result.** Three that
  silently did nothing: `VITA2NATIVE_GFX_NO_FB_FETCH` (the cache is read before the
  flag, so clear it), `VITA2NATIVE_DEBUG_FORCE_COLOR` (needs a `_Color` uniform) and
  `VITA2NATIVE_DEBUG_TEXCOLOR` (needs `fragTex_tex` and `v_TexCoord0`).
  `VITA2NATIVE_DEBUG_PROGRAM_COLORS` does work generally — use it to prove
  fragment output reaches the framebuffer at all.
