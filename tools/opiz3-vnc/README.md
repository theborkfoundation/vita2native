# Orange Pi Zero 3: TigerVNC test bed

Runs Angry Birds Trilogy in a bare Xtigervnc session — no desktop environment.
The game is the only client, so the VNC screen is exactly the 960x544 game
window.

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
  virtual display. The guest-side readback, not VNC, is the bottleneck:
  `V2N_RENDER_SCALE=0.6667` gives about 8 fps, and `1` gives the native
  960x544 image at about 5 fps.
- Set `V2N_FPS_OVERLAY=1` to draw the FPS graph in the top-right corner;
  `V2N_FPS=1` only writes FPS lines to the log.
- VNC has no audio. The game writes to the PulseAudio `v2n` null sink.
- To keep port 5901 off the LAN, set `localhost=yes` in `~/.vnc/config` and use
  `ssh -N -L 5901:127.0.0.1:5901 opiz3`, then connect to `vnc://localhost:5901`.
