# AGENTS

How to build vita2native and get a binary onto a test bed.

**`ISSUES.md` is the issue tracker — read it before starting work and update it
as you go.** It lists every open defect, performance target and planned feature
with a stable `I-nn` id, a location to start from, and the evidence behind it.
It is git-ignored, so it will not appear in `git status`; it exists in the
working tree. What the emulator supports today is in `README.md`.

## Build

```sh
tools/zig-toolchain.sh     # once; writes wrappers into build/
tools/debian-sysroot.py    # once: the armhf sysroot
cmake --preset release
cmake --build --preset release
```

The artifact is `build/release/vita2native`. For on-device debugging use
`debug`, which keeps symbols.

Cross builds link the target's SDL2 from `build/sysroot-armhf`, pinned by
`tools/sysroots/armhf.lock`. `tools/debian-sysroot.py --verify` checks the tree
against its lock.

## Staging the binary

**Copy it as `v2n-arm`, into `~/v2n/` on the test bed:**

```sh
scp build/release/vita2native <host>:~/v2n/v2n-arm
```

Never name it `vita2native`. That name is the runtime's filesystem root
(`VITA2NATIVE_ROOT_PATH` in `include/config.h`), created on first run, so a
binary with that name collides with the guest's `ux0:`/`savedata/` tree.

A running binary cannot be overwritten (`text file busy` on ARM). Stop it first,
or upload to a temporary name and move:

```sh
scp build/release/vita2native <host>:~/v2n/v2n-arm.new
ssh <host> 'pkill -f "[v]2n-arm"; sleep 1; mv ~/v2n/v2n-arm.new ~/v2n/v2n-arm'
```

Use the bracketed `pkill` pattern. `pkill -f "./v2n-arm"`
matches the shell running it and kills your own SSH session.

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

Then copy the title to `<host>:~/vita-games/<TITLEID>/`, so `eboot.bin` sits at
`~/vita-games/<TITLEID>/eboot.bin`.

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
  trailing slash into `~/vita-games/<TITLEID>/` doubles the nesting. Move the
  inner directory up afterwards.

## Test beds

### scratchvm

x86_64 Ubuntu 24.04, qemu-user-static, **armhf multiarch with an X display on
`:0`**. This is where the ARM32 binary is exercised without real hardware.

```sh
scp build/release/vita2native scratchvm:~/v2n/v2n-arm
ssh scratchvm 'cd ~/v2n && DISPLAY=:0 SDL_VIDEODRIVER=x11 \
  qemu-arm-static -L /usr/arm-linux-gnueabihf ./v2n-arm \
  ~/vita-games/<TITLEID>/eboot.bin'
```

- **Weston VNC kiosk** is the normal path now: `~/.local/bin/weston-vnc-kiosk
  run <cmd>` injects `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY=wayland-kiosk`. It
  is a user service (`systemctl --user status weston-vnc-kiosk`), 640x480.
  The script is not on `PATH` for non-interactive SSH — call it by full path.
- Everything runs under `qemu-arm-static`, so absolute speeds are meaningless.
  Use it for correctness and for frame content (`VITA2NATIVE_DUMP_FRAME`), not
  for frame-rate comparisons.
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

2 GB Allwinner H618 with Mali-G31 (Panfrost), `ssh opiz3`. Serves a Weston
Wayland kiosk; run titles with `weston-vnc-kiosk run …` so the Wayland
environment is set.

- Its Weston runs the **Pixman** backend with no dmabuf, so Wayland gives
  llvmpipe, not the GPU. Software `eglSwapBuffers` then dominates the frame
  (measured ~460 ms present against ~4 ms guest). `SDL_VIDEODRIVER=dummy` gets
  Mali-G31, because only the compositor path is software.
- **This board hard-hangs on asset-heavy loads** (SSH banner times out, ping and
  TCP still fine, needs a power cycle). See `ISSUES.md` I-15: run with
  `VITA2NATIVE_FINISH_PER_FRAME=1`, a cgroup memory cap, and logs on disk rather
  than tmpfs. Do not assume a `dummy`-driver run is cheap — the guest still
  loads and decodes every asset.

### R36S

Rockchip RK3326 with Mali-G31 through the vendor blob, 640x480 DSI panel,
`ssh r36s`. Runs **sway**, so it is the first GPU-composited Wayland target and
the intended test bed for KMS/DRM presentation (`ISSUES.md` I-10). Deploy the
binary to `~/v2n/v2n-arm`.

## Rendering traps

- **A black panel with a healthy fps counter means every draw is being dropped.**
  Set `VITA2NATIVE_GL_CHECK=1`: `GL error 0x506` is
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
  `VITA2NATIVE_GXM_SHADER_DUMP=<dir>` writes `.dsm` plus the translated
  `.vert`/`.frag`. Read the GLSL when a draw is black or wrong-coloured. It only
  writes on a cache *miss*, so clear `vita2native/cache/shaders` first.

- **Validate a diagnostic knob before trusting a negative result.** Three that
  silently did nothing: `VITA2NATIVE_NO_FB_FETCH` (the cache is read before the
  flag, so clear it), `VITA2NATIVE_FORCE_COLOR` (needs a `_Color` uniform) and
  `VITA2NATIVE_DEBUG_TEXCOLOR` (needs `fragTex_tex` and `v_TexCoord0`).
  `VITA2NATIVE_DEBUG_PROGRAM_COLORS` does work generally — use it to prove
  fragment output reaches the framebuffer at all.
