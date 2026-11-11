# v2n-extract

Turns a PlayStation Vita `.pkg` (plus its `work.bin` license) into the directory
`tools/v2n title <ID>` runs on a device.

```sh
tools/v2n-extract/build.sh                       # host build, once
build/v2n-extract-host/v2n-extract --list game.pkg
build/v2n-extract-host/v2n-extract --pkg game.pkg --work-bin work.bin --out ~/tests/apps
```

## What it does

A Vita package has three encryption layers, and the tool peels them in order:

| Layer | What it is | Key needed |
| --- | --- | --- |
| pkg container | AES-128-CTR over the table of contents and file data | none — three public per-firmware constants |
| PFS | the encrypted filesystem inside modern packages (`sce_pfs/files.db`) | the **license** (`work.bin` → zRIF → klicensee) |
| SELF | `eboot.bin` and `sce_module/*.suprx` | the same klicensee; the per-firmware keys are compiled in |

So there are no keys to supply by hand, but a license is required: without one
the game files stay PFS-encrypted and unusable. Old (non-PFS) packages decrypt
with the container layer alone; the tool still asks for a license because it
cannot tell which files are PFS-managed before decryption.

`--list` needs no license and prints the title, title id, content type and the
table of contents.

## Output

```
<out>/<TITLE_ID>/
    eboot.bin          decrypted SELF (sdk 0x00c0), ready for vita2native
    sce_module/*.suprx decrypted
    sce_sys/...        param.sfo, icon0.png, livearea, manual
    Data/...           game data as the title expects it
```

That is the layout `tools/v2n title <ID>` expects on the device:

```sh
scp -r PCSE00294 opiz3:~/tests/apps/
tools/v2n title PCSE00294
```

## Options

```
--pkg <file>        package to read
--out <dir>         output directory (default: .)
--work-bin <file>   NoNpDrm work.bin license
--zrif <string>     license as a zRIF string, instead of --work-bin
--list              print the contents and exit (no license needed)
--keep-temp         keep the intermediate container extraction
--no-self           stop before SELF decryption
```

## Build

Host-only target: it links OpenSSL 3 and the PFS engine, neither of which
belongs in the 32-bit ARM build.

- CMake 3.16+, a C and C++17 compiler
- OpenSSL 3 (`brew install openssl@3` on macOS; `libssl-dev` on Debian)
- zlib

`build.sh` writes `build/v2n-extract-host/v2n-extract` and points CMake at the
Homebrew OpenSSL keg when it finds one.

## Where the code comes from

- **pkg reader** (`src/pkg_reader.c`) — C, written here; the container layout
  and the three public keys follow [pkg2zip](https://github.com/mmozeiko/pkg2zip)
  (public domain) and Vita3K's `packages/pkg.cpp`.
- **PFS layer** — the vendored `external/Vita3K/external/psvpfstools`
  (TeamMolecule's psvpfstools, GPLv2), called through its `execute()` entry
  point exactly as Vita3K does.
- **SELF layer** — Vita3K's `vita3k/packages/src/sce_utils.cpp` (GPLv2), which
  carries the key store and `decrypt_fself()`. `compat/util/log.h` replaces its
  spdlog logging so only that translation unit is needed.

The emulator already links GPLv2 Vita3K code for the shader recompiler, so this
adds no new licensing obligation; the tool is a host binary and is not part of
the deployed artifact.

## Notes

- The PFS engine prints its own per-file progress (`Decrypted: ...`) on stdout.
- `work.bin` is exactly 512 bytes. `rif2zrif()` needs the stream opened at the
  end (`std::ios::ate`) to size it — a helper here, not the caller, does that.
- The info block's `sfo` offset is absolute, while table-of-contents offsets are
  relative to the data area. pkg2zip and Vita3K disagree only in appearance;
  both are handled.
