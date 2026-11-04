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
SCEGXM_NID = 0xF76B66BD
GXM_INITIALIZE_NID = 0xB0F1E4EC

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

    if mode == "gxm":
        # push {r4, lr}
        # ldr r0, [pc, #16]   ; r0 = zeroed SceGxmInitializeParams
        # bl stub             ; sceGxmInitialize
        # cmp r0, #0
        # moveq r0, #42
        # movne r0, #1
        # pop {r4, pc}
        return b"".join(struct.pack("<I", insn) for insn in (
            0xE92D4010,
            0xE59F0010,
            arm_bl(OFF_MODULE_START + 0x08, OFF_STUB),
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


def encode_import(mode="return"):
    imp = bytearray(IMPORT_SIZE)

    struct.pack_into("<HHHHHH", imp, 0,
                     IMPORT_SIZE, 1, 0, 1, 0, 0)  # size, version, flags, funcs, vars, tls
    struct.pack_into("<I", imp, IMP_LIBRARY_NID,
                     SCEGXM_NID if mode == "gxm" else LIBKERNEL_NID)
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
    rx[OFF_IMPORT:OFF_IMPORT + IMPORT_SIZE] = encode_import(mode)
    struct.pack_into("<I", rx, OFF_FUNC_NID_TABLE,
                     MKDIR_NID if mode == "io" else
                     GXM_INITIALIZE_NID if mode == "gxm" else EXIT_PROCESS_NID)
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

    if mode == "gxm":
        # Literal pool entry holding the address of the zeroed init params.
        rela += encode_rela(0, 1, 0x1C, 0)

    return rela


# --- GXM scene fixture ------------------------------------------------------
#
# Exercises the non-shader GXM path end to end:
#   initialize -> create context -> create render target -> color surface
#   -> begin scene -> end scene
# Uses movw/movt absolute relocations for RW pointers, so all structures live
# in the data segment and no literal pool is needed.

GXM_STUB_BASE = 0x140
GXM_LIB_NAME = 0x190
GXM_MODINFO = 0x1A0
GXM_IMPORT = 0x200
GXM_NIDS = 0x228
GXM_ENTRIES = 0x240
GXM_RX_SIZE = 0x258

GXM_FUNCS = [
    0xB0F1E4EC,  # sceGxmInitialize
    0xE84CE5B4,  # sceGxmCreateContext
    0x207AF96B,  # sceGxmCreateRenderTarget
    0xED0F6E25,  # sceGxmColorSurfaceInit
    0x8734FF4E,  # sceGxmBeginScene
    0xFE300E2F,  # sceGxmEndScene
]

RW_INIT = 0x000
RW_CTXPARAMS = 0x020
RW_HOSTMEM = 0x050
RW_RTPARAMS = 0x4050
RW_CSURF = 0x4070
RW_FB = 0x40A0
RW_CTX_OUT = 0x80A0
RW_RT_OUT = 0x80A4
RW_SIZE = 0x80A8

R_ARM_MOVW_ABS_NC = 43
R_ARM_MOVT_ABS = 44


def arm_movw(rd, imm16):
    return 0xE3000000 | ((imm16 & 0xF000) << 4) | (rd << 12) | (imm16 & 0x0FFF)


def arm_movt(rd, imm16):
    return 0xE3400000 | ((imm16 & 0xF000) << 4) | (rd << 12) | (imm16 & 0x0FFF)


def arm_mov_imm(rd, imm8):
    return 0xE3A00000 | (rd << 12) | (imm8 & 0xFF)


def build_gxm_scene_elf():
    code = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, GXM_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | bit)       # orrne r4, r4, #bit

    rx_relocs = []

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    movw_movt_rw(0, RW_INIT)
    call(0)
    check(1)

    movw_movt_rw(0, RW_CTXPARAMS)
    movw_movt_rw(1, RW_CTX_OUT)
    call(1)
    check(2)

    movw_movt_rw(0, RW_RTPARAMS)
    movw_movt_rw(1, RW_RT_OUT)
    call(2)
    check(4)

    # sceGxmColorSurfaceInit(surface, fmt, type, scale, outreg, w, h, stride, data)
    movw_movt_rw(0, RW_CSURF)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD018)                 # sub sp, sp, #24
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)                 # str r12, [sp, #0]
    emit(arm_mov_imm(12, 64))
    emit(0xE58DC004)
    emit(0xE58DC008)
    emit(0xE58DC00C)
    movw_movt_rw(12, RW_FB)
    emit(0xE58DC010)                 # str r12, [sp, #16]
    call(3)
    check(8)
    emit(0xE28DD018)                 # add sp, sp, #24

    # sceGxmBeginScene(ctx, flags, rt, valid, vsync, fsync, color, ds)
    movw_movt_rw(12, RW_CTX_OUT)
    emit(0xE59C0000)                 # ldr r0, [r12]
    emit(arm_mov_imm(1, 0))
    movw_movt_rw(12, RW_RT_OUT)
    emit(0xE59C2000)                 # ldr r2, [r12]
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD010)                 # sub sp, sp, #16
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    emit(0xE58DC004)
    movw_movt_rw(12, RW_CSURF)
    emit(0xE58DC008)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC00C)
    call(4)
    check(16)
    emit(0xE28DD010)                 # add sp, sp, #16

    # sceGxmEndScene(ctx, NULL, NULL)
    movw_movt_rw(12, RW_CTX_OUT)
    emit(0xE59C0000)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(5)
    check(32)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= GXM_STUB_BASE, "GXM fixture code overlaps the stubs"

    rx = bytearray(GXM_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    rx[GXM_LIB_NAME:GXM_LIB_NAME + len(LIB_NAME)] = LIB_NAME

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, GXM_IMPORT)
    struct.pack_into("<I", info, 0x30, GXM_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[GXM_MODINFO:GXM_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(GXM_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, SCEGXM_NID)
    rx[GXM_IMPORT:GXM_IMPORT + IMPORT_SIZE] = imp
    struct.pack_into("<I", rx, GXM_NIDS, 0)

    for i, nid in enumerate(GXM_FUNCS):
        struct.pack_into("<I", rx, GXM_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, GXM_IMPORT + IMP_LIBRARY_NAME, GXM_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, GXM_IMPORT + IMP_FUNC_NID_TABLE, GXM_NIDS))
    rx_relocs.append(encode_rela(0, 0, GXM_IMPORT + IMP_FUNC_ENTRY_TABLE, GXM_ENTRIES))
    for i in range(len(GXM_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, GXM_ENTRIES + i * 4, GXM_STUB_BASE + i * 12))

    rw = bytearray(RW_SIZE)
    struct.pack_into("<I", rw, RW_CTXPARAMS + 4, 0x4000)  # hostMemSize
    struct.pack_into("<HH", rw, RW_RTPARAMS + 4, 64, 64)
    struct.pack_into("<H", rw, RW_RTPARAMS + 8, 1)
    rw_relocs = [encode_rela(1, 1, RW_CTXPARAMS, RW_HOSTMEM)]

    rela = bytearray(b"".join(rx_relocs) + b"".join(rw_relocs))
    return assemble_elf(rx, rw, rela, GXM_MODINFO)


def assemble_elf(rx, rw, rela, entry_offset):
    phoff = 52
    rx_off = 0x100
    rw_off = (rx_off + len(rx) + 0xF) & ~0xF
    rela_off = (rw_off + len(rw) + 0xF) & ~0xF
    elf = bytearray(rela_off + len(rela))

    elf[0:16] = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    struct.pack_into("<HHIIIIIHHHHHH", elf, 16,
                     ET_SCE_RELEXEC, EM_ARM, 1, entry_offset, phoff, 0, 0x05000000,
                     52, 32, 3, 0, 0, 0)

    ph = phoff
    struct.pack_into("<IIIIIIII", elf, ph, PT_LOAD, rx_off, 0, 0, len(rx), len(rx),
                     PF_R | PF_X, 0x100)
    ph += 32
    struct.pack_into("<IIIIIIII", elf, ph, PT_LOAD, rw_off, 0, 0, len(rw), len(rw),
                     PF_R | PF_W, 0x100)
    ph += 32
    struct.pack_into("<IIIIIIII", elf, ph, PT_SCE_RELA, rela_off, 0, 0, len(rela), len(rela), 0, 4)

    elf[rx_off:rx_off + len(rx)] = rx
    elf[rw_off:rw_off + len(rw)] = rw
    elf[rela_off:rela_off + len(rela)] = rela
    return bytes(elf)


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

    for mode in ("return", "exit", "io", "gxm"):
        path = os.path.join(out_dir, f"guest_{mode}.elf")
        with open(path, "wb") as f:
            f.write(build_elf(mode))
        print(f"wrote {path}")

    scene_path = os.path.join(out_dir, "guest_gxm_scene.elf")
    with open(scene_path, "wb") as f:
        f.write(build_gxm_scene_elf())
    print(f"wrote {scene_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
