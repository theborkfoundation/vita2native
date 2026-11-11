---
name: v2n-run-title
description: Bring up a new PS Vita title on a vita2native test device — take the decrypted SELF from the Vita3K cache, stage it with its game data, switch the kiosk to it, and run the first-boot checklist (unresolved NIDs, aborts, black screen, saves, input).
---

# Running a new title

Use this when the user names a title or a title ID that has to be tried on a
device. Test devices: `opiz3` (Orange Pi Zero 3, TigerVNC kiosk) and `r36s`
(R36S/ArkOS, offline most of the time).

## 1. Find a decrypted SELF

Vita3K decrypts a title the first time it runs it and caches the result:

```
~/Library/Application Support/Vita3K/Vita3K/cache/decrypted_selfs/<ID>/
    eboot.bin  sce_module/  sce_sys/
~/Library/Application Support/Vita3K/Vita3K/fs/ux0/app/<ID>/       # game data
```

Check the SELF is really decrypted — a Vita `eboot.bin` looks like opaque `data`
to `file`:

```sh
od -A d -t x4 -N 16 <path>/eboot.bin
# 0000000 00454353 00000003 000100c0 ...   <- 0x00c0: decrypted
# 0000000 00454353 00000003 00010540 ...   <- 0x0540: still encrypted
```

If it is encrypted, run the title once in Vita3K (or ask the user to) and re-check
before staging: vita2native only loads decrypted SELF/ELF.

## 2. Stage it

The staged tree is the title's own layout — the decrypted `eboot.bin`,
`sce_module/`, `sce_sys/` plus whatever data the title ships:

```sh
ID=PCSE01177
scp -r <decrypted_selfs>/$ID opiz3:/home/bachnx/tests/apps/
scp -r <ux0/app>/$ID/. opiz3:/home/bachnx/tests/apps/$ID/     # data, if present
```

`tools/v2n title` with no argument lists exactly what is staged, so a missing
directory shows up immediately. Titles that read save data need `savedata0:` to
resolve under the run directory; a fresh title therefore logs a few
`sceIoOpen savedata0:... failed` lines on first boot, which is expected.

## 3. Run it

```sh
tools/v2n title $ID        # writes V2N_APP into ~/.vnc/v2n.conf, restarts the kiosk
tools/v2n shot first-boot
tools/v2n log 60
```

On the R36S there is no kiosk: `tools/v2n deploy r36s` prints the login-shell
command, EmulationStation has to be stopped for the direct-KMS path, and the
profile is what supplies the Mali EGL environment.

## 4. First-boot checklist

Read the log in this order; each item is a different kind of problem.

1. **Unresolved imports.** `Could not resolve NID 0x… (stub at 0x…), export not
   found!` — the title called an API that does not exist. Resolve the name with
   `grep -rn "0xNID" external/vita-headers/db/360/`. This is the most common
   first-boot failure and usually shows up as a crash seconds later.
2. **Loader refusals.** Anything about the SELF, segments, relocations or a
   missing `sce_module` — staging problem, not an emulator bug.
3. **`PostInitApplication` / `main` abort.** If the guest aborts immediately,
   use the v2n-crash-triage skill (`VITA2NATIVE_TRACE_ABORT=1`, `tools/v2n gdb`).
4. **Black screen but sound.** Render path — use the v2n-render-debug skill. A
   quick check is `tools/v2n dump-frame 240`: if the guest framebuffer has
   pixels, the title is fine and the presenter is not.
5. **Input does nothing.** A tap shorter than one frame is dropped at 4-20 fps:
   hold for at least 0.8 s (`tools/v2n drive click X Y 1.2`). Touch-driven
   engines may ignore `SceTouch` and use `SceSystemGesture` instead — check with
   `VITA2NATIVE_INPUT_DEBUG=1` before assuming a coordinate is wrong.
6. **Save data.** `savedata0:` maps under the run directory. Mono/PSM titles
   build paths with a Windows separator (`savedata0:<Title>/Saves\`); the
   loader's `host_fs_resolve_path` normalises `\` to `/`, and without it the
   save enumeration fails with an unhandled `System::Exception`.
7. **Performance.** Expect a few fps for 3D titles; `V2N_RENDER_SCALE=1` keeps
   text readable, `0.6667` is faster but blurs fonts. Do not report a title as
   broken because it is slow on llvmpipe.

## 5. Recording the result

- Keep the previous config: `tools/v2n title` copies `~/.vnc/v2n.conf` to
  `v2n.conf.bak` before switching.
- Note the outcome in `PLAN.md` (it is gitignored and is the running log): title
  ID, what broke, which env var or commit fixed it.
- Commit emulator fixes separately from staging changes; device trees and
  titles are not in the repository.

## Pitfalls

- Keyboard→pad mapping for the kiosk: `Z`/`Return` Cross, `X`/`BackSpace`
  Circle, `A` Square, `S` Triangle, arrows D-pad, `Q`/`E` L1/R1.
- `V2N_INPUT_DEBUG=1` also turns on `NP` and save tracing and floods the log;
  turn it off in `~/.vnc/v2n.conf` once input is understood.
- The kiosk relaunches on exit, so a title that quits instantly looks like a
  boot loop; `tools/v2n log` shows the repeated banner.
- `tools/v2n clean cores` after a crash: cores are 400-500 MB each and the
  device root filesystem is small.
