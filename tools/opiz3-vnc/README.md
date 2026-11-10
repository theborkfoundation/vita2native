# Orange Pi Zero 3: TigerVNC kiosk

Runs the game directly in a bare Xtigervnc session — there is no desktop
environment and no display manager on this image. The game is the only client,
so the VNC screen is exactly the 960x544 game window. Chasm (PCSE01031) is the
title currently configured.

At boot `tigervncserver@:1.service` starts the session, `~/.vnc/xstartup`
sources `~/.vnc/v2n.conf`, and the game runs. Restarting the unit relaunches
the game (`Restart=always` in `/etc/systemd/system/tigervncserver@.service.d/`).

```sh
sudo systemctl restart tigervncserver@:1.service
sudo systemctl stop tigervncserver@:1.service
```

## Crashes

`xstartup` copies the log and the core (when `V2N_CRASH_CORE=1` in
`v2n.conf`) to timestamped files before the session restarts, so a crash is
not lost to the next run:

```sh
ls -lt ~/v2n-run/crash-*.log ~/v2n-run/core-*
```

## Server

```sh
# one-time
cp tools/opiz3-vnc/xstartup ~/.vnc/xstartup
cp tools/opiz3-vnc/v2n.conf.example ~/.vnc/v2n.conf
chmod 755 ~/.vnc/xstartup
printf 'v2n-pass\n' | vncpasswd -f > ~/.vnc/passwd
chmod 600 ~/.vnc/passwd
printf 'geometry=960x544\ndepth=24\nlocalhost=no\n' > ~/.vnc/config

# start/stop
vncserver :1
vncserver -kill :1
```

The VNC session starts the game directly. Remove `session=xfce` from
`~/.vnc/config` if it is present; otherwise TigerVNC ignores `xstartup`.

Paths and debug knobs live in `~/.vnc/v2n.conf`; `xstartup` only sources it.
Edit that file and restart the session, not `xstartup`. The SDL window follows
the `geometry` in `~/.vnc/config` unless `V2N_WINDOW_SIZE` is set.

## Client (macOS)

Open `vnc://192.168.1.202:5901` (Finder → Go → Connect to Server) or:

```sh
open vnc://192.168.1.202:5901
```

Password: `v2n-pass`.

In Screen Sharing, use **View → Show in a window** and turn off **Scale to fit**
so the 960x544 frame is displayed 1:1 and stays sharp.

## Notes

- The OPi renders offscreen with Panfrost, then SDL copies the frame into the
  virtual display. The guest-side readback, not VNC, is the bottleneck: at
  native scale Chasm runs at about 3.5 fps; `V2N_RENDER_SCALE=0.6667` is faster
  (about 5.5 fps) but blurs the small font, so `1` is the readable setting.
- Set `V2N_FPS_OVERLAY=1` to draw the FPS graph in the top-right corner;
  `V2N_FPS=1` only writes FPS lines to the log.
- VNC has no audio. The game writes to the PulseAudio `v2n` null sink.
- Mono (the PSM port) builds save paths with a Windows `\` separator, e.g.
  `savedata0:Chasm/Saves\`. `host_fs_resolve_path` normalises `\` to `/`;
  without that the save enumeration fails and the game aborts with an
  unhandled `System::Exception`.
- The launcher kills its child when the session is stopped; otherwise the game
  is reparented into the login session scope and keeps running after the X
  server is gone.
- To keep port 5901 off the LAN, set `localhost=yes` in `~/.vnc/config` and use
  `ssh -N -L 5901:127.0.0.1:5901 opiz3`, then connect to `vnc://localhost:5901`.
