# AGENTS

Notes for agents working on this repository itself. Everything here is about
the emulator's behaviour and the debugging judgement that goes with it; there
are no per-machine build, deploy or run instructions.

## Rendering traps

- **A black panel with a healthy fps counter means every draw is being
  dropped, not that the guest is slow.** Set `VITA2NATIVE_GL_CHECK=1` and read
  the log. `GL error 0x506` is `GL_INVALID_FRAMEBUFFER_OPERATION`, and GL
  silently discards the draw that triggered it.

  `gxm_gl_check()` reports the bound framebuffer and its status for that error,
  which is what names the cause:

  ```
  SceGxm: GL error 0x506 at draw (fbo 1 status 0x8cd7)
  ```

  `0x8cd5` is `GL_FRAMEBUFFER_COMPLETE`; `0x8cd7` is
  `GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT`.

- **A framebuffer's attachment list does not follow its name across threads**
  on some vendor GLES blobs. A title that creates its render target on one
  thread and draws from another sees the same framebuffer come back with *no
  attachments* and every draw rejected — while presenting at full frame rate.
  Shakedown: Hawaii did exactly this (created on `<main>`, drew from
  `thread_render`): 4964 rejected draws, black panel, 60 fps.

  `sceGxmBeginScene` now re-attaches the colour/depth objects whenever the
  drawing thread changes, tracked in `SceGxmRenderTarget.attached_thread`. If
  a title renders black with `0x8cd7` at draw time, check that re-attach is
  still in place before looking anywhere else.

  Angry Birds Trilogy never hits this because it creates and uses its render
  targets on one thread — which is why "it works on title X" proves nothing
  about the thread handling.

- **The `.dsm` shader dump is guest GXP disassembly, not what reaches GL.**
  `VITA2NATIVE_GXM_SHADER_DUMP=<dir>` writes both: `.dsm` plus the translated
  `.vert`/`.frag`. Read the GLSL when a draw renders black or wrong-coloured.
  Note the dump only writes on a cache *miss*, so clear `vita2native/cache/shaders`
  first.

- **Validate a diagnostic knob before trusting a negative result.** Three that
  silently did nothing and produced false "ruled out" conclusions:
  - `VITA2NATIVE_NO_FB_FETCH` — the shader cache is read *before* the flag is
    consulted, so cached shaders are reused. Clear the cache to test it.
  - `VITA2NATIVE_FORCE_COLOR` — needs a `_Color` uniform in the program.
  - `VITA2NATIVE_DEBUG_TEXCOLOR` — needs `fragTex_tex` and `v_TexCoord0` in the
    GLSL.

  `VITA2NATIVE_DEBUG_PROGRAM_COLORS` does work generally (each fragment program
  gets a flat colour), which makes it the one to use to prove fragment output
  reaches the framebuffer at all. Run the knob against a title known to render
  and check the frame dump changed before concluding anything.

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
  offset 8; `0x00000540` means it is still encrypted and vita2native will not
  load it.
