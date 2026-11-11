---
name: v2n-render-debug
description: Debug vita2native rendering on a test device — wrong or flipped orientation, black or missing geometry, wrong colours, blank window, or slow frame rate. Covers frame dumps, viewport/clip traces, texture and draw probes, and the headless-vs-window pitfall.
---

# Debugging the render path

Use this when a title boots and makes sound but the picture is wrong: black,
upside down, mirrored, scaled, clipped, one colour, or too slow.

## 1. Establish what the guest actually presented

The deployment build is **HEADLESS=ON**. Presentation therefore goes through
`source/modules/SceDisplay_null.c` -> `source/host/host_video_sdl.c`;
`SceDisplay_linux.c` and `gxm_direct_present_enabled()`
(`host_display_uses_kms()`) are not part of this build. Do not debug the
present path in `SceDisplay_linux.c`.

```sh
tools/v2n log 40          # "SceDisplay: N frames in 1.0 s (..fps)" proves swap works
tools/v2n shot now        # what the window shows
tools/v2n dump-frame 240  # what the guest framebuffer holds, independent of the window
```

If `dump-frame` is correct and `shot` is not, the renderer is fine and the bug is
in the presenter (scale, letterbox, `SDL_UpdateTexture` stride, window size). If
both are wrong, the bug is in the renderer. That split rules out half the search
space before touching code.

`VITA2NATIVE_DUMP_FRAME` only exists in the null display path, i.e. only in the
headless build — it silently does nothing on a desktop/SDL-display build.

## 2. Orientation

Bitmaps captured through the null display are already in the guest's top-down
order: the readback flips GL's bottom-up rows. That matches a title whose
viewport `y_scale` is **negative** (NDC +1 lands at the top of the surface). A
positive `y_scale` asks for the opposite direction and is handled by negating
clip-space Y through `GXMRenderVertUniformBlock.viewport_flip[1]`
(`flip_y` in `gxm_flush_state`). Real values seen so far: Chasm `-272`,
Iconoclasts `+180` on a 640x368 target.

```sh
VITA2NATIVE_TRACE_VIEWPORT=1 tools/v2n log 30
```

prints up to 16 lines of `SceGxm viewport: guest off=… scale=… -> gl x,y WxH (rt WxH)`.
The first line after a scene change is the one to read; a `scale` sign that does
not match the captured flip means the flip decision is wrong.

A vertical flip that appears *only* in the window means the presenter is
flipping again — not the viewport.

## 3. Missing or clipped geometry

- `VITA2NATIVE_TRACE_TEXTURES=1` — texture uploads with format and size. A
  texture with a 1x1 size or a zero-sized upload explains a black sprite.
- `VITA2NATIVE_GXM_FRAME_TRACE=1` — per-frame surface report: render target
  size, draws, clears. Start here when a frame is entirely one colour.
- `VITA2NATIVE_GXM_PROGRAM_TRACE=1` — shader translation and link results.
- `VITA2NATIVE_TRACE_DRAWS=1` — per-draw state (expensive; only for a few
  frames).
- `VITA2NATIVE_DUMP_SHADER_PATH=<dir>` — dump translated GLSL for inspection.
- `VITA2NATIVE_DEBUG_PROGRAM_COLORS=1` — colour every program differently; a
  missing region is then obviously a program that never drew.
- `VITA2NATIVE_DUMP_TEXTURES=<dir>` — dump sampled textures.

A band of background colour at one edge means the region clip rect
(`sceGxmSetRegionClip`) or the viewport is smaller than the surface, not a
missing draw. Compare the `TRACE_VIEWPORT` rect with the render target size.

## 4. Not drawing at all

These force one stage on so the others drop out:

| Variable | Effect |
| --- | --- |
| `VITA2NATIVE_NO_CULL=1` | disable culling; geometry appears if the winding is wrong |
| `VITA2NATIVE_NO_DEPTH=1` | disable depth test/write; content appears if depth is broken |
| `VITA2NATIVE_FORCE_COLOR=1` | ignore textures, draw with the vertex colour |
| `VITA2NATIVE_CLEAR_COLOR=<rrggbb>` | paint the clear colour so "drawn black" and "not drawn" look different |
| `VITA2NATIVE_POISON_UNIFORMS=1` | fill uniforms with a pattern; content appears if a uniform block is stale |

Pixel-level probing on the live frame:

```sh
VITA2NATIVE_PROBE_AFTER=5 VITA2NATIVE_PROBE_PIXEL=50,60 \
VITA2NATIVE_PROBE_TEXDUMP=1 VITA2NATIVE_PROBE_VERTS=1 tools/v2n log 120
```

`PROBE_AFTER=<seconds>` arms the probe (it then covers ~2000 batches),
`PROBE_PIXEL=x,y` picks the sample point (default 50,60), and the `probe f…`
lines report program, shader hashes, sampler type, texture format, vertex data
and the centre pixel.

## 5. Speed

```sh
VITA2NATIVE_FPS=1                       # fps lines in the log
VITA2NATIVE_FPS_OVERLAY=1               # graph in the window's top-right
VITA2NATIVE_METRICS_FILE=/tmp/m.csv     # <ms>,<fps>,<frames> per second
python3 tools/fps_chart.py /tmp/m.csv   # live chart, server on 127.0.0.1:8099
VITA2NATIVE_PROFILE=1 VITA2NATIVE_PROFILE_FRAME=1   # frame budget breakdown
```

The budget lines split `finish`, `read` (readback), `draw`, `ds`, `resolve`,
`convert`. Knobs worth A/B-ing, one at a time:

- `VITA2NATIVE_SKIP_READBACK=1` — size of the readback cost (the frame is not
  presented, so measure, do not judge visuals).
- `VITA2NATIVE_NO_MSAA=1` — 2x MSAA costs roughly 30 ms/frame on a Panfrost
  device.
- `VITA2NATIVE_RENDER_SCALE=0.6667` — scales presentation only; faster but
  blurs small fonts. The kiosk sets it as `V2N_RENDER_SCALE`.
- `VITA2NATIVE_TEXTURE_CACHE_MB=<n>` — the cache is capped at 256 MB by
  default; raising it trades memory for fewer re-uploads.

On the Orange Pi the X server has no DRI3, so anything that renders *through*
X11 EGL resolves to llvmpipe. Guest GLES must stay offscreen (surfaceless
Panfrost) and only presentation may touch SDL — that split is deliberate and
should not be "fixed".

## Pitfalls

- The kiosk restarts the title on every binary deploy; check `v2n log` after
  `v2n deploy` before concluding a change did nothing.
- `VITA2NATIVE_DUMP_FRAME_AT` indexes *presented* frames, not host time, so
  captures are comparable across runs even at 4 fps.
- `tools/img2png.py` reads both the XWD screenshots and the BMP dumps; a BMP
  dump is 24-bit bottom-up, so a reader that assumes top-down shows the frame
  upside down and sends you chasing a flip that is not there.
- A title that renders into an offscreen target and presents it directly
  (Iconoclasts) has its orientation decided by the viewport Y sign, not by the
  readback path.
