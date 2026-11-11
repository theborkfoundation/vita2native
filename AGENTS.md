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

  Do **not** set `VITA2NATIVE_KMS_PRESENT=1` here. That switches to the raw
  KMS/DRM presenter, and on the R36S's vendor kernel the VOP accepts every
  `drmModePageFlip` while continuing to scan the buffer installed by
  `drmModeSetCrtc` — frames advance in the log (`SceGxm present texture ...`,
  `SceDisplay: N frames in 1.0 s`), `dri/0/summary` shows an ACTIVE plane, and
  the panel still shows the first (black) frame. Raw KMS is opt-in and stays
  off by default for that reason.

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
  before the kiosk restarts.
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
