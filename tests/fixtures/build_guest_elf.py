#!/usr/bin/env python3
"""Generate minimal ET_SCE_RELEXEC fixtures for the vita2native loader.

The generated files exercise, end to end, the parts of the loader that are
hard to unit test:

  * ET_SCE_RELEXEC parsing and program header handling
  * PT_LOAD mapping into the JIT region (RX) and the data buffer (RW)
  * PT_SCE_RELA relocations (R_ARM_ABS32 against both segments)
  * module info discovery through the encoded e_entry
  * import resolution: the SceLibKernel:sceKernelExitProcess stub is patched
    with movw/movt/bx and called from guest code

Two variants are produced:

  guest_return.elf  module_start returns 42 if the relocated data pointer is
                    non-zero (proving relocations ran) and 1 otherwise.  This
                    also exercises "guest main returns", which must end the
                    host process.
  guest_exit.elf    module_start calls the imported sceKernelExitProcess(0),
                    which must exit the host process.

Usage: build_guest_elf.py OUTPUT_DIR
"""

import os
import struct
import sys

ET_SCE_RELEXEC = 0xFE04
EM_ARM = 40
PT_LOAD = 1
PT_SCE_RELA = 0x60000000

PF_X = 1
PF_W = 2
PF_R = 4

R_ARM_ABS32 = 2

# SceLibKernel library NID and the sceKernelExitProcess function NID, as
# exported by source/modules/SceLibKernel.c.
LIBKERNEL_NID = 0xCAE9ACE6
EXIT_PROCESS_NID = 0x7595D9AA
MKDIR_NID = 0x9670D39F

# Layout of the RX segment.
OFF_MODULE_START = 0x000
OFF_STUB = 0x040
OFF_LIB_NAME = 0x060
OFF_TEST_PATH = 0x070
OFF_MODULE_INFO = 0x080
OFF_IMPORT = 0x0E0
OFF_FUNC_NID_TABLE = 0x108
OFF_FUNC_ENTRY_TABLE = 0x10C
RX_SIZE = 0x110

# Layout of the RW segment.
OFF_CHECK_WORD = 0x000
RW_SIZE = 0x004

MODULE_INFO_SIZE = 0x60
IMPORT_SIZE = 0x24

# Fields inside the short (0x24) import struct.
IMP_LIBRARY_NID = 12
IMP_LIBRARY_NAME = 16
IMP_FUNC_NID_TABLE = 20
IMP_FUNC_ENTRY_TABLE = 24
IMP_VAR_NID_TABLE = 28
IMP_VAR_ENTRY_TABLE = 32

LIB_NAME = b"SceLibKernel\0"
MODULE_NAME = b"v2n-fixture\0"
TEST_PATH = b"ux0:/fix\0"


def arm_bl(pc, target):
    return 0xEB000000 | (((target - (pc + 8)) >> 2) & 0x00FFFFFF)


def encode_module_start(mode):
    """Return the ARM code for module_start (at OFF_MODULE_START)."""
    if mode == "return":
        # ldr r1, [pc, #16]   ; r1 = &check_word (literal at 0x18)
        # ldr r0, [r1]
        # cmp r0, #0
        # movne r0, #42       ; the relocation happened
        # moveq r0, #1        ; the relocation did not happen
        # bx lr
        return b"".join(struct.pack("<I", insn) for insn in (
            0xE59F1010,
            0xE5910000,
            0xE3500000,
            0x13A0002A,
            0x03A00001,
            0xE12FFF1E,
        ))

    if mode == "io":
        # ldr r0, [pc, #20]   ; r0 = "ux0:/fix" (literal at 0x20)
        # ldr r1, [pc, #20]   ; r1 = 0777 (literal at 0x24)
        # bl stub             ; sceIoMkdir
        # cmp r0, #0
        # moveq r0, #42
        # movne r0, #1
        # pop {r4, pc}
        return b"".join(struct.pack("<I", insn) for insn in (
            0xE92D4010,
            0xE59F0014,
            0xE59F1014,
            arm_bl(OFF_MODULE_START + 0x0C, OFF_STUB),
            0xE3500000,
            0x03A0002A,
            0x13A00001,
            0xE8BD8010,
        ))

    # push {r4, lr}
    # mov r0, #0
    # bl stub             ; sceKernelExitProcess never returns
    # pop {r4, pc}
    return b"".join(struct.pack("<I", insn) for insn in (
        0xE92D4010,
        0xE3A00000,
        arm_bl(OFF_MODULE_START + 0x08, OFF_STUB),
        0xE8BD8010,
    ))


def encode_module_info():
    info = bytearray(MODULE_INFO_SIZE)

    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)  # attributes, version
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    info[0x1F] = 0  # module type 0 = executable

    struct.pack_into("<I", info, 0x20, 0)                         # gp_value
    struct.pack_into("<I", info, 0x24, 0)                         # export_top
    struct.pack_into("<I", info, 0x28, 0)                         # export_end
    struct.pack_into("<I", info, 0x2C, OFF_IMPORT)                # import_top
    struct.pack_into("<I", info, 0x30, OFF_IMPORT + IMPORT_SIZE)  # import_end
    struct.pack_into("<I", info, 0x34, 0x12345678)                # module_nid
    struct.pack_into("<I", info, 0x44, OFF_MODULE_START)          # module_start
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)                # module_stop
    struct.pack_into("<I", info, 0x5C, 0x03570011)                # module_sdk_version
    return info


