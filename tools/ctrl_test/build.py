#!/usr/bin/env python3
"""Build the guest-side controller mapping probe.

The output is a fixed-address ET_SCE_EXEC that vita2native can load directly:

    python3 tools/ctrl_test/build.py
    ./v2n-arm build/ctrl-test/ctrl_test.elf

Zig is used as the ARM compiler; no VitaSDK installation is required.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

ET_SCE_EXEC = 0xFE00
EM_ARM = 40
PT_LOAD = 1
RX_BASE = 0x81000000
RW_BASE = 0x81010000
MODULE_NAME = b"v2n-ctrl-test\0"


def fail(message):
    print(f"ctrl_test: {message}", file=sys.stderr)
    return 1


def parse_phdrs(data):
    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phentsize = struct.unpack_from("<H", data, 42)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]

    if e_phentsize != 32:
        raise ValueError(f"unexpected program header size {e_phentsize}")

    phdrs = []
    for i in range(e_phnum):
        offset = e_phoff + i * e_phentsize
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = (
            struct.unpack_from("<IIIIIIII", data, offset)
        )
        phdrs.append(
            {
                "type": p_type,
                "offset": p_offset,
                "vaddr": p_vaddr,
                "filesz": p_filesz,
                "memsz": p_memsz,
                "flags": p_flags,
                "align": p_align,
            }
        )
    return phdrs


def main():
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    here = os.path.dirname(os.path.abspath(__file__))

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output_dir",
        nargs="?",
        default=os.path.join(repo, "build", "ctrl-test"),
        help="directory for ctrl_test.elf (default: build/ctrl-test)",
    )
    args = parser.parse_args()

    zig = os.environ.get("ZIG") or shutil.which("zig")
    if not zig:
        return fail("zig not found (set ZIG=/path/to/zig)")

    os.makedirs(args.output_dir, exist_ok=True)
    output = os.path.join(args.output_dir, "ctrl_test.elf")

    command = [
        zig,
        "cc",
        "-target",
        "arm-linux-gnueabihf",
        "-mcpu=cortex_a9",
        "-marm",
        "-mfloat-abi=hard",
        "-O2",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-sanitize=all",
        "-fno-pic",
        "-fno-PIE",
        "-fno-stack-protector",
        "-fno-asynchronous-unwind-tables",
        "-fno-unwind-tables",
        "-nostdlib",
        "-static",
        "-no-pie",
        "-I",
        os.path.join(repo, "external", "font8x8"),
        f"-Wl,-T,{os.path.join(here, 'ctrl_test.ld')}",
        "-Wl,-e,module_start",
        "-Wl,--build-id=none",
        "-Wl,-z,max-page-size=0x1000",
        "-o",
        output,
        os.path.join(here, "ctrl_test.c"),
        os.path.join(here, "ctrl_test.S"),
    ]

    print("ctrl_test: " + " ".join(command))
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode

    with open(output, "rb") as f:
        data = bytearray(f.read())

    if data[0:4] != b"\x7fELF":
        return fail("linker did not produce an ELF")
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != EM_ARM:
        return fail(f"unexpected e_machine {e_machine}")

    phdrs = parse_phdrs(data)
    loads = [ph for ph in phdrs if ph["type"] == PT_LOAD]
    if not loads:
        return fail("no PT_LOAD segments")
    if loads[0]["vaddr"] != RX_BASE:
        return fail(f"first PT_LOAD is at 0x{loads[0]['vaddr']:08X}, expected 0x{RX_BASE:08X}")

    rx = loads[0]
    module_file_offset = rx["offset"]
    if data[module_file_offset + 4 : module_file_offset + 4 + len(MODULE_NAME)] != MODULE_NAME:
        return fail("module info is not at the start of the RX segment")

    import_top = struct.unpack_from("<I", data, module_file_offset + 0x2C)[0]
    import_end = struct.unpack_from("<I", data, module_file_offset + 0x30)[0]
    module_start = struct.unpack_from("<I", data, module_file_offset + 0x44)[0]
    if not (0 < import_top < import_end <= rx["filesz"]):
        return fail(f"bad import range 0x{import_top:X}..0x{import_end:X}")
    if not (0 < module_start < rx["filesz"]):
        return fail(f"bad module_start offset 0x{module_start:X}")

    # The loader expects e_entry to encode (segment index << 30) | offset. The
    # module info lives at offset zero of segment zero.
    struct.pack_into("<H", data, 16, ET_SCE_EXEC)
    struct.pack_into("<I", data, 24, 0)
    struct.pack_into("<I", data, 36, 0x05000000)  # EABI version 5

    with open(output, "wb") as f:
        f.write(data)

    print(f"ctrl_test: wrote {output}")
    print(
        "ctrl_test: RX 0x%08X (%u bytes), RW 0x%08X (%u bytes), "
        "module_start +0x%X, imports +0x%X..+0x%X"
        % (
            rx["vaddr"],
            rx["memsz"],
            loads[1]["vaddr"] if len(loads) > 1 else 0,
            loads[1]["memsz"] if len(loads) > 1 else 0,
            module_start,
            import_top,
            import_end,
        )
    )
    print(f"ctrl_test: run with: v2n-arm {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
