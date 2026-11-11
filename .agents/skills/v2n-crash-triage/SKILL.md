---
name: v2n-crash-triage
description: Diagnose a vita2native crash, abort or silent exit on a test device (Orange Pi Zero 3, R36S) — read the preserved crash log, trace the abort caller, attach gdb to the live guest, disassemble Thumb guest code and decide between a missing Sce API and a title-side bug.
---

# Triaging a vita2native crash

Use this when a title dies, aborts, hangs or exits silently on a device. All
commands assume the repo root as the working directory.

## 1. Get the evidence

The kiosk restarts on exit and truncates `vnc-game.log`, so a real fault is
preserved as `crash-<stamp>-<status>.log` (and `core-<stamp>` when
`V2N_CRASH_CORE=1`). Only two things are treated as a fault by the launcher:

- `vita2native: guest signal <n> at pc=... lr=... sp=... fault=...`
- `sceKernelCallAbortHandler called`

```sh
tools/v2n crash        # newest crash, the run-up to the fatal line
tools/v2n crash -a     # whole file
tools/v2n log 200      # live run instead
```

Everything after the fatal line in that log is the register dump and
`/proc/self/maps`; the run-up is what matters.

If nothing was preserved, the title exited cleanly: look for a guest-side
`exit`/`abort` reason in the tail of the live log and check for `Could not
resolve NID` lines first.

## 2. Classify the fault

| Log evidence | Meaning | Next |
| --- | --- | --- |
| `guest signal 11`, `pc` in `0x81000000-0xc0000000` | segfault inside guest (JIT) code | step 3 |
| `fault=` is a small address (`0x0`, `0xc`, `0x8`) | null/dangling guest pointer | step 3, inspect registers |
| `guest signal 4` (SIGILL) | mis-decoded instruction or a stub overwritten | check `Could not resolve NID` |
| `sceKernelCallAbortHandler called` | guest called abort (assert, C++ exception, Mono) | step 4 |
| `Could not resolve NID 0x... (stub at 0x...), export not found!` | missing Sce API | step 5 |

An unresolved NID almost always ends in a *later* SIGSEGV, so grep for it
before blaming the faulting instruction.

## 3. Locate the faulting guest address

The mapping lines give the module: the JIT code segment is the `rwxp` mapping
around `0x81000000`; `pc - mapping_start` is the offset into the main image
(`VITA2NATIVE_DUMP_MAIN_CODE=<path>` writes that image with relocation applied,
so `arm-none-eabi-objdump -D -b binary -marm` or `radare2` can be pointed at the
offset).

Live inspection beats a core dump:

```sh
tools/v2n gdb          # scp tools/gdb/live-attach.gdb, attach, wait for the signal
```

Read from the same session:

```
(gdb) set arm force-mode thumb      # guest code is Thumb-2; without this x/i decodes garbage
(gdb) x/16i $pc-16
(gdb) info registers
```

Guest return addresses are Thumb `BL` words on the host stack: odd 32-bit words
in `0x90000000-0xc0000000`. Scan words from `$sp` upward. The run-up log lines
(`sceKernelAllocMemBlock: "basic" -> ...`) tell you which guest block a pointer
came from, which is usually enough to tell a bad index from a freed block.

`VITA2NATIVE_CRASH_CODE_DUMP=<file>` dumps 4 MB from the start of the mapping
that contains `pc`; `VITA2NATIVE_CRASH_DUMP=<offset[,offset...]>` dumps 64 bytes
at image offsets; `VITA2NATIVE_CRASH_DUMP_ARRAY=<sp-offset>` prints the length
word of a guest array pointer on the stack (the Mono "bad array" pattern).

## 4. Trace an abort

Rerun with the abort tracer and read the tail:

```sh
VITA2NATIVE_TRACE_ABORT=1 tools/v2n log 400
```

It prints `vita2native: abort caller[<i>] = 0x...` for odd words in the JIT range
found on the host stack — i.e. guest return addresses that placed the abort.
Decrement by one and subtract the mapping start to get the image offset, then
disassemble as in step 3.

`sceKernelCallAbortHandler` is also where a Mono title reports an unhandled
`System::Exception`; the exception class name is usually in the lines just
before the abort.

## 5. Resolve a missing NID

`external/vita-headers/db/360/` maps NID to name:

```sh
grep -rn "0xAABBCCDD" external/vita-headers/db/360/
```

gives `sceSomething: 0xAABBCCDD` and the library file it belongs to. Then:

1. Check `include/modules/` and `source/modules/` for the module; a partly
   implemented module is the common case.
2. Implement the export with the same `vita2native_export` registration as its
   neighbours, or, if the title only queries it, make the stub succeed with a
   plausible value instead of failing.
3. `VITA2NATIVE_TRACE_IMPORTS=1` logs every resolution — very noisy for a big
   title, so only use it to confirm the stub address a guest PC belongs to.

## 6. Decide: missing API or title bug

- The fault is *inside* a stub's caller, right after a call that returned
  `SCE_KERNEL_ERROR_MODULEMGR_NO_FUNC_NID` or a plausible-looking success:
  missing API. Implement it.
- The fault is a null dereference on data the emulator handed out (a buffer from
  `sceKernelAllocMemBlock`, a `sceGxm` pointer, a file handle): look for the
  emulator returning a value the guest did not expect (size, alignment, struct
  layout, error code).
- The guest does the same thing on hardware-invalid state: title bug or DRM.
  Do not paper over it in the loader.
- Touch-driven engines may never call `SceTouch` and use `SceSystemGesture`
  instead; "no input" is not a crash but looks like one when the title sits on a
  splash screen.

## Pitfalls

- A core dump is 400-800 MB and usually cannot read the `memfd:vita2native-jit`
  region, so gdb on a core shows no guest stack. Prefer `tools/v2n gdb`.
- `tools/v2n clean cores` matters: the root filesystem on the Orange Pi is 15 GB
  and cores fill it fast.
- The guest is Thumb everywhere; without `set arm force-mode thumb` the
  disassembly is nonsense and you will chase a phantom instruction.
- `file` reports a Vita `eboot.bin` as `data`. Check the SELF header word at
  offset 8: `0x...00c0` is decrypted, `0x...0540` is still encrypted (see the
  v2n-run-title skill).
- After a crash the kiosk relaunches the title within a couple of seconds, so
  attach first and reproduce second.