def encode_import():
    imp = bytearray(IMPORT_SIZE)

    struct.pack_into("<HHHHHH", imp, 0,
                     IMPORT_SIZE, 1, 0, 1, 0, 0)  # size, version, flags, funcs, vars, tls
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, LIBKERNEL_NID)
    struct.pack_into("<I", imp, IMP_LIBRARY_NAME, 0)      # relocated
    struct.pack_into("<I", imp, IMP_FUNC_NID_TABLE, 0)    # relocated
    struct.pack_into("<I", imp, IMP_FUNC_ENTRY_TABLE, 0)  # relocated
    struct.pack_into("<I", imp, IMP_VAR_NID_TABLE, 0)
    struct.pack_into("<I", imp, IMP_VAR_ENTRY_TABLE, 0)
    return imp


def build_rx(mode):
    rx = bytearray(RX_SIZE)

    code = encode_module_start(mode)
    assert len(code) <= OFF_STUB - OFF_MODULE_START
    rx[OFF_MODULE_START:OFF_MODULE_START + len(code)] = code

    if mode == "io":
        # Second literal used as the mkdir mode argument.
        struct.pack_into("<I", rx, 0x24, 0x1FF)

    # The import stub, rewritten by the loader with movw/movt/bx.
    rx[OFF_STUB:OFF_STUB + 12] = b"\0" * 12

    rx[OFF_LIB_NAME:OFF_LIB_NAME + len(LIB_NAME)] = LIB_NAME
    rx[OFF_TEST_PATH:OFF_TEST_PATH + len(TEST_PATH)] = TEST_PATH
    rx[OFF_MODULE_INFO:OFF_MODULE_INFO + MODULE_INFO_SIZE] = encode_module_info()
    rx[OFF_IMPORT:OFF_IMPORT + IMPORT_SIZE] = encode_import()
    struct.pack_into("<I", rx, OFF_FUNC_NID_TABLE,
                     MKDIR_NID if mode == "io" else EXIT_PROCESS_NID)
    struct.pack_into("<I", rx, OFF_FUNC_ENTRY_TABLE, 0)  # relocated
    return rx


def build_rw():
    rw = bytearray(RW_SIZE)
    struct.pack_into("<I", rw, OFF_CHECK_WORD, 0)  # relocated
    return rw


def encode_rela(datseg, symseg, offset, addend, code=R_ARM_ABS32):
    r_type = (datseg << 16) | (code << 8) | (symseg << 4)
    return struct.pack("<III", r_type, addend, offset)


def build_rela(mode):
    rela = bytearray()

    # Import table pointers: absolute RX addresses.
    rela += encode_rela(0, 0, OFF_IMPORT + IMP_LIBRARY_NAME, OFF_LIB_NAME)
    rela += encode_rela(0, 0, OFF_IMPORT + IMP_FUNC_NID_TABLE, OFF_FUNC_NID_TABLE)
    rela += encode_rela(0, 0, OFF_IMPORT + IMP_FUNC_ENTRY_TABLE, OFF_FUNC_ENTRY_TABLE)

    # The stub address the loader patches with movw/movt/bx.
    rela += encode_rela(0, 0, OFF_FUNC_ENTRY_TABLE, OFF_STUB)

    # RW word pointing into RX, used by the "return" variant as a relocation
    # smoke test.
    rela += encode_rela(1, 0, OFF_CHECK_WORD, OFF_MODULE_START)

    if mode == "return":
        # Literal pool entry in RX holding the address of the RW check word.
        rela += encode_rela(0, 1, 0x18, OFF_CHECK_WORD)

    if mode == "io":
        # Literal pool entry holding the address of the test path string.
        rela += encode_rela(0, 0, 0x20, OFF_TEST_PATH)

    return rela


def build_elf(mode):
    rx = build_rx(mode)
    rw = build_rw()
    rela = build_rela(mode)

    phnum = 3
    phoff = 52
    rx_off = 0x100
    rw_off = (rx_off + len(rx) + 0xF) & ~0xF
    rela_off = (rw_off + len(rw) + 0xF) & ~0xF

    elf = bytearray(rela_off + len(rela))

    # ELF header.
    elf[0:16] = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    struct.pack_into("<HHIIIIIHHHHHH", elf, 16,
                     ET_SCE_RELEXEC,     # e_type
                     EM_ARM,             # e_machine
                     1,                  # e_version
                     OFF_MODULE_INFO,    # e_entry: segment 0, module info offset
                     phoff,              # e_phoff
                     0,                  # e_shoff
                     0x05000000,         # e_flags: EABI version 5
                     52,                 # e_ehsize
                     32,                 # e_phentsize
                     phnum,              # e_phnum
                     0,                  # e_shentsize
                     0,                  # e_shnum
                     0)                  # e_shstrndx

    # Program headers.
    ph = phoff
    struct.pack_into("<IIIIIIII", elf, ph,
                     PT_LOAD, rx_off, 0, 0, len(rx), len(rx), PF_R | PF_X, 0x100)
    ph += 32
    struct.pack_into("<IIIIIIII", elf, ph,
                     PT_LOAD, rw_off, 0, 0, len(rw), len(rw), PF_R | PF_W, 0x100)
    ph += 32
    struct.pack_into("<IIIIIIII", elf, ph,
                     PT_SCE_RELA, rela_off, 0, 0, len(rela), len(rela), 0, 4)

    elf[rx_off:rx_off + len(rx)] = rx
    elf[rw_off:rw_off + len(rw)] = rw
    elf[rela_off:rela_off + len(rela)] = rela
    return bytes(elf)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1

    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    for mode in ("return", "exit", "io"):
        path = os.path.join(out_dir, f"guest_{mode}.elf")
        with open(path, "wb") as f:
            f.write(build_elf(mode))
        print(f"wrote {path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
