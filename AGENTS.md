# AGENTS

## R36S (ArkOS)

- Work from `/home/ark/v2n-run`; the host binary is `v2n-arm`.
- Deploy the normal `build/cmake-arm-rel/vita2native` build. The
  `Linux-Cross-MinSize` preset is for size experiments and rare cases only; do
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
    VITA2NATIVE_KMS_PRESENT=1 VITA2NATIVE_RENDER_SCALE=0.6667 \
    ./v2n-arm PCSE00322/eboot.bin"'
  ```

  A plain `ssh r36s 'command'` runs a non-login shell and does not get this
  environment. `VITA2NATIVE_KMS_PRESENT=1` (or `SDL_VIDEODRIVER=kmsdrm`) is
  also required for a manual run to use the direct-KMS present path.

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
