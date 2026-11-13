# AGENTS

Device work goes through `tools/v2n` (see README): `deploy`, `title`, `shot`,
`drive`, `log`, `crash`, `test`, `dump-frame`, `gdb`, `clean`. The notes below
are the judgement that goes with those commands, not a second copy of them.

## R36S (ArkOS)

- Work from `/home/ark/v2n-run`; the host binary is `v2n-arm`.
- Deploy the normal `build/cmake-arm-rel/vita2native` build (preset `arm-rel`).
  The `arm-minsize` preset is for size experiments and rare cases only; do
  not use it as the default deployment artifact.
- A direct-KMS run needs EmulationStation stopped, otherwise `drmSetMaster`
  fails and direct EGL attach is unavailable:

  ```sh
  sudo systemctl stop emulationstation
  ```

  Restart it when done:

  ```sh
  sudo systemctl start emulationstation
  ```

- The Mali EGL/GLES environment is injected by **login shells**. Run the game
  through one, otherwise `SDL_VIDEO_EGL_DRIVER=libEGL.so` is missing and EGL
  loads Mesa/glvnd instead of the vendor blob. GLES 3.x initialization then
  fails, the null GPU backend is selected, and the screen stays black:

  ```sh
  ssh -tt r36s 'bash -lc "cd /home/ark/v2n-run && \
    SDL_VIDEODRIVER=kmsdrm VITA2NATIVE_RENDER_SCALE=0.6667 \
    ./v2n-arm PCSE00322/eboot.bin"'
  ```

  A plain `ssh r36s 'command'` runs a non-login shell and does not get this
  environment. `SDL_VIDEODRIVER=kmsdrm` lets SDL own the panel and present
  through its own KMSDRM driver, which is what works on this vendor kernel.

- `VITA2NATIVE_RENDER_SCALE` is a **speed/quality** choice, and the right value
  depends on the title's native surface, not on the panel. The panel here is
  640x480, so a title that already draws at 640x368 fits it 1:1 and
  `RENDER_SCALE=1` needs no resampling at all. `0.6667` shrinks that to 427x245
  and then upscales back, which is faster (Iconoclasts: 40 vs 30 fps) but
  resamples at a non-integer ratio (640/427 = 1.50), so some source pixels land
  1 px wide and others 2 px - visible on pixel art. The 960x544 titles the line
  above was written for do want `0.6667` on this panel; check what the title
  actually asks for with `render target WxH (draw WxH)` in the log before
  copying the value.

- `VITA2NATIVE_KMS_PRESENT=1` switches to the raw KMS/DRM presenter, which
  blits the render-target texture straight to the scanout buffer and skips the
  per-frame readback. It is **not** broken on every title: Angry Birds Trilogy
  runs correctly through it, and is much faster for it (14.8 fps with the
  readback vs 27.5 fps with direct present, both a correct picture).

  It has failed on at least one title, though: the VOP accepted every
  `drmModePageFlip` while continuing to scan the buffer installed by
  `drmModeSetCrtc`, so frames advanced in the log (`SceGxm present texture ...`,
  `SceDisplay: N frames in 1.0 s`), `dri/0/summary` showed an ACTIVE plane, and
  the panel stayed on the first (black) frame. Which title that was is not
  recorded here — if you reach for this flag, confirm the picture on the panel
  rather than trusting the log, and fall back to the SDL presenter
  (`SDL_VIDEODRIVER=kmsdrm` alone) if it does not come up.

  Note that dropping the readback *without* direct present is worse than either:
  `VITA2NATIVE_SKIP_READBACK=1` leaves the guest framebuffer stale, so the panel
  shows only the CPU-composited overlays (dialogs, FPS pane) on an old frame.
  It measures the render path (38.9 fps) but is not playable.

- Maximum performance mode uses the ArkOS helper, not raw sysfs:

  ```sh
  sudo perfmax performance
  ```

  It sets the CPU, GPU and DMC governors to `performance`. Do not run `perfmax`
  without an argument; `ondemand` and `powersave` are the other mode names.

- Verify the result:

  ```sh
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  cat /sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu/governor
  cat /sys/devices/platform/dmc/devfreq/dmc/governor
  cat /sys/class/thermal/thermal_zone0/temp
  ```

- Thermal throttling starts around 80 °C: even with `performance` governors the
  kernel caps the CPU at 1.296 GHz (from 1.512) and the GPU at 480/400 MHz (from
  520). Let the device cool for 2-3 minutes before benchmarking; the caps lift
  when the temperature drops.

## Orange Pi Zero 3 (Ubuntu 24.04)

- This image has **no desktop environment and no display manager**; do not try
  to "remove XFCE". The game is shown through a TigerVNC X session that starts
  the game directly (kiosk).
- Work from `/home/bachnx/v2n-run`; the host binary is `v2n-arm`. Titles are
  staged one directory per ID under `/home/bachnx/tests/apps/` and selected with
  `tools/v2n title <ID>` (the kiosk runs whatever `V2N_APP` points at).
- Deploy the normal `build/cmake-arm-rel/vita2native` build with
  `tools/v2n deploy`; it copies the binary and restarts the kiosk unit. The
  `arm-minsize` preset is for size experiments and rare cases only; do not use
  it as the default deployment artifact.

- `~/.vnc/xstartup` sources `~/.vnc/v2n.conf` and exports the runtime
  environment itself; a login shell is not needed. Connect to
  `vnc://<host>:5901`; the SDL window matches the VNC geometry (960x544).
- Log: `/home/bachnx/v2n-run/vnc-game.log`. A crash is copied to
  `crash-<stamp>-<status>.log` (and `core-<stamp>` when `V2N_CRASH_CORE=1`)
  before the session exits.
- The kiosk does not relaunch itself (`Restart=no`). When the game exits or
  crashes the service goes inactive; run the title again with
  `sudo systemctl restart tigervncserver@:1.service`, or `tools/v2n title` /
  `tools/v2n deploy`, which restart it for you.
- `V2N_RENDER_SCALE=1` keeps small fonts readable; `0.6667` is faster but
  blurs them. Heavy 3D titles run in the single digits either way.
- The launcher kills its child when the session stops. Without that the game
  is reparented into the login session scope and survives the X server.
- Mono/PSM titles build paths with a Windows `\` separator
  (`savedata0:<Title>/Saves\`); `host_fs_resolve_path` normalises it to `/`.
  Without it the save enumeration fails and the game aborts with an unhandled
  `System::Exception`.
- `VITA2NATIVE_TRACE_ABORT=1` logs the guest call stack in
  `sceKernelCallAbortHandler`, which is the quickest way to place an abort in
  guest code.
