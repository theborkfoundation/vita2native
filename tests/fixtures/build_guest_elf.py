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
SCEAUDIO_NID = 0x438BB957
SCEAPPUTIL_NID = 0x48E01D74
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


# --- SceAudio fixture ------------------------------------------------------

AUDIO_STUB_BASE = 0x140
AUDIO_LIB_NAME = 0x190
AUDIO_MODINFO = 0x1A0
AUDIO_IMPORT = 0x200
AUDIO_NIDS = 0x228
AUDIO_ENTRIES = 0x240
AUDIO_RX_SIZE = 0x258

AUDIO_LIB = b"SceAudio\0"

AUDIO_FUNCS = [
    0x5BC341E4,  # sceAudioOutOpenPort
    0x02DB3F5F,  # sceAudioOutOutput
    0x9C8EDAEA,  # sceAudioOutGetConfig
    0x69E2E6B5,  # sceAudioOutReleasePort
]

AUDIO_SAMPLES = 64
AUDIO_RW_SAMPLES = 0x000
AUDIO_RW_SIZE = AUDIO_SAMPLES * 2 * 2


def build_audio_elf():
    """Guest ELF that drives an audio port through the import stubs.

    Covers the streaming contract: open a stereo 48 kHz port, push four
    buffers (each call has to block), read the configuration back and release
    the port. Returns 42 when every call succeeded, otherwise a bitmask of the
    failures.
    """
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, AUDIO_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | bit)       # orrne r4, r4, #bit

    emit(0xE92D4070)                 # push {r4, r5, r6, lr} (keeps SP 8-byte aligned)
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # port = sceAudioOutOpenPort(MAIN, 64, 48000, STEREO)
    emit(arm_mov_imm(0, 0))
    emit(arm_mov_imm(1, AUDIO_SAMPLES))
    emit(arm_movw(2, 48000 & 0xFFFF))
    emit(arm_mov_imm(3, 1))
    call(0)
    emit(0xE1A05000)                 # mov r5, r0
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x43844001)                 # orrmi r4, r4, #1  (port < 0)

    for _ in range(4):
        # sceAudioOutOutput(port, samples)
        emit(0xE1A00005)             # mov r0, r5
        movw_movt_rw(1, AUDIO_RW_SAMPLES)
        call(1)
        check(2)

    # sceAudioOutGetConfig(port, LEN) == 64
    emit(0xE1A00005)                 # mov r0, r5
    emit(arm_mov_imm(1, 0))
    call(2)
    emit(0xE3500040)                 # cmp r0, #64
    emit(0x13844004)                 # orrne r4, r4, #4

    # sceAudioOutReleasePort(port)
    emit(0xE1A00005)                 # mov r0, r5
    call(3)
    check(8)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8070)                 # pop {r4, r5, r6, pc}

    assert len(code) * 4 <= AUDIO_STUB_BASE, "audio fixture code overlaps the stubs"

    rx = bytearray(AUDIO_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    rx[AUDIO_LIB_NAME:AUDIO_LIB_NAME + len(AUDIO_LIB)] = AUDIO_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, AUDIO_IMPORT)
    struct.pack_into("<I", info, 0x30, AUDIO_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[AUDIO_MODINFO:AUDIO_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(AUDIO_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, SCEAUDIO_NID)
    rx[AUDIO_IMPORT:AUDIO_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(AUDIO_FUNCS):
        struct.pack_into("<I", rx, AUDIO_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, AUDIO_IMPORT + IMP_LIBRARY_NAME, AUDIO_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, AUDIO_IMPORT + IMP_FUNC_NID_TABLE, AUDIO_NIDS))
    rx_relocs.append(encode_rela(0, 0, AUDIO_IMPORT + IMP_FUNC_ENTRY_TABLE, AUDIO_ENTRIES))
    for i in range(len(AUDIO_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, AUDIO_ENTRIES + i * 4,
                                     AUDIO_STUB_BASE + i * 12))

    rw = bytearray(AUDIO_RW_SIZE)
    for i in range(AUDIO_SAMPLES * 2):
        struct.pack_into("<h", rw, i * 2, 4096 if (i % 2) == 0 else -4096)

    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, AUDIO_MODINFO)


# --- SceNgs fixture --------------------------------------------------------

NGS_STUB_BASE = 0x200
NGS_LIB_NAME = 0x280
NGS_MODINFO = 0x290
NGS_IMPORT = 0x2F0
NGS_NIDS = 0x320
NGS_ENTRIES = 0x350
NGS_RX_SIZE = 0x3E0

NGS_LIB = b"SceNgs\0"
NGS_LIB_NID = 0xB01598D9

NGS_FUNCS = [
    0x6CE8B36F,  # sceNgsSystemGetRequiredMemorySize
    0xED14CF4A,  # sceNgsSystemInit
    0x79A121D1,  # sceNgsVoiceDefGetMasterBuss
    0x477318C0,  # sceNgsRackGetRequiredMemorySize
    0x0A92E4EC,  # sceNgsRackInit
    0xFE1A98E9,  # sceNgsRackGetVoiceHandle
    0xFA0A0F34,  # sceNgsVoicePlay
    0x5551410D,  # sceNgsVoiceGetInfo
    0xDD5CA10B,  # sceNgsRackRelease
    0x4A25BEBC,  # sceNgsSystemRelease
]

NGS_RW_PARAMS = 0x000
NGS_RW_SIZE_OUT = 0x020
NGS_RW_SYSMEM = 0x040
NGS_RW_RACKDESC = 0x1840
NGS_RW_RACKBUF = 0x1860
NGS_RW_SYSTEM = 0x1880
NGS_RW_RACK = 0x1884
NGS_RW_VOICE = 0x1888
NGS_RW_VOICEINFO = 0x1890
NGS_RW_RACKMEM = 0x18B0
NGS_RW_SIZE = 0x28B0


def build_ngs_elf():
    """Guest ELF that drives a complete NGS system/rack/voice lifecycle.

    Every call goes through a real guest import stub, so this also proves the
    SceNgs library NID and the function NIDs used by the port.
    """
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, NGS_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def orrne_r4(bit):
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def orreq_r4(bit):
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))  # ldr rd, [rd]

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # sceNgsSystemGetRequiredMemorySize(&params, &size)
    movw_movt_rw(0, NGS_RW_PARAMS)
    movw_movt_rw(1, NGS_RW_SIZE_OUT)
    call(0)
    check(1)

    # sceNgsSystemInit(system_mem, size, &params, &system)
    movw_movt_rw(0, NGS_RW_SYSMEM)
    load_rw(1, NGS_RW_SIZE_OUT)
    movw_movt_rw(2, NGS_RW_PARAMS)
    movw_movt_rw(3, NGS_RW_SYSTEM)
    call(1)
    check(2)

    # rack_desc.voice_definition = sceNgsVoiceDefGetMasterBuss()
    call(2)
    movw_movt_rw(1, NGS_RW_RACKDESC)
    emit(0xE5810000)                 # str r0, [r1]

    # sceNgsRackGetRequiredMemorySize(system, &rack_desc, &size)
    load_rw(0, NGS_RW_SYSTEM)
    movw_movt_rw(1, NGS_RW_RACKDESC)
    movw_movt_rw(2, NGS_RW_SIZE_OUT)
    call(3)
    check(4)

    # sceNgsRackInit(system, &rack_buffer, &rack_desc, &rack)
    load_rw(0, NGS_RW_SYSTEM)
    movw_movt_rw(1, NGS_RW_RACKBUF)
    movw_movt_rw(2, NGS_RW_RACKDESC)
    movw_movt_rw(3, NGS_RW_RACK)
    call(4)
    check(8)

    # sceNgsRackGetVoiceHandle(rack, 0, &voice)
    load_rw(0, NGS_RW_RACK)
    emit(arm_mov_imm(1, 0))
    movw_movt_rw(2, NGS_RW_VOICE)
    call(5)
    check(16)
    load_rw(1, NGS_RW_VOICE)
    emit(0xE3510000)                 # cmp r1, #0
    orreq_r4(32)

    # sceNgsVoicePlay(voice)
    load_rw(0, NGS_RW_VOICE)
    call(6)
    check(64)

    # sceNgsVoiceGetInfo(voice, &info), expect ACTIVE (1)
    load_rw(0, NGS_RW_VOICE)
    movw_movt_rw(1, NGS_RW_VOICEINFO)
    call(7)
    check(128)
    load_rw(1, NGS_RW_VOICEINFO)
    emit(0xE3510001)                 # cmp r1, #1
    emit(0x13844000 | arm_encode_imm(256))  # orrne r4, r4, #256

    # sceNgsRackRelease(rack, NULL)
    load_rw(0, NGS_RW_RACK)
    emit(arm_mov_imm(1, 0))
    call(8)
    check(512)

    # sceNgsSystemRelease(system)
    load_rw(0, NGS_RW_SYSTEM)
    call(9)
    check(1024)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= NGS_STUB_BASE, "NGS fixture code overlaps the stubs"

    rx = bytearray(NGS_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[NGS_LIB_NAME:NGS_LIB_NAME + len(NGS_LIB)] = NGS_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, NGS_IMPORT)
    struct.pack_into("<I", info, 0x30, NGS_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[NGS_MODINFO:NGS_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(NGS_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, NGS_LIB_NID)
    rx[NGS_IMPORT:NGS_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(NGS_FUNCS):
        struct.pack_into("<I", rx, NGS_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, NGS_IMPORT + IMP_LIBRARY_NAME, NGS_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, NGS_IMPORT + IMP_FUNC_NID_TABLE, NGS_NIDS))
    rx_relocs.append(encode_rela(0, 0, NGS_IMPORT + IMP_FUNC_ENTRY_TABLE, NGS_ENTRIES))
    for i in range(len(NGS_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, NGS_ENTRIES + i * 4, NGS_STUB_BASE + i * 12))

    rw = bytearray(NGS_RW_SIZE)
    struct.pack_into("<iiiii", rw, NGS_RW_PARAMS, 2, 4, 256, 48000, 0)
    struct.pack_into("<iiiii", rw, NGS_RW_RACKDESC + 4, 2, 2, 4, 4, 0)
    rw_relocs = [encode_rela(1, 1, NGS_RW_RACKBUF, NGS_RW_RACKMEM)]

    rela = bytearray(b"".join(rx_relocs) + b"".join(rw_relocs))
    return assemble_elf(rx, rw, rela, NGS_MODINFO)


# --- SceIofilemgr async fixture -------------------------------------------

IO_STUB_BASE = 0x280
IO_LIB1_NAME = 0x330
IO_LIB2_NAME = 0x340
IO_MODINFO = 0x350
IO_IMPORT1 = 0x3B0
IO_IMPORT2 = 0x3D4
IO_NIDS1 = 0x400
IO_ENTRIES1 = 0x420
IO_NIDS2 = 0x440
IO_ENTRIES2 = 0x460
IO_RX_SIZE = 0x480

IO_LIB1 = b"SceLibKernel\0"
IO_LIB2 = b"SceIofilemgr\0"
IO_LIB1_NID = 0xCAE9ACE6
IO_LIB2_NID = 0xF2FF276E

IO_KERNEL_FUNCS = [
    0x6A7EA9FD,  # sceIoOpenAsync
    0xCAC5D672,  # sceIoLseekAsync
    0x82B20B41,  # sceIoGetstatAsync
    0x8E5FCBB1,  # sceIoMkdirAsync
    0xEE9857CD,  # sceIoRenameAsync
    0x446A60AC,  # sceIoRemoveAsync
    0x9694D00F,  # sceIoRmdirAsync
]

IO_FILEMGR_FUNCS = [
    0xE0D63D2A,  # sceIoWriteAsync
    0x773EBD45,  # sceIoReadAsync
    0xD1C49D2F,  # sceIoComplete
    0x8EA3616A,  # sceIoCloseAsync
    0xCC67B6FD,  # _sceIoOpen
    0x539FD5C4,  # _sceIoPread
    0xC70B8886,  # sceIoClose
]

IO_RW_PATH = 0x000
IO_RW_PATH2 = 0x020
IO_RW_DIR = 0x040
IO_RW_DATA = 0x050
IO_RW_BUF = 0x060
IO_RW_FD = 0x068
IO_RW_FD2 = 0x06C
IO_RW_STAT = 0x080
IO_RW_PREAD = 0x0E0
IO_RW_SIZE = 0x100


def build_io_async_elf():
    """Guest ELF that drives the async I/O lifecycle over real import stubs."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, IO_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))  # ldr rd, [rd]

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_nonzero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    def check_equal(value, bit):
        emit(arm_movw(1, value & 0xFFFF))
        if value > 0xFFFF:
            emit(arm_movt(1, value >> 16))
        emit(0xE1500001)             # cmp r0, r1
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    emit(0xE92D4070)                 # push {r4, r5, r6, lr} (keeps SP 8-byte aligned)
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # fd = sceIoOpenAsync(path, RDWR|CREAT|TRUNC, 0666)
    movw_movt_rw(0, IO_RW_PATH)
    emit(arm_movw(1, 0x0603))
    emit(arm_movw(2, 0x01B6))
    call(0)
    movw_movt_rw(1, IO_RW_FD)
    emit(0xE5810000)                 # str r0, [r1]
    check_nonzero(1)

    # sceIoComplete(fd) == 0 after open
    load_rw(0, IO_RW_FD)
    call(9)
    check(2)

    # sceIoWriteAsync(fd, "hello", 5), completion result == 5
    load_rw(0, IO_RW_FD)
    movw_movt_rw(1, IO_RW_DATA)
    emit(arm_mov_imm(2, 5))
    call(7)
    check(4)
    load_rw(0, IO_RW_FD)
    call(9)
    check_equal(5, 8)

    # sceIoLseekAsync(fd, 0, SEEK_SET), completion result == 0.
    # 64-bit SceOff goes in r2:r3 and the trailing int on the stack.
    load_rw(0, IO_RW_FD)
    emit(arm_mov_imm(1, 0))          # padding before the 64-bit argument
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)                 # sub sp, sp, #8
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)                 # str r12, [sp]
    call(1)
    emit(0xE28DD008)                 # add sp, sp, #8
    check(16)
    load_rw(0, IO_RW_FD)
    call(9)
    check(32)

    # sceIoReadAsync(fd, buf, 5), completion result == 5 and buf[0] == 'h'
    load_rw(0, IO_RW_FD)
    movw_movt_rw(1, IO_RW_BUF)
    emit(arm_mov_imm(2, 5))
    call(8)
    check(64)
    load_rw(0, IO_RW_FD)
    call(9)
    check_equal(5, 128)
    movw_movt_rw(1, IO_RW_BUF)
    emit(0xE5D11000)                 # ldrb r1, [r1]
    emit(0xE3510068)                 # cmp r1, #'h'
    emit(0x13844000 | arm_encode_imm(256))

    # sceIoCloseAsync(fd)
    load_rw(0, IO_RW_FD)
    call(10)
    check(512)

    # _sceIoOpen(path, RDONLY, 0, NULL), _sceIoPread(fd, {buf, 5, 0}) == 5
    movw_movt_rw(0, IO_RW_PATH)
    emit(arm_mov_imm(1, 1))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    call(11)
    movw_movt_rw(1, IO_RW_FD2)
    emit(0xE5810000)
    check_nonzero(1024)
    load_rw(0, IO_RW_FD2)
    movw_movt_rw(1, IO_RW_PREAD)
    call(12)
    check_equal(5, 2048)
    load_rw(0, IO_RW_FD2)
    call(13)
    check(4096)

    # Path operations: getstat, mkdir, rename, remove, rmdir.
    movw_movt_rw(0, IO_RW_PATH)
    movw_movt_rw(1, IO_RW_STAT)
    call(2)
    check(8192)
    call(9)
    emit(arm_mov_imm(0, 0))
    check(16384)

    movw_movt_rw(0, IO_RW_DIR)
    emit(arm_movw(1, 0x01FF))
    call(3)
    check(32768)
    emit(arm_mov_imm(0, 0))
    call(9)
    check(65536)

    movw_movt_rw(0, IO_RW_PATH)
    movw_movt_rw(1, IO_RW_PATH2)
    call(4)
    check(131072)
    emit(arm_mov_imm(0, 0))
    call(9)
    check(262144)

    movw_movt_rw(0, IO_RW_PATH2)
    call(5)
    check(524288)
    emit(arm_mov_imm(0, 0))
    call(9)
    check(1048576)

    movw_movt_rw(0, IO_RW_DIR)
    call(6)
    check(2097152)
    emit(arm_mov_imm(0, 0))
    call(9)
    check(4194304)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8070)                 # pop {r4, r5, r6, pc}

    assert len(code) * 4 <= IO_STUB_BASE, "I/O fixture code overlaps the stubs"

    rx = bytearray(IO_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[IO_LIB1_NAME:IO_LIB1_NAME + len(IO_LIB1)] = IO_LIB1
    rx[IO_LIB2_NAME:IO_LIB2_NAME + len(IO_LIB2)] = IO_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, IO_IMPORT1)
    struct.pack_into("<I", info, 0x30, IO_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[IO_MODINFO:IO_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(IO_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, IO_LIB1_NID)
    rx[IO_IMPORT1:IO_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(IO_FILEMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, IO_LIB2_NID)
    rx[IO_IMPORT2:IO_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(IO_KERNEL_FUNCS):
        struct.pack_into("<I", rx, IO_NIDS1 + i * 4, nid)
    for i, nid in enumerate(IO_FILEMGR_FUNCS):
        struct.pack_into("<I", rx, IO_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, IO_IMPORT1 + IMP_LIBRARY_NAME, IO_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, IO_IMPORT1 + IMP_FUNC_NID_TABLE, IO_NIDS1))
    rx_relocs.append(encode_rela(0, 0, IO_IMPORT1 + IMP_FUNC_ENTRY_TABLE, IO_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, IO_IMPORT2 + IMP_LIBRARY_NAME, IO_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, IO_IMPORT2 + IMP_FUNC_NID_TABLE, IO_NIDS2))
    rx_relocs.append(encode_rela(0, 0, IO_IMPORT2 + IMP_FUNC_ENTRY_TABLE, IO_ENTRIES2))

    for i in range(len(IO_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, IO_ENTRIES1 + i * 4, IO_STUB_BASE + i * 12))
    for i in range(len(IO_FILEMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, IO_ENTRIES2 + i * 4,
                                     IO_STUB_BASE + (len(IO_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(IO_RW_SIZE)
    rw[IO_RW_PATH:IO_RW_PATH + 15] = b"ux0:/async.txt\0"
    rw[IO_RW_PATH2:IO_RW_PATH2 + 16] = b"ux0:/async2.txt\0"
    rw[IO_RW_DIR:IO_RW_DIR + 10] = b"ux0:/adir\0"
    rw[IO_RW_DATA:IO_RW_DATA + 6] = b"hello\0"
    struct.pack_into("<I", rw, IO_RW_PREAD + 4, 5)
    rw_relocs = [encode_rela(1, 1, IO_RW_PREAD, IO_RW_BUF)]

    rela = bytearray(b"".join(rx_relocs) + b"".join(rw_relocs))
    return assemble_elf(rx, rw, rela, IO_MODINFO)


# --- SceLibKernel runtime fixture -------------------------------------------

KR_STUB_BASE = 0x200
KR_LIB_NAME = 0x270
KR_MODINFO = 0x280
KR_IMPORT = 0x2E0
KR_NIDS = 0x308
KR_ENTRIES = 0x328
KR_RX_SIZE = 0x350

KR_LIB = b"SceLibKernel\0"
KR_LIB_NID = 0xCAE9ACE6

KR_FUNCS = [
    0x2E84A93B,  # sceKernelAtomicAddAndGet32
    0x9B43E667,  # sceKernelAtomicGetAndSet32
    0x1124A1D4,  # sceKernelAtomicCompareAndSet32
    0xCA3294F1,  # sceKernelAtomicDecIfPositive32
    0xB110C123,  # sceKernelGetProcessTimeWide
    0x8AF15B5F,  # sceKernelGetSystemTime
    0xB2700165,  # sceKernelGetRandomNumber
    0x261E2C34,  # sceKernelGetOpenPsId
]

KR_RW_VALUE = 0x000
KR_RW_CLOCK = 0x008
KR_RW_RANDOM = 0x010
KR_RW_PSID = 0x020
KR_RW_SIZE = 0x040


def build_kernel_runtime_elf():
    """Guest ELF for atomics, clocks, RNG and OpenPsId."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, KR_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_zero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    def check_equal(value, bit):
        emit(arm_movw(2, value & 0xFFFF))
        if value > 0xFFFF:
            emit(arm_movt(2, value >> 16))
        emit(0xE1500002)             # cmp r0, r2
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # store 5 at value
    movw_movt_rw(0, KR_RW_VALUE)
    emit(arm_mov_imm(1, 5))
    emit(0xE5801000)                 # str r1, [r0]

    # sceKernelAtomicAddAndGet32(&value, 3) == 8
    movw_movt_rw(0, KR_RW_VALUE)
    emit(arm_mov_imm(1, 3))
    call(0)
    check_equal(8, 1)

    # sceKernelAtomicGetAndSet32(&value, 10) == 8
    movw_movt_rw(0, KR_RW_VALUE)
    emit(arm_mov_imm(1, 10))
    call(1)
    check_equal(8, 2)

    # sceKernelAtomicCompareAndSet32(&value, 10, 20) == 10
    movw_movt_rw(0, KR_RW_VALUE)
    emit(arm_mov_imm(1, 10))
    emit(arm_mov_imm(2, 20))
    call(2)
    check_equal(10, 4)

    # sceKernelAtomicDecIfPositive32(&value) == 19
    movw_movt_rw(0, KR_RW_VALUE)
    call(3)
    check_equal(19, 8)

    # sceKernelGetProcessTimeWide() != 0 (64-bit return in r0:r1)
    call(4)
    emit(0xE1800001)                 # orr r0, r0, r1
    check_zero(16)

    # sceKernelGetSystemTime(&clock), then the clock must be nonzero
    movw_movt_rw(0, KR_RW_CLOCK)
    call(5)
    check(32)
    movw_movt_rw(1, KR_RW_CLOCK)
    emit(0xE5910000)                 # ldr r0, [r1]
    emit(0xE5912004)                 # ldr r2, [r1, #4]
    emit(0xE1800002)                 # orr r0, r0, r2
    check_zero(64)

    # sceKernelGetRandomNumber(&random, 16), first word must be nonzero
    movw_movt_rw(0, KR_RW_RANDOM)
    emit(arm_mov_imm(1, 16))
    call(6)
    check(128)
    movw_movt_rw(1, KR_RW_RANDOM)
    emit(0xE5910000)                 # ldr r0, [r1]
    check_zero(256)

    # sceKernelGetOpenPsId(&psid), first byte must be 0x10
    movw_movt_rw(0, KR_RW_PSID)
    call(7)
    check(512)
    movw_movt_rw(1, KR_RW_PSID)
    emit(0xE5D10000)                 # ldrb r0, [r1]
    check_equal(0x10, 1024)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= KR_STUB_BASE, "kernel runtime fixture code overlaps the stubs"

    rx = bytearray(KR_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[KR_LIB_NAME:KR_LIB_NAME + len(KR_LIB)] = KR_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, KR_IMPORT)
    struct.pack_into("<I", info, 0x30, KR_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[KR_MODINFO:KR_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(KR_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, KR_LIB_NID)
    rx[KR_IMPORT:KR_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(KR_FUNCS):
        struct.pack_into("<I", rx, KR_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, KR_IMPORT + IMP_LIBRARY_NAME, KR_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, KR_IMPORT + IMP_FUNC_NID_TABLE, KR_NIDS))
    rx_relocs.append(encode_rela(0, 0, KR_IMPORT + IMP_FUNC_ENTRY_TABLE, KR_ENTRIES))
    for i in range(len(KR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, KR_ENTRIES + i * 4, KR_STUB_BASE + i * 12))

    rw = bytearray(KR_RW_SIZE)
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, KR_MODINFO)


# --- SceKernel locks fixture ------------------------------------------------

LK_STUB_BASE = 0x200
LK_LIB1_NAME = 0x2A0
LK_LIB2_NAME = 0x2B0
LK_MODINFO = 0x2C0
LK_IMPORT1 = 0x320
LK_IMPORT2 = 0x344
LK_NIDS1 = 0x370
LK_ENTRIES1 = 0x398
LK_NIDS2 = 0x3C0
LK_ENTRIES2 = 0x3D0
LK_RX_SIZE = 0x3E0

LK_LIB1 = b"SceLibKernel\0"
LK_LIB2 = b"SceThreadmgr\0"
LK_LIB1_NID = 0xCAE9ACE6
LK_LIB2_NID = 0x859A24B1

LK_KERNEL_FUNCS = [
    0x8667951D,  # sceKernelCreateRWLock
    0x079A573B,  # sceKernelGetRWLockInfo
    0xFA670F0F,  # sceKernelLockReadRWLock
    0x67A187BB,  # sceKernelLockWriteRWLock
    0xDA6EC8EF,  # sceKernelCreateLwMutex
    0x48C7EAE6,  # sceKernelCreateLwCond
    0x3AC63B9A,  # sceKernelSignalLwCond
    0x721F6CB3,  # sceKernelDeleteLwCond
    0x244E76D2,  # sceKernelDeleteLwMutex
]

LK_THREADMGR_FUNCS = [
    0xE73649CA,  # sceKernelDeleteRWLock
    0x3EF91145,  # sceKernelUnlockReadRWLock
    0xB4151397,  # sceKernelUnlockWriteRWLock
]

LK_RW_UID = 0x000
LK_RW_INFO = 0x008
LK_RW_MUTEX = 0x050
LK_RW_COND = 0x070
LK_RW_NAME = 0x090
LK_RW_NAME2 = 0x098
LK_RW_SIZE = 0x100


def build_kernel_locks_elf():
    """Guest ELF for RW locks and lightweight mutex/cond lifecycle."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, LK_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))  # ldr rd, [rd]

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_nonzero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # rw = sceKernelCreateRWLock("rw", 0, NULL)
    movw_movt_rw(0, LK_RW_NAME)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(0)
    movw_movt_rw(1, LK_RW_UID)
    emit(0xE5810000)
    check_nonzero(1)

    # lock read, then info.lockCount == 1
    load_rw(0, LK_RW_UID)
    call(2)
    check(2)
    load_rw(0, LK_RW_UID)
    movw_movt_rw(1, LK_RW_INFO)
    call(1)
    check(4)
    movw_movt_rw(1, LK_RW_INFO)
    emit(0xE591002C)                 # ldr r0, [r1, #44] (lockCount)
    emit(0xE3500001)                 # cmp r0, #1
    emit(0x13844000 | arm_encode_imm(8))

    # unlock read, lock write, lockCount == 1 and owner != 0
    load_rw(0, LK_RW_UID)
    call(10)
    check(16)
    load_rw(0, LK_RW_UID)
    call(3)
    check(32)
    load_rw(0, LK_RW_UID)
    movw_movt_rw(1, LK_RW_INFO)
    call(1)
    check(64)
    movw_movt_rw(1, LK_RW_INFO)
    emit(0xE591002C)                 # ldr r0, [r1, #44]
    emit(0xE3500001)                 # cmp r0, #1
    emit(0x13844000 | arm_encode_imm(128))
    emit(0xE5910030)                 # ldr r0, [r1, #48] (writeOwnerId)
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03844000 | arm_encode_imm(256))

    # unlock write and delete
    load_rw(0, LK_RW_UID)
    call(11)
    check(512)
    load_rw(0, LK_RW_UID)
    call(9)
    check(1024)

    # lwmutex = sceKernelCreateLwMutex(&work, "lw", 0, 0, NULL)
    movw_movt_rw(0, LK_RW_MUTEX)
    movw_movt_rw(1, LK_RW_NAME2)
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)                 # sub sp, sp, #8
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)                 # str r12, [sp]
    call(4)
    emit(0xE28DD008)                 # add sp, sp, #8
    check(2048)

    # lwcond = sceKernelCreateLwCond(&cond, "lc", 0, &mutex, NULL)
    movw_movt_rw(0, LK_RW_COND)
    movw_movt_rw(1, LK_RW_NAME2)
    emit(arm_mov_imm(2, 0))
    movw_movt_rw(3, LK_RW_MUTEX)
    emit(0xE24DD008)                 # sub sp, sp, #8
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    call(5)
    emit(0xE28DD008)
    check(4096)

    movw_movt_rw(0, LK_RW_COND)
    call(6)
    check(8192)
    movw_movt_rw(0, LK_RW_COND)
    call(7)
    check(16384)
    movw_movt_rw(0, LK_RW_MUTEX)
    call(8)
    check(32768)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= LK_STUB_BASE, "locks fixture code overlaps the stubs"

    rx = bytearray(LK_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[LK_LIB1_NAME:LK_LIB1_NAME + len(LK_LIB1)] = LK_LIB1
    rx[LK_LIB2_NAME:LK_LIB2_NAME + len(LK_LIB2)] = LK_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, LK_IMPORT1)
    struct.pack_into("<I", info, 0x30, LK_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[LK_MODINFO:LK_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(LK_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, LK_LIB1_NID)
    rx[LK_IMPORT1:LK_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(LK_THREADMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, LK_LIB2_NID)
    rx[LK_IMPORT2:LK_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(LK_KERNEL_FUNCS):
        struct.pack_into("<I", rx, LK_NIDS1 + i * 4, nid)
    for i, nid in enumerate(LK_THREADMGR_FUNCS):
        struct.pack_into("<I", rx, LK_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, LK_IMPORT1 + IMP_LIBRARY_NAME, LK_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, LK_IMPORT1 + IMP_FUNC_NID_TABLE, LK_NIDS1))
    rx_relocs.append(encode_rela(0, 0, LK_IMPORT1 + IMP_FUNC_ENTRY_TABLE, LK_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, LK_IMPORT2 + IMP_LIBRARY_NAME, LK_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, LK_IMPORT2 + IMP_FUNC_NID_TABLE, LK_NIDS2))
    rx_relocs.append(encode_rela(0, 0, LK_IMPORT2 + IMP_FUNC_ENTRY_TABLE, LK_ENTRIES2))

    for i in range(len(LK_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, LK_ENTRIES1 + i * 4, LK_STUB_BASE + i * 12))
    for i in range(len(LK_THREADMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, LK_ENTRIES2 + i * 4,
                                     LK_STUB_BASE + (len(LK_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(LK_RW_SIZE)
    rw[LK_RW_NAME:LK_RW_NAME + 3] = b"rw\0"
    rw[LK_RW_NAME2:LK_RW_NAME2 + 3] = b"lw\0"
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, LK_MODINFO)


# --- SceKernel timer fixture -----------------------------------------------

TM_STUB_BASE = 0x200
TM_LIB1_NAME = 0x280
TM_LIB2_NAME = 0x290
TM_MODINFO = 0x2A0
TM_IMPORT1 = 0x300
TM_IMPORT2 = 0x324
TM_NIDS1 = 0x350
TM_ENTRIES1 = 0x370
TM_NIDS2 = 0x390
TM_ENTRIES2 = 0x3B0
TM_RX_SIZE = 0x3D0

TM_LIB1 = b"SceLibKernel\0"
TM_LIB2 = b"SceThreadmgr\0"
TM_LIB1_NID = 0xCAE9ACE6
TM_LIB2_NID = 0x859A24B1

TM_KERNEL_FUNCS = [
    0x2255B2A5,  # sceKernelCreateTimer
    0x621D293B,  # sceKernelSetTimerEvent
    0x75B1EB3E,  # sceKernelGetTimerEventRemainingTime
    0x9CCF768C,  # sceKernelCancelTimer
]

TM_THREADMGR_FUNCS = [
    0x48091E0C,  # sceKernelStartTimer
    0x869E9F20,  # sceKernelStopTimer
    0x273B4A4D,  # sceKernelSetTimerTimeWide
    0x3EFD3165,  # sceKernelGetTimerTimeWide
    0x5DBC1960,  # sceKernelGetTimerBaseWide
    0xAB1E42C4,  # sceKernelDeleteTimer
]

TM_RW_UID = 0x000
TM_RW_INTERVAL = 0x008
TM_RW_REMAINING = 0x010
TM_RW_NAME = 0x020
TM_RW_SIZE = 0x040


def build_kernel_timer_elf():
    """Guest ELF for timer create/start/set/get/event/cancel/delete."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, TM_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_nonzero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # timer = sceKernelCreateTimer("tmr", 0, NULL)
    movw_movt_rw(0, TM_RW_NAME)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(0)
    movw_movt_rw(1, TM_RW_UID)
    emit(0xE5810000)
    check_nonzero(1)

    # start, set time 5000 us, get time >= 5000
    load_rw(0, TM_RW_UID)
    call(4)
    check(2)
    load_rw(0, TM_RW_UID)
    emit(arm_mov_imm(1, 0))          # padding
    emit(arm_movw(2, 5000))
    emit(arm_mov_imm(3, 0))
    call(6)
    check(4)
    load_rw(0, TM_RW_UID)
    call(7)
    emit(arm_movw(1, 5000))
    emit(0xE1500001)                 # cmp r0, r1
    emit(0x33844000 | arm_encode_imm(8))  # orrlo r4, r4, #8

    # set a 1000 us repeating event and check remaining <= 1000
    load_rw(0, TM_RW_UID)
    emit(arm_mov_imm(1, 0))
    movw_movt_rw(2, TM_RW_INTERVAL)
    emit(arm_mov_imm(3, 1))
    call(1)
    check(16)
    load_rw(0, TM_RW_UID)
    movw_movt_rw(1, TM_RW_REMAINING)
    call(2)
    check(32)
    load_rw(0, TM_RW_REMAINING)
    emit(arm_movw(1, 1000))
    emit(0xE1500001)                 # cmp r0, r1
    emit(0x83844000 | arm_encode_imm(64))  # orrhi r4, r4, #64

    # stop, base time >= 5000, cancel, delete
    load_rw(0, TM_RW_UID)
    call(5)
    check(128)
    load_rw(0, TM_RW_UID)
    call(8)
    emit(arm_movw(1, 5000))
    emit(0xE1500001)
    emit(0x33844000 | arm_encode_imm(256))
    load_rw(0, TM_RW_UID)
    emit(arm_mov_imm(1, 0))
    call(3)
    check(512)
    load_rw(0, TM_RW_UID)
    call(9)
    check(1024)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= TM_STUB_BASE, "timer fixture code overlaps the stubs"

    rx = bytearray(TM_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[TM_LIB1_NAME:TM_LIB1_NAME + len(TM_LIB1)] = TM_LIB1
    rx[TM_LIB2_NAME:TM_LIB2_NAME + len(TM_LIB2)] = TM_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, TM_IMPORT1)
    struct.pack_into("<I", info, 0x30, TM_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[TM_MODINFO:TM_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(TM_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, TM_LIB1_NID)
    rx[TM_IMPORT1:TM_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(TM_THREADMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, TM_LIB2_NID)
    rx[TM_IMPORT2:TM_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(TM_KERNEL_FUNCS):
        struct.pack_into("<I", rx, TM_NIDS1 + i * 4, nid)
    for i, nid in enumerate(TM_THREADMGR_FUNCS):
        struct.pack_into("<I", rx, TM_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, TM_IMPORT1 + IMP_LIBRARY_NAME, TM_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, TM_IMPORT1 + IMP_FUNC_NID_TABLE, TM_NIDS1))
    rx_relocs.append(encode_rela(0, 0, TM_IMPORT1 + IMP_FUNC_ENTRY_TABLE, TM_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, TM_IMPORT2 + IMP_LIBRARY_NAME, TM_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, TM_IMPORT2 + IMP_FUNC_NID_TABLE, TM_NIDS2))
    rx_relocs.append(encode_rela(0, 0, TM_IMPORT2 + IMP_FUNC_ENTRY_TABLE, TM_ENTRIES2))

    for i in range(len(TM_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TM_ENTRIES1 + i * 4, TM_STUB_BASE + i * 12))
    for i in range(len(TM_THREADMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TM_ENTRIES2 + i * 4,
                                     TM_STUB_BASE + (len(TM_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(TM_RW_SIZE)
    rw[TM_RW_NAME:TM_RW_NAME + 4] = b"tmr\0"
    struct.pack_into("<Q", rw, TM_RW_INTERVAL, 1000)
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, TM_MODINFO)


# --- SceKernel thread info fixture -----------------------------------------

TI_STUB_BASE = 0x200
TI_LIB_NAME = 0x270
TI_MODINFO = 0x280
TI_IMPORT = 0x2E0
TI_NIDS = 0x308
TI_ENTRIES = 0x330
TI_RX_SIZE = 0x370

TI_LIB = b"SceLibKernel\0"
TI_LIB_NID = 0xCAE9ACE6

TI_FUNCS = [
    0xC53A9848,  # sceKernelGetThreadCurrentPriority
    0x8D9C5461,  # sceKernelGetThreadInfo
    0xD5DC26C4,  # sceKernelGetThreadExitStatus
    0xD6B01013,  # sceKernelGetThreadRunStatus
    0xE0241FAA,  # sceKernelGetSystemInfo
    0x84092FD0,  # sceKernelCheckThreadStack
    0x36833F93,  # sceKernelChangeCurrentThreadAttr
    0x8C57AC2A,  # sceKernelGetThreadCpuAffinityMask2
]

TI_RW_INFO = 0x000
TI_RW_RUN = 0x080
TI_RW_SYS = 0x0C0
TI_RW_STATUS = 0x110
TI_RW_SIZE = 0x120


def build_thread_info_elf():
    """Guest ELF for the thread information and priority queries."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, TI_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_nonzero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)             # ldr r0, [r1]
        emit(arm_movw(2, value & 0xFFFF))
        emit(0xE1500002)             # cmp r0, r2
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # current priority is nonzero
    call(0)
    check_nonzero(1)

    # thread info for the calling thread: size 0x80, non-empty name
    emit(arm_mov_imm(0, 0))
    movw_movt_rw(1, TI_RW_INFO)
    call(1)
    check(2)
    check_word(TI_RW_INFO, 0x80, 4)
    movw_movt_rw(1, TI_RW_INFO)
    emit(0xE5D10008)                 # ldrb r0, [r1, #8]
    check_nonzero(8)

    # exit status reads back as zero for a running thread
    emit(arm_mov_imm(0, 0))
    movw_movt_rw(1, TI_RW_STATUS)
    call(2)
    check(16)
    load_rw(0, TI_RW_STATUS)
    check(32)

    # run status: size 0x34 and a thread id in cpuInfo[0]
    emit(arm_mov_imm(0, 0))
    movw_movt_rw(1, TI_RW_RUN)
    call(3)
    check(64)
    check_word(TI_RW_RUN, 0x34, 128)
    load_rw(0, TI_RW_RUN + 8)
    check_nonzero(256)

    # system info: size 0x48, active CPU mask 1
    movw_movt_rw(0, TI_RW_SYS)
    call(4)
    check(512)
    check_word(TI_RW_SYS, 0x48, 1024)
    check_word(TI_RW_SYS + 4, 1, 2048)

    call(5)
    check(4096)
    emit(arm_mov_imm(0, 0))
    emit(arm_mov_imm(1, 0))
    call(6)
    check(8192)
    call(7)
    emit(arm_mov_imm(1, 1))
    emit(0xE1500001)                 # cmp r0, r1
    emit(0x13844000 | arm_encode_imm(16384))

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= TI_STUB_BASE, "thread info fixture code overlaps the stubs"

    rx = bytearray(TI_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[TI_LIB_NAME:TI_LIB_NAME + len(TI_LIB)] = TI_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, TI_IMPORT)
    struct.pack_into("<I", info, 0x30, TI_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[TI_MODINFO:TI_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(TI_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, TI_LIB_NID)
    rx[TI_IMPORT:TI_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(TI_FUNCS):
        struct.pack_into("<I", rx, TI_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, TI_IMPORT + IMP_LIBRARY_NAME, TI_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, TI_IMPORT + IMP_FUNC_NID_TABLE, TI_NIDS))
    rx_relocs.append(encode_rela(0, 0, TI_IMPORT + IMP_FUNC_ENTRY_TABLE, TI_ENTRIES))
    for i in range(len(TI_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TI_ENTRIES + i * 4, TI_STUB_BASE + i * 12))

    rw = bytearray(TI_RW_SIZE)
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, TI_MODINFO)


# --- SceKernel event fixture ------------------------------------------------

EV_STUB_BASE = 0x300
EV_LIB1_NAME = 0x380
EV_LIB2_NAME = 0x390
EV_MODINFO = 0x3A0
EV_IMPORT1 = 0x400
EV_IMPORT2 = 0x424
EV_NIDS1 = 0x450
EV_ENTRIES1 = 0x468
EV_NIDS2 = 0x480
EV_ENTRIES2 = 0x490
EV_RX_SIZE = 0x4B0

EV_LIB1 = b"SceLibKernel\0"
EV_LIB2 = b"SceThreadmgr\0"
EV_LIB1_NID = 0xCAE9ACE6
EV_LIB2_NID = 0x859A24B1

EV_KERNEL_FUNCS = [
    0xE6DB2494,  # sceKernelCreateSimpleEvent
    0x120F03AF,  # sceKernelWaitEvent
    0x241F3634,  # sceKernelPollEvent
    0xE5A2AC66,  # sceKernelGetEventPattern
    0xE35F19D6,  # sceKernelGetEventInfo
    0x603AB770,  # sceKernelCancelEvent
]

EV_THREADMGR_FUNCS = [
    0x324218CD,  # sceKernelSetEvent
    0x7B2A4B28,  # sceKernelClearEvent
    0x8D27BAD6,  # sceKernelPulseEvent
    0x208CFE28,  # sceKernelDeleteSimpleEvent
]

EV_RW_UID = 0x000
EV_RW_RESULT = 0x008
EV_RW_USERDATA = 0x010
EV_RW_INFO = 0x020
EV_RW_TIMEOUT = 0x060
EV_RW_NAME = 0x070
EV_RW_SIZE = 0x080


def build_kernel_event_elf():
    """Guest ELF for simple-event set/wait/poll/clear/pulse/info."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, EV_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def check_nonzero(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x03844000 | arm_encode_imm(bit))  # orreq r4, r4, #bit

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)             # ldr r0, [r1]
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # event = sceKernelCreateSimpleEvent("evt", 0, 0, NULL)
    movw_movt_rw(0, EV_RW_NAME)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    call(0)
    movw_movt_rw(1, EV_RW_UID)
    emit(0xE5810000)
    check_nonzero(1)

    # set bits 0 and 2 with user data 0x12345678
    load_rw(0, EV_RW_UID)
    emit(arm_mov_imm(1, 5))
    emit(arm_movw(2, 0x5678))
    emit(arm_movt(2, 0x1234))
    emit(arm_mov_imm(3, 0))
    call(6)
    check(2)

    # poll bit 0 -> result pattern 5, user data 0x12345678
    load_rw(0, EV_RW_UID)
    emit(arm_mov_imm(1, 1))
    movw_movt_rw(2, EV_RW_RESULT)
    movw_movt_rw(3, EV_RW_USERDATA)
    call(2)
    check(4)
    check_word(EV_RW_RESULT, 5, 8)
    check_word(EV_RW_USERDATA, 0x12345678, 16)
    check_word(EV_RW_USERDATA + 4, 0, 32)

    # wait bit 2 (already set) with no timeout
    load_rw(0, EV_RW_UID)
    emit(arm_mov_imm(1, 4))
    movw_movt_rw(2, EV_RW_RESULT)
    movw_movt_rw(3, EV_RW_USERDATA)
    emit(0xE24DD008)                 # sub sp, sp, #8
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    call(1)
    emit(0xE28DD008)
    check(64)

    # clear bit 0, pattern becomes 4
    load_rw(0, EV_RW_UID)
    emit(arm_mov_imm(1, 1))
    call(7)
    check(128)
    load_rw(0, EV_RW_UID)
    movw_movt_rw(1, EV_RW_RESULT)
    call(3)
    check(256)
    check_word(EV_RW_RESULT, 4, 512)

    # wait bit 4 with a 1000 us timeout -> SCE_KERNEL_ERROR_WAIT_TIMEOUT
    load_rw(0, EV_RW_UID)
    movw_movt_rw(12, EV_RW_TIMEOUT)
    emit(arm_movw(1, 1000))
    emit(0xE58C1000)                 # str r1, [r12]
    emit(arm_mov_imm(1, 16))
    movw_movt_rw(2, EV_RW_RESULT)
    movw_movt_rw(3, EV_RW_USERDATA)
    emit(0xE24DD008)                 # sub sp, sp, #8
    emit(0xE58DC000)                 # str r12, [sp]
    call(1)
    emit(0xE28DD008)
    emit(arm_movw(1, 0x8005))
    emit(arm_movt(1, 0x8002))
    emit(0xE1500001)
    emit(0x13844000 | arm_encode_imm(1024))

    # pulse bit 3, resulting pattern is 0xC
    load_rw(0, EV_RW_UID)
    emit(arm_mov_imm(1, 8))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    call(8)
    check(2048)
    load_rw(0, EV_RW_UID)
    movw_movt_rw(1, EV_RW_RESULT)
    call(3)
    check(4096)
    check_word(EV_RW_RESULT, 12, 8192)

    # info: size 0x40 and pattern 0xC
    load_rw(0, EV_RW_UID)
    movw_movt_rw(1, EV_RW_INFO)
    call(4)
    check(16384)
    check_word(EV_RW_INFO, 0x40, 32768)
    check_word(EV_RW_INFO + 44, 12, 65536)

    load_rw(0, EV_RW_UID)
    call(5)
    check(131072)
    load_rw(0, EV_RW_UID)
    call(9)
    check(262144)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= EV_STUB_BASE, "event fixture code overlaps the stubs"

    rx = bytearray(EV_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[EV_LIB1_NAME:EV_LIB1_NAME + len(EV_LIB1)] = EV_LIB1
    rx[EV_LIB2_NAME:EV_LIB2_NAME + len(EV_LIB2)] = EV_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, EV_IMPORT1)
    struct.pack_into("<I", info, 0x30, EV_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[EV_MODINFO:EV_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(EV_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, EV_LIB1_NID)
    rx[EV_IMPORT1:EV_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(EV_THREADMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, EV_LIB2_NID)
    rx[EV_IMPORT2:EV_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(EV_KERNEL_FUNCS):
        struct.pack_into("<I", rx, EV_NIDS1 + i * 4, nid)
    for i, nid in enumerate(EV_THREADMGR_FUNCS):
        struct.pack_into("<I", rx, EV_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, EV_IMPORT1 + IMP_LIBRARY_NAME, EV_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, EV_IMPORT1 + IMP_FUNC_NID_TABLE, EV_NIDS1))
    rx_relocs.append(encode_rela(0, 0, EV_IMPORT1 + IMP_FUNC_ENTRY_TABLE, EV_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, EV_IMPORT2 + IMP_LIBRARY_NAME, EV_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, EV_IMPORT2 + IMP_FUNC_NID_TABLE, EV_NIDS2))
    rx_relocs.append(encode_rela(0, 0, EV_IMPORT2 + IMP_FUNC_ENTRY_TABLE, EV_ENTRIES2))

    for i in range(len(EV_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, EV_ENTRIES1 + i * 4, EV_STUB_BASE + i * 12))
    for i in range(len(EV_THREADMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, EV_ENTRIES2 + i * 4,
                                     EV_STUB_BASE + (len(EV_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(EV_RW_SIZE)
    rw[EV_RW_NAME:EV_RW_NAME + 4] = b"evt\0"
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, EV_MODINFO)


# --- Thread callbacks and message pipes ------------------------------------

TE_CALLBACK_OFFSET = 0x380
TE_STUB_BASE = 0x3C0
TE_LIB1_NAME = 0x460
TE_LIB2_NAME = 0x470
TE_MODINFO = 0x480
TE_IMPORT1 = 0x4E0
TE_IMPORT2 = 0x504
TE_NIDS1 = 0x530
TE_ENTRIES1 = 0x548
TE_NIDS2 = 0x560
TE_ENTRIES2 = 0x588
TE_RX_SIZE = 0x5C0

TE_LIB1 = b"SceLibKernel\0"
TE_LIB2 = b"SceThreadmgr\0"
TE_LIB1_NID = 0xCAE9ACE6
TE_LIB2_NID = 0x859A24B1

TE_KERNEL_FUNCS = [
    0x0A10C1C8,  # sceKernelCreateMsgPipe
    0x94D506F7,  # sceKernelSendMsgPipeVector
    0x4E81DD5C,  # sceKernelReceiveMsgPipe
    0x5615B006,  # sceKernelTryReceiveMsgPipe
    0xEF14BA37,  # sceKernelCancelMsgPipe
    0x88DD1BC8,  # sceKernelGetCallbackInfo
]

TE_THREADMGR_FUNCS = [
    0xB19CF7E9,  # sceKernelCreateCallback
    0xA4683592,  # sceKernelNotifyCallback
    0xE53E41F6,  # sceKernelCheckCallback
    0x038644D5,  # sceKernelGetCallbackCount
    0xD469676B,  # sceKernelDeleteCallback
    0x4046D16B,  # sceKernelGetMsgPipeInfo
    0x0E1CB9F6,  # sceKernelOpenMsgPipe
    0x1305A065,  # sceKernelCloseMsgPipe
    0xE78BCCF7,  # sceKernelDeleteMsgPipe
]

TE_RW_CB_ID = 0x000
TE_RW_MP_ID = 0x004
TE_RW_RESULT = 0x008
TE_RW_CALLBACK_DATA = 0x010
TE_RW_CB_INFO = 0x020
TE_RW_MP_INFO = 0x060
TE_RW_VECTOR = 0x0A0
TE_RW_BUFFER = 0x0C0
TE_RW_CB_NAME = 0x100
TE_RW_MP_NAME = 0x104
TE_RW_PART1 = 0x108
TE_RW_PART2 = 0x10C
TE_RW_SIZE = 0x120


def build_thread_extra_elf():
    """Guest ELF for thread callbacks and the message-pipe vector family."""
    code = []
    rx_relocs = []
    callback_offset = TE_CALLBACK_OFFSET

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt(rd, offset, symseg):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, symseg, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, symseg, base + 4, offset, R_ARM_MOVT_ABS))

    def movw_movt_rw(rd, offset):
        movw_movt(rd, offset, 1)

    def movw_movt_code(rd, offset):
        movw_movt(rd, offset, 0)

    def call(index):
        emit(arm_bl(len(code) * 4, TE_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def store_rw(rs, offset):
        movw_movt_rw(12, offset)
        emit(0xE58C0000 | (rs << 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_nonzero(bit):
        emit(0xE3500000)
        emit(0x03844000 | arm_encode_imm(bit))

    def check_negative(bit):
        emit(0xE3500000)
        emit(0xA3844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_word_pointer(offset, pointer_offset, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        movw_movt_rw(2, pointer_offset)
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))

    # sceKernelCreateCallback("cb", 0, guest_callback, &callback_data)
    movw_movt_rw(0, TE_RW_CB_NAME)
    emit(arm_mov_imm(1, 0))
    movw_movt_code(2, callback_offset)
    movw_movt_rw(3, TE_RW_CALLBACK_DATA)
    call(6)
    store_rw(0, TE_RW_CB_ID)
    check_nonzero(1)

    # notify with argument 0x1234, then run the callback
    load_rw(0, TE_RW_CB_ID)
    emit(arm_movw(1, 0x1234))
    call(7)
    check(2)
    call(8)
    check_nonzero(4)
    check_word(TE_RW_CALLBACK_DATA, 0x1234, 8)
    check_word(TE_RW_CALLBACK_DATA + 4, 1, 16)

    # pending count is reset after the callback ran
    load_rw(0, TE_RW_CB_ID)
    call(9)
    emit(0xE3500000)
    emit(0x13844000 | arm_encode_imm(32))

    # callback info: size and callbackId
    load_rw(0, TE_RW_CB_ID)
    movw_movt_rw(1, TE_RW_CB_INFO)
    call(5)
    check(64)
    check_word(TE_RW_CB_INFO, 0x3C, 128)
    movw_movt_rw(1, TE_RW_CB_INFO + 4)
    emit(0xE5911000)
    movw_movt_rw(2, TE_RW_CB_ID)
    emit(0xE5922000)
    emit(0xE1510002)
    emit(0x13844000 | arm_encode_imm(256))

    # delete the callback
    load_rw(0, TE_RW_CB_ID)
    call(10)
    check(512)

    # msgpipe: 256-byte buffer, send two vector pieces ("AB" + "CD")
    movw_movt_rw(0, TE_RW_MP_NAME)
    emit(arm_mov_imm(1, 0x40))
    emit(arm_mov_imm(2, 12))
    emit(arm_movw(3, 0x100))
    emit(0xE24DD008)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    call(0)
    emit(0xE28DD008)
    store_rw(0, TE_RW_MP_ID)
    check_nonzero(1024)

    load_rw(0, TE_RW_MP_ID)
    movw_movt_rw(1, TE_RW_VECTOR)
    emit(arm_mov_imm(2, 2))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)
    movw_movt_rw(12, TE_RW_RESULT)
    emit(0xE58DC000)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC004)
    call(1)
    emit(0xE28DD008)
    check(2048)
    check_word(TE_RW_RESULT, 4, 4096)

    # receive four bytes and verify the stream order
    load_rw(0, TE_RW_MP_ID)
    movw_movt_rw(1, TE_RW_BUFFER)
    emit(arm_mov_imm(2, 4))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)
    movw_movt_rw(12, TE_RW_RESULT)
    emit(0xE58DC000)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC004)
    call(2)
    emit(0xE28DD008)
    check(8192)
    check_word(TE_RW_RESULT, 4, 16384)
    check_word(TE_RW_BUFFER, 0x44434241, 32768)

    # an empty pipe must fail the non-blocking receive
    load_rw(0, TE_RW_MP_ID)
    movw_movt_rw(1, TE_RW_BUFFER)
    emit(arm_mov_imm(2, 4))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    call(3)
    emit(0xE28DD008)
    check_negative(65536)

    # message-pipe info reports the full buffer as free again
    load_rw(0, TE_RW_MP_ID)
    movw_movt_rw(1, TE_RW_MP_INFO)
    call(11)
    check(131072)
    check_word(TE_RW_MP_INFO, 0x3C, 262144)
    check_word(TE_RW_MP_INFO + 44, 0x100, 524288)
    check_word(TE_RW_MP_INFO + 48, 0x100, 1048576)

    # open/close by name, cancel, then delete
    movw_movt_rw(0, TE_RW_MP_NAME)
    call(12)
    check_nonzero(2097152)
    load_rw(0, TE_RW_MP_ID)
    call(13)
    check(4194304)
    load_rw(0, TE_RW_MP_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(4)
    check(8388608)
    load_rw(0, TE_RW_MP_ID)
    call(14)
    check(16777216)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= callback_offset, f"thread fixture code overlaps the callback ({len(code) * 4:#x})"
    assert callback_offset + 16 <= TE_STUB_BASE, "callback overlaps the stubs"

    rx = bytearray(TE_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    # Guest callback: store (arg, count) through userData and return 0.
    callback_code = [
        0xE5832000,                 # str r2, [r3]
        0xE5831004,                 # str r1, [r3, #4]
        0xE3A00000,                 # mov r0, #0
        0xE12FFF1E,                 # bx lr
    ]
    rx[callback_offset:callback_offset + len(callback_code) * 4] = b"".join(
        struct.pack("<I", w) for w in callback_code)
    rx[TE_LIB1_NAME:TE_LIB1_NAME + len(TE_LIB1)] = TE_LIB1
    rx[TE_LIB2_NAME:TE_LIB2_NAME + len(TE_LIB2)] = TE_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, TE_IMPORT1)
    struct.pack_into("<I", info, 0x30, TE_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[TE_MODINFO:TE_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(TE_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, TE_LIB1_NID)
    rx[TE_IMPORT1:TE_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(TE_THREADMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, TE_LIB2_NID)
    rx[TE_IMPORT2:TE_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(TE_KERNEL_FUNCS):
        struct.pack_into("<I", rx, TE_NIDS1 + i * 4, nid)
    for i, nid in enumerate(TE_THREADMGR_FUNCS):
        struct.pack_into("<I", rx, TE_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, TE_IMPORT1 + IMP_LIBRARY_NAME, TE_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, TE_IMPORT1 + IMP_FUNC_NID_TABLE, TE_NIDS1))
    rx_relocs.append(encode_rela(0, 0, TE_IMPORT1 + IMP_FUNC_ENTRY_TABLE, TE_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, TE_IMPORT2 + IMP_LIBRARY_NAME, TE_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, TE_IMPORT2 + IMP_FUNC_NID_TABLE, TE_NIDS2))
    rx_relocs.append(encode_rela(0, 0, TE_IMPORT2 + IMP_FUNC_ENTRY_TABLE, TE_ENTRIES2))

    for i in range(len(TE_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TE_ENTRIES1 + i * 4, TE_STUB_BASE + i * 12))
    for i in range(len(TE_THREADMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TE_ENTRIES2 + i * 4,
                                     TE_STUB_BASE + (len(TE_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(TE_RW_SIZE)
    rw[TE_RW_CB_NAME:TE_RW_CB_NAME + 3] = b"cb\0"
    rw[TE_RW_MP_NAME:TE_RW_MP_NAME + 3] = b"mp\0"
    rw[TE_RW_PART1:TE_RW_PART1 + 2] = b"AB"
    rw[TE_RW_PART2:TE_RW_PART2 + 2] = b"CD"
    struct.pack_into("<II", rw, TE_RW_VECTOR, 0, 2)
    struct.pack_into("<II", rw, TE_RW_VECTOR + 8, 0, 2)
    rw_relocs = [
        encode_rela(1, 1, TE_RW_VECTOR, TE_RW_PART1),
        encode_rela(1, 1, TE_RW_VECTOR + 8, TE_RW_PART2),
    ]
    rela = bytearray(b"".join(rx_relocs) + b"".join(rw_relocs))
    return assemble_elf(rx, rw, rela, TE_MODINFO)


# --- Module manager fixture ------------------------------------------------

MM_STUB_BASE = 0x200
MM_LIB1_NAME = 0x260
MM_LIB2_NAME = 0x270
MM_MODINFO = 0x280
MM_IMPORT1 = 0x2E0
MM_IMPORT2 = 0x304
MM_NIDS1 = 0x330
MM_ENTRIES1 = 0x33C
MM_NIDS2 = 0x348
MM_ENTRIES2 = 0x34C
MM_RX_SIZE = 0x360

MM_LIB1 = b"SceLibKernel\0"
MM_LIB2 = b"SceModulemgr\0"
MM_LIB1_NID = 0xCAE9ACE6
MM_LIB2_NID = 0xEAED1616

MM_LIBKERNEL_FUNCS = [
    0x2DCC4AFA,  # sceKernelLoadStartModule
    0x3B2CBA09,  # sceKernelStopModule
    0x1987920E,  # sceKernelUnloadModule
]

MM_MODULEMGR_FUNCS = [
    0x36585DAF,  # sceKernelGetModuleInfo
]

MM_RW_ID = 0x000
MM_RW_STATUS = 0x004
MM_RW_INFO = 0x010
MM_RW_PATH = 0x1D0
MM_RW_SIZE = 0x200
MM_PATH = b"app0:/mod.suprx\0"
MM_MODULE_FILE = b"mod.suprx\0"

MM_MODULE_INFO = 0x100
MM_MODULE_RX_SIZE = 0x200
MM_MODULE_RW_SIZE = 0x100


def build_module_file():
    """Minimal PRX-style module: module_start returns 0x2A, stop returns 0x2B."""
    rx = bytearray(MM_MODULE_RX_SIZE)
    rx[0:12] = b"".join(struct.pack("<I", w) for w in (
        arm_mov_imm(0, 0x2A),
        0xE12FFF1E,  # bx lr
        arm_mov_imm(0, 0x2B),
    ))
    rx[0x20:0x28] = b"".join(struct.pack("<I", w) for w in (
        arm_mov_imm(0, 0x2B),
        0xE12FFF1E,  # bx lr
    ))

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x08] = b"mod\0"
    info[0x1F] = 0x06
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)  # module_start offset
    struct.pack_into("<I", info, 0x48, 0x00000020)  # module_stop offset
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[MM_MODULE_INFO:MM_MODULE_INFO + MODULE_INFO_SIZE] = info

    return assemble_elf(rx, bytearray(MM_MODULE_RW_SIZE), b"", MM_MODULE_INFO)


def build_module_host_elf():
    """Guest ELF that loads, starts, inspects, stops and unloads mod.suprx."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, MM_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def store_rw(rs, offset):
        movw_movt_rw(12, offset)
        emit(0xE58C0000 | (rs << 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_nonzero(bit):
        emit(0xE3500000)
        emit(0x03844000 | arm_encode_imm(bit))

    def check_negative(bit):
        emit(0xE3500000)
        emit(0xA3844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))

    # sceKernelLoadStartModule("app0:/mod.suprx", 0, NULL, 0, NULL, &status)
    movw_movt_rw(0, MM_RW_PATH)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    movw_movt_rw(12, MM_RW_STATUS)
    emit(0xE58DC004)
    call(0)
    emit(0xE28DD008)
    store_rw(0, MM_RW_ID)
    check_nonzero(1)
    check_word(MM_RW_STATUS, 0x2A, 2)

    # sceKernelGetModuleInfo(modid, &info)
    load_rw(0, MM_RW_ID)
    movw_movt_rw(1, MM_RW_INFO)
    call(3)
    check(4)
    check_word(MM_RW_INFO, 0x1B8, 8)
    movw_movt_rw(1, MM_RW_INFO + 4)
    emit(0xE5911000)
    load_rw(2, MM_RW_ID)
    emit(0xE1510002)
    emit(0x13844000 | arm_encode_imm(16))
    check_word(MM_RW_INFO + 12, 0x00646F6D, 32)   # "mod"
    check_word(MM_RW_INFO + 84, 0x30707061, 64)   # "app0"
    check_word(MM_RW_INFO + 88, 0x6F6D2F3A, 128)  # ":/mo"
    movw_movt_rw(1, MM_RW_INFO + 44)
    emit(0xE5910000)
    check_nonzero(256)

    # sceKernelStopModule(modid, 0, NULL, 0, NULL, &status)
    load_rw(0, MM_RW_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD008)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    movw_movt_rw(12, MM_RW_STATUS)
    emit(0xE58DC004)
    call(1)
    emit(0xE28DD008)
    check(512)
    check_word(MM_RW_STATUS, 0x2B, 1024)

    # unload and confirm the handle is gone
    load_rw(0, MM_RW_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(2)
    check(2048)
    load_rw(0, MM_RW_ID)
    movw_movt_rw(1, MM_RW_INFO)
    call(3)
    check_negative(4096)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= 0x200, f"module fixture code overlaps the stubs ({len(code) * 4:#x})"

    rx = bytearray(MM_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[MM_LIB1_NAME:MM_LIB1_NAME + len(MM_LIB1)] = MM_LIB1
    rx[MM_LIB2_NAME:MM_LIB2_NAME + len(MM_LIB2)] = MM_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, MM_IMPORT1)
    struct.pack_into("<I", info, 0x30, MM_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[MM_MODINFO:MM_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(MM_LIBKERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, MM_LIB1_NID)
    rx[MM_IMPORT1:MM_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(MM_MODULEMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, MM_LIB2_NID)
    rx[MM_IMPORT2:MM_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(MM_LIBKERNEL_FUNCS):
        struct.pack_into("<I", rx, MM_NIDS1 + i * 4, nid)
    for i, nid in enumerate(MM_MODULEMGR_FUNCS):
        struct.pack_into("<I", rx, MM_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, MM_IMPORT1 + IMP_LIBRARY_NAME, MM_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, MM_IMPORT1 + IMP_FUNC_NID_TABLE, MM_NIDS1))
    rx_relocs.append(encode_rela(0, 0, MM_IMPORT1 + IMP_FUNC_ENTRY_TABLE, MM_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, MM_IMPORT2 + IMP_LIBRARY_NAME, MM_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, MM_IMPORT2 + IMP_FUNC_NID_TABLE, MM_NIDS2))
    rx_relocs.append(encode_rela(0, 0, MM_IMPORT2 + IMP_FUNC_ENTRY_TABLE, MM_ENTRIES2))

    for i in range(len(MM_LIBKERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, MM_ENTRIES1 + i * 4, MM_STUB_BASE + i * 12))
    for i in range(len(MM_MODULEMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, MM_ENTRIES2 + i * 4,
                                     MM_STUB_BASE + (len(MM_LIBKERNEL_FUNCS) + i) * 12))

    rw = bytearray(MM_RW_SIZE)
    rw[MM_RW_PATH:MM_RW_PATH + len(MM_PATH)] = MM_PATH
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, MM_MODINFO)


# --- Module exports fixture ------------------------------------------------

ME_STUB_BASE = 0x180
ME_LIB_NAME = 0x1E0
ME_MODINFO = 0x200
ME_IMPORT = 0x260
ME_NIDS = 0x290
ME_ENTRIES = 0x298
ME_RX_SIZE = 0x2C0

ME_LIB = b"SceLibKernel\0"
ME_LIB_NID = 0xCAE9ACE6
ME_FUNCS = [
    0x2DCC4AFA,  # sceKernelLoadStartModule
    0x1987920E,  # sceKernelUnloadModule
]

ME_RW_LIB_ID = 0x000
ME_RW_IMP_ID = 0x004
ME_RW_STATUS = 0x008
ME_RW_LIB_PATH = 0x010
ME_RW_IMP_PATH = 0x020
ME_RW_SIZE = 0x040
ME_LIB_PATH = b"app0:/lib.suprx\0"
ME_IMP_PATH = b"app0:/imp.suprx\0"

ME_LIB_MODINFO = 0x80
ME_IMP_MODINFO = 0xC0
ME_MODULE_RX_SIZE = 0x140


def build_export_library():
    """Module exporting one function (NID 0x11223344) that returns 0x77."""
    rx = bytearray(ME_MODULE_RX_SIZE)
    rx[0x00:0x08] = b"".join(struct.pack("<I", w) for w in (
        arm_mov_imm(0, 0x2A),
        0xE12FFF1E,
    ))
    rx[0x20:0x28] = b"".join(struct.pack("<I", w) for w in (
        arm_mov_imm(0, 0x77),
        0xE12FFF1E,
    ))

    struct.pack_into("<I", rx, 0x30, 0x99AABBCC)   # exported variable

    export = bytearray(0x20)
    struct.pack_into("<HHHHI", export, 0, 0x20, 1, 1, 1, 1)
    struct.pack_into("<I", export, 16, 0x12345678)  # library NID
    rx[0x40:0x60] = export
    struct.pack_into("<I", rx, 0x60, 0x11223344)   # function NID
    struct.pack_into("<I", rx, 0x64, 0x55667788)   # variable NID
    rx[0x70:0x7B] = b"SceTestLib\0"

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x08] = b"lib\0"
    info[0x1F] = 0x06
    struct.pack_into("<I", info, 0x24, 0x40)  # export_top
    struct.pack_into("<I", info, 0x28, 0x60)  # export_end
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    rx[ME_LIB_MODINFO:ME_LIB_MODINFO + MODULE_INFO_SIZE] = info

    rela = bytearray()
    rela += encode_rela(0, 0, 0x40 + 20, 0x70)  # library_name
    rela += encode_rela(0, 0, 0x40 + 24, 0x60)  # nid_table
    rela += encode_rela(0, 0, 0x40 + 28, 0x68)  # entry_table
    rela += encode_rela(0, 0, 0x68, 0x20)       # exported function
    rela += encode_rela(0, 0, 0x6C, 0x30)       # exported variable
    return assemble_elf(rx, bytearray(0x40), rela, ME_LIB_MODINFO)


def build_import_module():
    """Module importing SceTestLib:0x11223344 and checking its return value."""
    rx = bytearray(ME_MODULE_RX_SIZE)
    code = [
        0xE92D4010,          # push {r4, lr}
        arm_bl(0x04, 0x60),
        0xE3500077,          # cmp r0, #0x77
        0x13A04001,          # movne r4, #1
        0x03A04000,          # moveq r4, #0
        arm_movw(1, 0),
        arm_movt(1, 0),
        0xE5911000,          # ldr r1, [r1]
        0xE5910000,          # ldr r0, [r1]
        arm_movw(2, 0xBBCC),
        arm_movt(2, 0x99AA),
        0xE1500002,          # cmp r0, r2
        0x13844002,          # orrne r4, r4, #2
        0xE3540000,          # cmp r4, #0
        0x03A0002A,          # moveq r0, #0x2a
        0x13A00001,          # movne r0, #1
        0xE8BD8010,          # pop {r4, pc}
    ]
    rx[0x00:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, 1, 1, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, 0x12345678)
    rx[0x70:0x70 + IMPORT_SIZE] = imp
    rx[0x98:0xA2] = b"SceTestLib\0"
    struct.pack_into("<I", rx, 0xA8, 0x11223344)
    struct.pack_into("<I", rx, 0xB0, 0x55667788)

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x08] = b"imp\0"
    info[0x1F] = 0x06
    struct.pack_into("<I", info, 0x2C, 0x70)  # import_top
    struct.pack_into("<I", info, 0x30, 0x70 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    rx[ME_IMP_MODINFO:ME_IMP_MODINFO + MODULE_INFO_SIZE] = info

    rela = bytearray()
    rela += encode_rela(0, 0, 0x70 + IMP_LIBRARY_NAME, 0x98)
    rela += encode_rela(0, 0, 0x70 + IMP_FUNC_NID_TABLE, 0xA8)
    rela += encode_rela(0, 0, 0x70 + IMP_FUNC_ENTRY_TABLE, 0xAC)
    rela += encode_rela(0, 0, 0x70 + IMP_VAR_NID_TABLE, 0xB0)
    rela += encode_rela(0, 0, 0x70 + IMP_VAR_ENTRY_TABLE, 0xB4)
    rela += encode_rela(0, 0, 0xAC, 0x60)  # import stub
    # movw/movt r1, <address of the imported variable slot>
    rela += encode_rela(0, 0, 0x14, 0xB4, R_ARM_MOVW_ABS_NC)
    rela += encode_rela(0, 0, 0x18, 0xB4, R_ARM_MOVT_ABS)
    return assemble_elf(rx, bytearray(0x40), rela, 0xC0)


def build_module_export_host_elf():
    """Guest ELF that loads lib.suprx then imp.suprx and unloads both."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, ME_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def store_rw(rs, offset):
        movw_movt_rw(12, offset)
        emit(0xE58C0000 | (rs << 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_nonzero(bit):
        emit(0xE3500000)
        emit(0x03844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    def load_start(path_offset, id_offset, bit):
        movw_movt_rw(0, path_offset)
        emit(arm_mov_imm(1, 0))
        emit(arm_mov_imm(2, 0))
        emit(arm_mov_imm(3, 0))
        emit(0xE24DD008)
        emit(arm_mov_imm(12, 0))
        emit(0xE58DC000)
        movw_movt_rw(12, ME_RW_STATUS)
        emit(0xE58DC004)
        call(0)
        emit(0xE28DD008)
        store_rw(0, id_offset)
        check_nonzero(bit)
        check_word(ME_RW_STATUS, 0x2A, bit << 1)

    emit(0xE92D4010)
    emit(arm_mov_imm(4, 0))

    load_start(ME_RW_LIB_PATH, ME_RW_LIB_ID, 1)
    load_start(ME_RW_IMP_PATH, ME_RW_IMP_ID, 4)

    load_rw(0, ME_RW_IMP_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(1)
    check(16)
    load_rw(0, ME_RW_LIB_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(1)
    check(32)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= ME_STUB_BASE, f"module export fixture code overlaps stubs ({len(code) * 4:#x})"

    rx = bytearray(ME_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[ME_LIB_NAME:ME_LIB_NAME + len(ME_LIB)] = ME_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, ME_IMPORT)
    struct.pack_into("<I", info, 0x30, ME_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[ME_MODINFO:ME_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(ME_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, ME_LIB_NID)
    rx[ME_IMPORT:ME_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(ME_FUNCS):
        struct.pack_into("<I", rx, ME_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, ME_IMPORT + IMP_LIBRARY_NAME, ME_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, ME_IMPORT + IMP_FUNC_NID_TABLE, ME_NIDS))
    rx_relocs.append(encode_rela(0, 0, ME_IMPORT + IMP_FUNC_ENTRY_TABLE, ME_ENTRIES))
    for i in range(len(ME_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, ME_ENTRIES + i * 4, ME_STUB_BASE + i * 12))

    rw = bytearray(ME_RW_SIZE)
    rw[ME_RW_LIB_PATH:ME_RW_LIB_PATH + len(ME_LIB_PATH)] = ME_LIB_PATH
    rw[ME_RW_IMP_PATH:ME_RW_IMP_PATH + len(ME_IMP_PATH)] = ME_IMP_PATH
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, ME_MODINFO)


# --- SceSysmem fixture -----------------------------------------------------

SM_STUB_BASE = 0x280
SM_LIB_NAME = 0x330
SM_MODINFO = 0x340
SM_IMPORT = 0x3A0
SM_NIDS = 0x3D0
SM_ENTRIES = 0x400
SM_RX_SIZE = 0x440

SM_LIB = b"SceSysmem\0"
SM_LIB_NID = 0x37FE725A
SM_FUNCS = [
    0xB9D5EBDE,  # sceKernelAllocMemBlock
    0xB8EF5818,  # sceKernelGetMemBlockBase
    0xA33B99D1,  # sceKernelFindMemBlockByAddr
    0x4010AD65,  # sceKernelGetMemBlockInfoByAddr
    0x006F3DB4,  # sceKernelGetMemBlockInfoByRange
    0x8EB8DFBB,  # sceKernelOpenMemBlock
    0xB680E3A0,  # sceKernelCloseMemBlock
    0xA91E15EE,  # sceKernelFreeMemBlock
    0xE2D7E137,  # sceKernelAllocMemBlockForVM
    0x4EA13FEA,  # sceKernelFreeMemBlockForVM
    0x19D2A81A,  # sceKernelSyncVMDomain
]

SM_RW_UID = 0x000
SM_RW_BASE = 0x004
SM_RW_INFO = 0x008
SM_RW_VM_UID = 0x020
SM_RW_NAME = 0x040
SM_RW_VM_NAME = 0x048
SM_RW_SIZE = 0x060
SM_NAME = b"sysmem-test\0"
SM_VM_NAME = b"vm-test\0"
SM_TYPE_USER_RW = 0x0C20D060


def build_sysmem_elf():
    """Guest ELF for the memory-block query, VM and open-by-name surface."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, SM_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def store_rw(rs, offset):
        movw_movt_rw(12, offset)
        emit(0xE58C0000 | (rs << 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_nonzero(bit):
        emit(0xE3500000)
        emit(0x03844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_field_equals_id(field_offset, id_offset, bit):
        movw_movt_rw(1, field_offset)
        emit(0xE5911000)
        load_rw(2, id_offset)
        emit(0xE1510002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)
    emit(arm_mov_imm(4, 0))

    # alloc a 4 KiB USER_RW block
    movw_movt_rw(0, SM_RW_NAME)
    emit(arm_movw(1, SM_TYPE_USER_RW & 0xFFFF))
    emit(arm_movt(1, (SM_TYPE_USER_RW >> 16) & 0xFFFF))
    emit(arm_movw(2, 0x1000))
    emit(arm_mov_imm(3, 0))
    call(0)
    store_rw(0, SM_RW_UID)
    check_nonzero(1)

    # base, find-by-addr, info-by-addr
    load_rw(0, SM_RW_UID)
    movw_movt_rw(1, SM_RW_BASE)
    call(1)
    check(2)
    load_rw(0, SM_RW_BASE)
    emit(arm_mov_imm(1, 0))
    call(2)
    check_nonzero(4)
    load_rw(0, SM_RW_BASE)
    movw_movt_rw(1, SM_RW_INFO)
    call(3)
    check(8)
    check_word(SM_RW_INFO, 0x18, 16)
    check_word(SM_RW_INFO + 8, 0x1000, 32)
    check_word(SM_RW_INFO + 12, 0xD0, 64)
    check_word(SM_RW_INFO + 20, SM_TYPE_USER_RW, 128)
    check_field_equals_id(SM_RW_INFO + 4, SM_RW_BASE, 256)

    # range query covers the first half of the block
    load_rw(0, SM_RW_BASE)
    emit(arm_movw(1, 0x800))
    movw_movt_rw(2, SM_RW_INFO)
    call(4)
    check(512)
    check_word(SM_RW_INFO + 8, 0x1000, 1024)

    # open by name, close, sync VM
    movw_movt_rw(0, SM_RW_NAME)
    emit(arm_mov_imm(1, 0))
    call(5)
    check_nonzero(2048)
    load_rw(0, SM_RW_UID)
    call(6)
    check(4096)
    load_rw(0, SM_RW_UID)
    load_rw(1, SM_RW_BASE)
    emit(arm_movw(2, 0x1000))
    call(10)
    check(8192)

    # VM allocation helper
    movw_movt_rw(0, SM_RW_VM_NAME)
    emit(arm_movw(1, 0x1000))
    call(8)
    store_rw(0, SM_RW_VM_UID)
    check_nonzero(16384)
    load_rw(0, SM_RW_VM_UID)
    call(9)
    check(32768)

    load_rw(0, SM_RW_UID)
    call(7)
    check(65536)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= SM_STUB_BASE, f"sysmem fixture code overlaps stubs ({len(code) * 4:#x})"

    rx = bytearray(SM_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[SM_LIB_NAME:SM_LIB_NAME + len(SM_LIB)] = SM_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, SM_IMPORT)
    struct.pack_into("<I", info, 0x30, SM_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[SM_MODINFO:SM_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(SM_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, SM_LIB_NID)
    rx[SM_IMPORT:SM_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(SM_FUNCS):
        struct.pack_into("<I", rx, SM_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, SM_IMPORT + IMP_LIBRARY_NAME, SM_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, SM_IMPORT + IMP_FUNC_NID_TABLE, SM_NIDS))
    rx_relocs.append(encode_rela(0, 0, SM_IMPORT + IMP_FUNC_ENTRY_TABLE, SM_ENTRIES))
    for i in range(len(SM_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, SM_ENTRIES + i * 4, SM_STUB_BASE + i * 12))

    rw = bytearray(SM_RW_SIZE)
    rw[SM_RW_NAME:SM_RW_NAME + len(SM_NAME)] = SM_NAME
    rw[SM_RW_VM_NAME:SM_RW_VM_NAME + len(SM_VM_NAME)] = SM_VM_NAME
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, SM_MODINFO)


# --- Thread event handler fixture ------------------------------------------

TH_STUB_BASE = 0x240
TH_LIB1_NAME = 0x290
TH_LIB2_NAME = 0x2A0
TH_MODINFO = 0x2B0
TH_IMPORT1 = 0x310
TH_IMPORT2 = 0x334
TH_NIDS1 = 0x360
TH_ENTRIES1 = 0x374
TH_NIDS2 = 0x384
TH_ENTRIES2 = 0x388
TH_RX_SIZE = 0x3A0

TH_LIB1 = b"SceLibKernel\0"
TH_LIB2 = b"SceThreadmgr\0"
TH_LIB1_NID = 0xCAE9ACE6
TH_LIB2_NID = 0x859A24B1

TH_KERNEL_FUNCS = [
    0x6D8C0F13,  # sceKernelRegisterThreadEventHandler
    0xC5C11EE7,  # sceKernelCreateThread
    0xF08DE149,  # sceKernelStartThread
    0xDDB395A9,  # sceKernelWaitThreadEnd
]

TH_THREADMGR_FUNCS = [
    0x2C8ED6F0,  # sceKernelUnregisterThreadEventHandler
]

TH_HANDLER_OFFSET = 0x200
TH_CHILD_OFFSET = 0x1E0

TH_RW_HANDLER_ID = 0x000
TH_RW_THREAD_ID = 0x004
TH_RW_STATUS = 0x008
TH_RW_EVENT_DATA = 0x010
TH_RW_HANDLER_NAME = 0x030
TH_RW_THREAD_NAME = 0x038
TH_RW_SIZE = 0x050
TH_HANDLER_NAME = b"thandler\0"
TH_THREAD_NAME = b"tchild\0"


def build_thread_event_elf():
    """Guest ELF exercising thread start/end handler dispatch."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt(rd, offset, symseg):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, symseg, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, symseg, base + 4, offset, R_ARM_MOVT_ABS))

    def movw_movt_rw(rd, offset):
        movw_movt(rd, offset, 1)

    def movw_movt_code(rd, offset):
        movw_movt(rd, offset, 0)

    def call(index):
        emit(arm_bl(len(code) * 4, TH_STUB_BASE + index * 12))

    def load_rw(rd, offset):
        movw_movt_rw(rd, offset)
        emit(0xE5900000 | (rd << 16) | (rd << 12))

    def store_rw(rs, offset):
        movw_movt_rw(12, offset)
        emit(0xE58C0000 | (rs << 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_nonzero(bit):
        emit(0xE3500000)
        emit(0x03844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)
    emit(arm_mov_imm(4, 0))

    # register start/end handler
    movw_movt_rw(0, TH_RW_HANDLER_NAME)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0x0C))
    movw_movt_code(3, TH_HANDLER_OFFSET)
    emit(0xE24DD008)
    movw_movt_rw(12, TH_RW_EVENT_DATA)
    emit(0xE58DC000)
    call(0)
    emit(0xE28DD008)
    store_rw(0, TH_RW_HANDLER_ID)
    check_nonzero(1)

    # create and start a child thread
    movw_movt_rw(0, TH_RW_THREAD_NAME)
    movw_movt_code(1, TH_CHILD_OFFSET)
    emit(arm_mov_imm(2, 160))
    emit(arm_movw(3, 0x4000))
    emit(0xE24DD010)
    emit(arm_mov_imm(12, 0))
    emit(0xE58DC000)
    emit(0xE58DC004)
    emit(0xE58DC008)
    call(1)
    emit(0xE28DD010)
    store_rw(0, TH_RW_THREAD_ID)
    check_nonzero(2)

    load_rw(0, TH_RW_THREAD_ID)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(2)
    check(4)

    load_rw(0, TH_RW_THREAD_ID)
    movw_movt_rw(1, TH_RW_STATUS)
    emit(arm_mov_imm(2, 0))
    call(3)
    check(8)
    check_word(TH_RW_STATUS, 0x33, 16)

    # handler saw both START (0x04) and END (0x08)
    check_word(TH_RW_EVENT_DATA + 8, 0x0C, 32)
    movw_movt_rw(1, TH_RW_EVENT_DATA + 4)
    emit(0xE5911000)
    load_rw(2, TH_RW_THREAD_ID)
    emit(0xE1510002)
    emit(0x13844000 | arm_encode_imm(64))

    load_rw(0, TH_RW_HANDLER_ID)
    call(4)
    check(128)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= TH_CHILD_OFFSET, f"thread handler code overlaps child ({len(code) * 4:#x})"

    rx = bytearray(TH_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    # Child entry: return 0x33.
    rx[TH_CHILD_OFFSET:TH_CHILD_OFFSET + 8] = b"".join(struct.pack("<I", w) for w in (
        arm_mov_imm(0, 0x33), 0xE12FFF1E))

    # Handler: OR event type into the flag word and record the thread id.
    handler = [
        0xE593C008,  # ldr r12, [r3, #8]
        0xE18CC000,  # orr r12, r12, r0
        0xE583C008,  # str r12, [r3, #8]
        0xE5831004,  # str r1, [r3, #4]
        0xE3A00000,  # mov r0, #0
        0xE12FFF1E,  # bx lr
    ]
    rx[TH_HANDLER_OFFSET:TH_HANDLER_OFFSET + len(handler) * 4] = b"".join(
        struct.pack("<I", w) for w in handler)

    rx[TH_LIB1_NAME:TH_LIB1_NAME + len(TH_LIB1)] = TH_LIB1
    rx[TH_LIB2_NAME:TH_LIB2_NAME + len(TH_LIB2)] = TH_LIB2

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, TH_IMPORT1)
    struct.pack_into("<I", info, 0x30, TH_IMPORT2 + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[TH_MODINFO:TH_MODINFO + MODULE_INFO_SIZE] = info

    imp1 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp1, 0, IMPORT_SIZE, 1, 0, len(TH_KERNEL_FUNCS), 0, 0)
    struct.pack_into("<I", imp1, IMP_LIBRARY_NID, TH_LIB1_NID)
    rx[TH_IMPORT1:TH_IMPORT1 + IMPORT_SIZE] = imp1

    imp2 = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp2, 0, IMPORT_SIZE, 1, 0, len(TH_THREADMGR_FUNCS), 0, 0)
    struct.pack_into("<I", imp2, IMP_LIBRARY_NID, TH_LIB2_NID)
    rx[TH_IMPORT2:TH_IMPORT2 + IMPORT_SIZE] = imp2

    for i, nid in enumerate(TH_KERNEL_FUNCS):
        struct.pack_into("<I", rx, TH_NIDS1 + i * 4, nid)
    for i, nid in enumerate(TH_THREADMGR_FUNCS):
        struct.pack_into("<I", rx, TH_NIDS2 + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, TH_IMPORT1 + IMP_LIBRARY_NAME, TH_LIB1_NAME))
    rx_relocs.append(encode_rela(0, 0, TH_IMPORT1 + IMP_FUNC_NID_TABLE, TH_NIDS1))
    rx_relocs.append(encode_rela(0, 0, TH_IMPORT1 + IMP_FUNC_ENTRY_TABLE, TH_ENTRIES1))
    rx_relocs.append(encode_rela(0, 0, TH_IMPORT2 + IMP_LIBRARY_NAME, TH_LIB2_NAME))
    rx_relocs.append(encode_rela(0, 0, TH_IMPORT2 + IMP_FUNC_NID_TABLE, TH_NIDS2))
    rx_relocs.append(encode_rela(0, 0, TH_IMPORT2 + IMP_FUNC_ENTRY_TABLE, TH_ENTRIES2))
    for i in range(len(TH_KERNEL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TH_ENTRIES1 + i * 4, TH_STUB_BASE + i * 12))
    for i in range(len(TH_THREADMGR_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, TH_ENTRIES2 + i * 4,
                                     TH_STUB_BASE + (len(TH_KERNEL_FUNCS) + i) * 12))

    rw = bytearray(TH_RW_SIZE)
    rw[TH_RW_HANDLER_NAME:TH_RW_HANDLER_NAME + len(TH_HANDLER_NAME)] = TH_HANDLER_NAME
    rw[TH_RW_THREAD_NAME:TH_RW_THREAD_NAME + len(TH_THREAD_NAME)] = TH_THREAD_NAME
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, TH_MODINFO)


# --- SceMotion fixture -----------------------------------------------------

MO_STUB_BASE = 0x200
MO_LIB_NAME = 0x270
MO_MODINFO = 0x280
MO_IMPORT = 0x2E0
MO_NIDS = 0x310
MO_ENTRIES = 0x330
MO_RX_SIZE = 0x360

MO_LIB = b"SceMotion\0"
MO_LIB_NID = 0xDC571B3F
MO_FUNCS = [
    0x28034AC9,  # sceMotionStartSampling
    0xBDB32767,  # sceMotionGetState
    0x4F28BFE0,  # sceMotionGetBasicOrientation
    0xC1652201,  # sceMotionGetTiltCorrection
    0xDACB2A41,  # sceMotionSetAngleThreshold
    0x499B6C87,  # sceMotionGetAngleThreshold
    0x047F060A,  # sceMotionGetDeviceLocation
    0x47D679EA,  # sceMotionGetSensorState
]

MO_RW_STATE = 0x000
MO_RW_SENSOR = 0x100
MO_RW_VECTOR = 0x140
MO_RW_SIZE = 0x160


def build_motion_elf():
    """Guest ELF for the stationary SceMotion model."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, MO_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)
        emit(0x13844000 | arm_encode_imm(bit))

    def check_negative(bit):
        emit(0xE3500000)
        emit(0xA3844000 | arm_encode_imm(bit))

    def check_word(offset, value, bit):
        movw_movt_rw(1, offset)
        emit(0xE5910000)
        emit(arm_movw(2, value & 0xFFFF))
        emit(arm_movt(2, (value >> 16) & 0xFFFF))
        emit(0xE1500002)
        emit(0x13844000 | arm_encode_imm(bit))

    emit(0xE92D4010)
    emit(arm_mov_imm(4, 0))

    call(0)
    check(1)

    movw_movt_rw(0, MO_RW_STATE)
    call(1)
    check(2)
    check_word(MO_RW_STATE + 196, 0x3F800000, 4)  # basicOrientation.z

    movw_movt_rw(0, MO_RW_VECTOR)
    call(2)
    check(8)
    check_word(MO_RW_VECTOR + 8, 0x3F800000, 16)

    call(3)
    emit(0xE3500001)
    emit(0x13844000 | arm_encode_imm(32))

    emit(arm_movw(0, 0x0000))
    emit(arm_movt(0, 0x4248))  # 50.0f
    call(4)
    check_negative(64)

    call(5)
    emit(arm_movw(1, 0x0000))
    emit(arm_movt(1, 0x41F0))  # 30.0f = 0x41F00000
    emit(0xE1500001)
    emit(0x13844000 | arm_encode_imm(128))

    movw_movt_rw(0, MO_RW_VECTOR)
    call(6)
    check(256)
    check_word(MO_RW_VECTOR, 0xC2000000, 512)  # accelerometer.x = -32.0f

    movw_movt_rw(0, MO_RW_SENSOR)
    emit(arm_mov_imm(1, 1))
    call(7)
    check(1024)
    check_word(MO_RW_SENSOR + 8, 0x3F800000, 2048)

    emit(0xE1A00004)
    emit(0xE3500000)
    emit(0x03A0002A)
    emit(0xE8BD8010)

    assert len(code) * 4 <= MO_STUB_BASE, f"motion fixture code overlaps stubs ({len(code) * 4:#x})"

    rx = bytearray(MO_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[MO_LIB_NAME:MO_LIB_NAME + len(MO_LIB)] = MO_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, MO_IMPORT)
    struct.pack_into("<I", info, 0x30, MO_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[MO_MODINFO:MO_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(MO_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, MO_LIB_NID)
    rx[MO_IMPORT:MO_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(MO_FUNCS):
        struct.pack_into("<I", rx, MO_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, MO_IMPORT + IMP_LIBRARY_NAME, MO_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, MO_IMPORT + IMP_FUNC_NID_TABLE, MO_NIDS))
    rx_relocs.append(encode_rela(0, 0, MO_IMPORT + IMP_FUNC_ENTRY_TABLE, MO_ENTRIES))
    for i in range(len(MO_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, MO_ENTRIES + i * 4, MO_STUB_BASE + i * 12))

    rw = bytearray(MO_RW_SIZE)
    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, MO_MODINFO)


# --- SceAppUtil fixture ----------------------------------------------------

APPUTIL_STUB_BASE = 0x140
APPUTIL_LIB_NAME = 0x190
APPUTIL_MODINFO = 0x1A0
APPUTIL_IMPORT = 0x200
APPUTIL_NIDS = 0x228
APPUTIL_ENTRIES = 0x240
APPUTIL_RX_SIZE = 0x258

APPUTIL_LIB = b"SceAppUtil\0"

APPUTIL_FUNCS = [
    0xDAFFE671,  # sceAppUtilInit
    0x7E8FE96A,  # sceAppUtilSaveDataSlotCreate
    0x93F0D89F,  # sceAppUtilSaveDataSlotGetParam
    0x266A7646,  # sceAppUtilSaveDataSlotDelete
    0xB220B00B,  # sceAppUtilShutdown
]

APPUTIL_RW_INIT = 0x000
APPUTIL_RW_BOOT = 0x040
APPUTIL_RW_SLOT = 0x070
APPUTIL_RW_OUT = 0x3C0
APPUTIL_RW_SIZE = 0x710


def build_apputil_elf():
    """Guest ELF that drives the save-data slot lifecycle through imports."""
    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def movw_movt_rw(rd, offset):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, 1, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, 1, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, APPUTIL_STUB_BASE + index * 12))

    def check(bit):
        emit(0xE3500000)             # cmp r0, #0
        emit(0x13844000 | bit)       # orrne r4, r4, #bit

    emit(0xE92D4010)                 # push {r4, lr}
    emit(arm_mov_imm(4, 0))          # mov r4, #0

    # sceAppUtilInit(init, boot)
    movw_movt_rw(0, APPUTIL_RW_INIT)
    movw_movt_rw(1, APPUTIL_RW_BOOT)
    call(0)
    check(1)

    # sceAppUtilSaveDataSlotCreate(0, param, NULL)
    emit(arm_mov_imm(0, 0))
    movw_movt_rw(1, APPUTIL_RW_SLOT)
    emit(arm_mov_imm(2, 0))
    call(1)
    check(2)

    # sceAppUtilSaveDataSlotGetParam(0, out, NULL)
    emit(arm_mov_imm(0, 0))
    movw_movt_rw(1, APPUTIL_RW_OUT)
    emit(arm_mov_imm(2, 0))
    call(2)
    check(4)

    # sceAppUtilSaveDataSlotDelete(0, NULL)
    emit(arm_mov_imm(0, 0))
    emit(arm_mov_imm(1, 0))
    call(3)
    check(8)

    # sceAppUtilShutdown()
    call(4)
    check(16)

    emit(0xE1A00004)                 # mov r0, r4
    emit(0xE3500000)                 # cmp r0, #0
    emit(0x03A0002A)                 # moveq r0, #42
    emit(0xE8BD8010)                 # pop {r4, pc}

    assert len(code) * 4 <= APPUTIL_STUB_BASE, "apputil fixture code overlaps the stubs"

    rx = bytearray(APPUTIL_RX_SIZE)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)

    rx[APPUTIL_LIB_NAME:APPUTIL_LIB_NAME + len(APPUTIL_LIB)] = APPUTIL_LIB

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, APPUTIL_IMPORT)
    struct.pack_into("<I", info, 0x30, APPUTIL_IMPORT + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0x00000000)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[APPUTIL_MODINFO:APPUTIL_MODINFO + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(APPUTIL_FUNCS), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, SCEAPPUTIL_NID)
    rx[APPUTIL_IMPORT:APPUTIL_IMPORT + IMPORT_SIZE] = imp

    for i, nid in enumerate(APPUTIL_FUNCS):
        struct.pack_into("<I", rx, APPUTIL_NIDS + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, APPUTIL_IMPORT + IMP_LIBRARY_NAME, APPUTIL_LIB_NAME))
    rx_relocs.append(encode_rela(0, 0, APPUTIL_IMPORT + IMP_FUNC_NID_TABLE, APPUTIL_NIDS))
    rx_relocs.append(encode_rela(0, 0, APPUTIL_IMPORT + IMP_FUNC_ENTRY_TABLE, APPUTIL_ENTRIES))
    for i in range(len(APPUTIL_FUNCS)):
        rx_relocs.append(encode_rela(0, 0, APPUTIL_ENTRIES + i * 4,
                                     APPUTIL_STUB_BASE + i * 12))

    rw = bytearray(APPUTIL_RW_SIZE)
    struct.pack_into("<i", rw, APPUTIL_RW_SLOT + 8, 0x1234)  # userParam

    rela = bytearray(b"".join(rx_relocs))
    return assemble_elf(rx, rw, rela, APPUTIL_MODINFO)


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


def build_gxm_draw_elf(shader_dir):
    """Guest ELF that performs a full imported-SceGxm draw of a textured quad.

    Everything goes through the import stubs, so this covers the guest ARM
    calling convention (including stack arguments) and the whole shader
    patcher / stream / uniform / draw sequence. The framebuffer is checked in
    guest code; under a null graphics backend the calls are no-ops but still
    have to return success.
    """
    with open(os.path.join(shader_dir, "texture_v.gxp"), "rb") as f:
        vertex_gxp = f.read()
    with open(os.path.join(shader_dir, "texture_f.gxp"), "rb") as f:
        fragment_gxp = f.read()

    funcs = [
        0xB0F1E4EC,  # sceGxmInitialize
        0xE84CE5B4,  # sceGxmCreateContext
        0x05032658,  # sceGxmShaderPatcherCreate
        0x2B528462,  # sceGxmShaderPatcherRegisterProgram
        0xB7BBA6D5,  # sceGxmShaderPatcherCreateVertexProgram
        0x4ED2E49D,  # sceGxmShaderPatcherCreateFragmentProgram
        0x277794C4,  # sceGxmProgramFindParameterByName
        0x207AF96B,  # sceGxmCreateRenderTarget
        0xED0F6E25,  # sceGxmColorSurfaceInit
        0xA41DB0D6,  # sceGxmDepthStencilSurfaceInitDisabled
        0x4811AECB,  # sceGxmTextureInitLinear
        0x8734FF4E,  # sceGxmBeginScene
        0x60CF708E,  # sceGxmSetDefaultRegionClipAndViewport
        0x31FF8ABD,  # sceGxmSetVertexProgram
        0xAD2F48D9,  # sceGxmSetFragmentProgram
        0x29C34DF5,  # sceGxmSetFragmentTexture
        0x895DF2E9,  # sceGxmSetVertexStream
        0x97118913,  # sceGxmReserveVertexDefaultUniformBuffer
        0x65DD0C84,  # sceGxmSetUniformDataF
        0xBC059AFC,  # sceGxmDraw
        0xFE300E2F,  # sceGxmEndScene
    ]

    stub_base = 0x1400
    lib_name = 0x1500
    str_wvp = 0x1510
    modinfo = 0x1520
    import_off = 0x1580
    nids = 0x15A8
    entries = 0x1600
    vgxp_off = 0x1680
    fgxp_off = (vgxp_off + len(vertex_gxp) + 0xF) & ~0xF
    rx_size = (fgxp_off + len(fragment_gxp) + 0xF) & ~0xF

    # Data segment layout.
    rw_init = 0x0000
    rw_ctx_params = 0x0020
    rw_host_mem = 0x0050
    host_mem_size = 0x10000
    rw_ctx_out = rw_host_mem + host_mem_size
    rw_patcher_params = rw_ctx_out + 0x10
    rw_patcher_out = rw_patcher_params + 0x80
    rw_vertex_id = rw_patcher_out + 4
    rw_fragment_id = rw_vertex_id + 4
    rw_vertex_program = rw_fragment_id + 4
    rw_fragment_program = rw_vertex_program + 4
    rw_wvp = rw_fragment_program + 4
    rw_uniform = rw_wvp + 4
    rw_attributes = rw_uniform + 8
    rw_streams = rw_attributes + 16
    rw_rt_params = rw_streams + 8
    rw_rt_out = rw_rt_params + 0x14
    rw_color_surface = rw_rt_out + 8
    rw_depth_surface = rw_color_surface + 0x30
    rw_texture = rw_depth_surface + 0x14
    rw_identity = (rw_texture + 0x10 + 0xF) & ~0xF
    rw_positions = rw_identity + 64
    rw_uvs = rw_positions + 64
    rw_indices = rw_uvs + 32
    rw_tex_pixels = (rw_indices + 12 + 0xF) & ~0xF
    rw_fb = (rw_tex_pixels + 256 + 0xF) & ~0xF
    rw_size = rw_fb + 64 * 64 * 4

    code = []
    rx_relocs = []

    def emit(word):
        code.append(word & 0xFFFFFFFF)

    def imm(rd, value):
        emit(arm_movw(rd, value & 0xFFFF))
        emit(arm_movt(rd, (value >> 16) & 0xFFFF))

    def addr(rd, offset, symseg=1):
        base = len(code) * 4
        emit(arm_movw(rd, 0))
        emit(arm_movt(rd, 0))
        rx_relocs.append(encode_rela(0, symseg, base, offset, R_ARM_MOVW_ABS_NC))
        rx_relocs.append(encode_rela(0, symseg, base + 4, offset, R_ARM_MOVT_ABS))

    def call(index):
        emit(arm_bl(len(code) * 4, stub_base + index * 12))

    def check(bit):
        emit(0xE3500000)                       # cmp r0, #0
        emit(0x13844000 | arm_encode_imm(bit))  # orrne r4, r4, #bit

    def load_out(rd, offset):
        addr(12, offset)
        emit(0xE59C0000 | (rd << 12))          # ldr rd, [r12]

    def str_sp(rd, offset):
        emit(0xE58D0000 | (rd << 12) | (offset & 0xFFF))

    emit(0xE92D4010)                           # push {r4, lr}
    emit(0xE3A04000)                           # mov r4, #0

    # sceGxmInitialize(&params)
    addr(0, rw_init)
    call(0)
    check(1)

    # sceGxmCreateContext(&params, &context)
    addr(0, rw_ctx_params)
    addr(1, rw_ctx_out)
    call(1)
    check(2)

    # sceGxmShaderPatcherCreate(&params, &patcher)
    addr(0, rw_patcher_params)
    addr(1, rw_patcher_out)
    call(2)
    check(4)

    # Register both GXP programs.
    load_out(0, rw_patcher_out)
    addr(1, vgxp_off, 0)
    addr(2, rw_vertex_id)
    call(3)
    check(8)
    load_out(0, rw_patcher_out)
    addr(1, fgxp_off, 0)
    addr(2, rw_fragment_id)
    call(3)
    check(16)

    # sceGxmProgramFindParameterByName(vertex_gxp, "wvp")
    addr(0, vgxp_off, 0)
    addr(1, str_wvp, 0)
    call(6)
    addr(12, rw_wvp)
    emit(0xE58C0000)                           # str r0, [r12]
    emit(0xE3500000)                           # cmp r0, #0
    emit(0x03844000 | arm_encode_imm(32))      # orreq r4, r4, #32 (NULL is failure)

    # sceGxmShaderPatcherCreateVertexProgram(patcher, id, attrs, 2, streams, 2, &out)
    load_out(0, rw_patcher_out)
    load_out(1, rw_vertex_id)
    addr(2, rw_attributes)
    emit(arm_mov_imm(3, 2))
    emit(0xE24DD010)                           # sub sp, sp, #16
    addr(12, rw_streams)
    str_sp(12, 0)
    emit(arm_mov_imm(12, 2))
    str_sp(12, 4)
    addr(12, rw_vertex_program)
    str_sp(12, 8)
    call(4)
    check(64)
    emit(0xE28DD010)                           # add sp, sp, #16

    # sceGxmShaderPatcherCreateFragmentProgram(patcher, id, UCHAR4, NONE, NULL, vs, &out)
    load_out(0, rw_patcher_out)
    load_out(1, rw_fragment_id)
    emit(arm_mov_imm(2, 1))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD010)
    emit(arm_mov_imm(12, 0))
    str_sp(12, 0)
    addr(12, vgxp_off, 0)
    str_sp(12, 4)
    addr(12, rw_fragment_program)
    str_sp(12, 8)
    call(5)
    check(128)
    emit(0xE28DD010)

    # sceGxmCreateRenderTarget(&params, &target)
    addr(0, rw_rt_params)
    addr(1, rw_rt_out)
    call(7)
    check(256)

    # sceGxmColorSurfaceInit(surface, A8B8G8R8, LINEAR, NONE, 32BIT, 64, 64, 64, fb)
    addr(0, rw_color_surface)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD018)                           # sub sp, sp, #24
    emit(arm_mov_imm(12, 0))
    str_sp(12, 0)
    emit(arm_mov_imm(12, 64))
    str_sp(12, 4)
    str_sp(12, 8)
    str_sp(12, 12)
    addr(12, rw_fb)
    str_sp(12, 16)
    call(8)
    check(512)
    emit(0xE28DD018)

    # sceGxmDepthStencilSurfaceInitDisabled(&surface)
    addr(0, rw_depth_surface)
    call(9)
    check(1024)

    # sceGxmTextureInitLinear(&texture, pixels, U8U8U8U8_ABGR, 2, 2, 0)
    addr(0, rw_texture)
    addr(1, rw_tex_pixels)
    imm(2, 0x0C000000)
    emit(arm_mov_imm(3, 2))
    emit(0xE24DD010)
    emit(arm_mov_imm(12, 2))
    str_sp(12, 0)
    emit(arm_mov_imm(12, 0))
    str_sp(12, 4)
    call(10)
    check(2048)
    emit(0xE28DD010)

    # sceGxmBeginScene(ctx, 0, target, NULL, NULL, NULL, &color, &depth)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 0))
    load_out(2, rw_rt_out)
    emit(arm_mov_imm(3, 0))
    emit(0xE24DD010)
    emit(arm_mov_imm(12, 0))
    str_sp(12, 0)
    str_sp(12, 4)
    addr(12, rw_color_surface)
    str_sp(12, 8)
    addr(12, rw_depth_surface)
    str_sp(12, 12)
    call(11)
    check(4096)
    emit(0xE28DD010)

    # sceGxmSetDefaultRegionClipAndViewport(ctx, 64, 64)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 64))
    emit(arm_mov_imm(2, 64))
    call(12)

    # Programs, texture, streams.
    load_out(0, rw_ctx_out)
    load_out(1, rw_vertex_program)
    call(13)
    load_out(0, rw_ctx_out)
    load_out(1, rw_fragment_program)
    call(14)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 0))
    addr(2, rw_texture)
    call(15)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 0))
    addr(2, rw_positions)
    call(16)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 1))
    addr(2, rw_uvs)
    call(16)

    # sceGxmReserveVertexDefaultUniformBuffer(ctx, &uniform)
    load_out(0, rw_ctx_out)
    addr(1, rw_uniform)
    call(17)
    check(8192)

    # sceGxmSetUniformDataF(uniform, wvp, 0, 16, identity)
    load_out(0, rw_uniform)
    load_out(1, rw_wvp)
    emit(arm_mov_imm(2, 0))
    emit(arm_mov_imm(3, 16))
    emit(0xE24DD010)
    addr(12, rw_identity)
    str_sp(12, 0)
    call(18)
    check(16384)
    emit(0xE28DD010)

    # sceGxmDraw(ctx, TRIANGLES, U16, indices, 6)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    addr(3, rw_indices)
    emit(0xE24DD010)
    emit(arm_mov_imm(12, 6))
    str_sp(12, 0)
    call(19)
    check(32768)
    emit(0xE28DD010)

    # sceGxmEndScene(ctx, NULL, NULL)
    load_out(0, rw_ctx_out)
    emit(arm_mov_imm(1, 0))
    emit(arm_mov_imm(2, 0))
    call(20)
    check(65536)

    # The centre pixel must be the texture colour (A8B8G8R8: R G B A).
    addr(12, rw_fb)
    imm(2, (32 * 64 + 32) * 4)
    emit(0xE08C0002)                           # add r0, r12, r2
    emit(0xE5900000)                           # ldr r0, [r0]
    imm(1, 0x281E140A)
    emit(0xE3500000)                           # cmp r0, #0
    emit(0x01A00001)                           # moveq r0, r1 (null backend: no pixels)
    emit(0xE1500001)                           # cmp r0, r1
    emit(0x13844000 | arm_encode_imm(1 << 18)) # orrne r4, r4, #0x40000

    emit(0xE3540000)                           # cmp r4, #0
    emit(0x03A0002A)                           # moveq r0, #42
    emit(0x11A00004)                           # movne r0, r4
    emit(0xE8BD8010)                           # pop {r4, pc}

    assert len(code) * 4 <= stub_base, "draw fixture code overlaps the stubs"

    rx = bytearray(rx_size)
    rx[:len(code) * 4] = b"".join(struct.pack("<I", w) for w in code)
    rx[lib_name:lib_name + len(LIB_NAME)] = LIB_NAME
    rx[str_wvp:str_wvp + 4] = b"wvp\0"

    info = bytearray(MODULE_INFO_SIZE)
    struct.pack_into("<HH", info, 0x00, 0x0000, 0x0101)
    info[0x04:0x04 + len(MODULE_NAME)] = MODULE_NAME
    struct.pack_into("<I", info, 0x2C, import_off)
    struct.pack_into("<I", info, 0x30, import_off + IMPORT_SIZE)
    struct.pack_into("<I", info, 0x34, 0x12345678)
    struct.pack_into("<I", info, 0x44, 0)
    struct.pack_into("<I", info, 0x48, 0xFFFFFFFF)
    struct.pack_into("<I", info, 0x5C, 0x03570011)
    rx[modinfo:modinfo + MODULE_INFO_SIZE] = info

    imp = bytearray(IMPORT_SIZE)
    struct.pack_into("<HHHHHH", imp, 0, IMPORT_SIZE, 1, 0, len(funcs), 0, 0)
    struct.pack_into("<I", imp, IMP_LIBRARY_NID, SCEGXM_NID)
    rx[import_off:import_off + IMPORT_SIZE] = imp
    for i, nid in enumerate(funcs):
        struct.pack_into("<I", rx, nids + i * 4, nid)

    rx_relocs.append(encode_rela(0, 0, import_off + IMP_LIBRARY_NAME, lib_name))
    rx_relocs.append(encode_rela(0, 0, import_off + IMP_FUNC_NID_TABLE, nids))
    rx_relocs.append(encode_rela(0, 0, import_off + IMP_FUNC_ENTRY_TABLE, entries))
    for i in range(len(funcs)):
        rx_relocs.append(encode_rela(0, 0, entries + i * 4, stub_base + i * 12))

    rx[vgxp_off:vgxp_off + len(vertex_gxp)] = vertex_gxp
    rx[fgxp_off:fgxp_off + len(fragment_gxp)] = fragment_gxp

    rw = bytearray(rw_size)
    struct.pack_into("<I", rw, rw_ctx_params + 4, host_mem_size)
    struct.pack_into("<HHHH", rw, rw_rt_params + 4, 64, 64, 1, 0)
    struct.pack_into("<HHBBH", rw, rw_attributes, 0, 0, 9, 4, 0)
    struct.pack_into("<HHBBH", rw, rw_attributes + 8, 1, 0, 9, 2, 4)
    struct.pack_into("<HH", rw, rw_streams, 16, 0)
    struct.pack_into("<HH", rw, rw_streams + 4, 8, 0)
    struct.pack_into("<6H", rw, rw_indices, 0, 1, 2, 0, 2, 3)
    for i in range(8 * 8):
        rw[rw_tex_pixels + i * 4:rw_tex_pixels + i * 4 + 4] = bytes((10, 20, 30, 40))
    struct.pack_into("<16f", rw, rw_identity,
                     1.0, 0.0, 0.0, 0.0,
                     0.0, 1.0, 0.0, 0.0,
                     0.0, 0.0, 1.0, 0.0,
                     0.0, 0.0, 0.0, 1.0)
    struct.pack_into("<16f", rw, rw_positions,
                     -1.0, -1.0, 0.0, 1.0,
                     1.0, -1.0, 0.0, 1.0,
                     1.0, 1.0, 0.0, 1.0,
                     -1.0, 1.0, 0.0, 1.0)
    struct.pack_into("<8f", rw, rw_uvs,
                     0.0, 1.0,
                     1.0, 1.0,
                     1.0, 0.0,
                     0.0, 0.0)

    rw_relocs = [encode_rela(1, 1, rw_ctx_params, rw_host_mem)]
    rela = bytearray(b"".join(rx_relocs) + b"".join(rw_relocs))
    return assemble_elf(rx, rw, rela, modinfo)


def arm_encode_imm(value):
    """Encode a 32-bit value as an ARM data-processing immediate (rot, imm8)."""
    for rot in range(16):
        shift = 2 * rot
        rotated = ((value << shift) | (value >> (32 - shift))) & 0xFFFFFFFF if shift else value
        if rotated <= 0xFF:
            return (rot << 8) | rotated
    raise ValueError(f"0x{value:x} is not an ARM immediate")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    out_dir = sys.argv[1]
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    shader_dir = (sys.argv[2] if len(sys.argv) > 2 else
                  os.path.join(repo_root, "external", "Vita3K", "tools", "native-tool",
                               "src", "shaders"))
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

    audio_path = os.path.join(out_dir, "guest_audio.elf")
    with open(audio_path, "wb") as f:
        f.write(build_audio_elf())
    print(f"wrote {audio_path}")

    apputil_path = os.path.join(out_dir, "guest_apputil.elf")
    with open(apputil_path, "wb") as f:
        f.write(build_apputil_elf())
    print(f"wrote {apputil_path}")

    ngs_path = os.path.join(out_dir, "guest_ngs.elf")
    with open(ngs_path, "wb") as f:
        f.write(build_ngs_elf())
    print(f"wrote {ngs_path}")

    io_async_path = os.path.join(out_dir, "guest_io_async.elf")
    with open(io_async_path, "wb") as f:
        f.write(build_io_async_elf())
    print(f"wrote {io_async_path}")

    runtime_path = os.path.join(out_dir, "guest_kernel_runtime.elf")
    with open(runtime_path, "wb") as f:
        f.write(build_kernel_runtime_elf())
    print(f"wrote {runtime_path}")

    locks_path = os.path.join(out_dir, "guest_kernel_locks.elf")
    with open(locks_path, "wb") as f:
        f.write(build_kernel_locks_elf())
    print(f"wrote {locks_path}")

    timer_path = os.path.join(out_dir, "guest_kernel_timer.elf")
    with open(timer_path, "wb") as f:
        f.write(build_kernel_timer_elf())
    print(f"wrote {timer_path}")

    thread_path = os.path.join(out_dir, "guest_thread_info.elf")
    with open(thread_path, "wb") as f:
        f.write(build_thread_info_elf())
    print(f"wrote {thread_path}")

    event_path = os.path.join(out_dir, "guest_kernel_event.elf")
    with open(event_path, "wb") as f:
        f.write(build_kernel_event_elf())

    thread_extra_path = os.path.join(out_dir, "guest_thread_extra.elf")
    with open(thread_extra_path, "wb") as f:
        f.write(build_thread_extra_elf())

    module_host_path = os.path.join(out_dir, "guest_module_host.elf")
    with open(module_host_path, "wb") as f:
        f.write(build_module_host_elf())

    module_path = os.path.join(out_dir, "mod.suprx")
    with open(module_path, "wb") as f:
        f.write(build_module_file())

    module_export_host_path = os.path.join(out_dir, "guest_module_export.elf")
    with open(module_export_host_path, "wb") as f:
        f.write(build_module_export_host_elf())

    with open(os.path.join(out_dir, "lib.suprx"), "wb") as f:
        f.write(build_export_library())
    with open(os.path.join(out_dir, "imp.suprx"), "wb") as f:
        f.write(build_import_module())

    sysmem_path = os.path.join(out_dir, "guest_sysmem.elf")
    with open(sysmem_path, "wb") as f:
        f.write(build_sysmem_elf())

    thread_event_path = os.path.join(out_dir, "guest_thread_event.elf")
    with open(thread_event_path, "wb") as f:
        f.write(build_thread_event_elf())

    motion_path = os.path.join(out_dir, "guest_motion.elf")
    with open(motion_path, "wb") as f:
        f.write(build_motion_elf())
    print(f"wrote {event_path}")

    if os.path.exists(os.path.join(shader_dir, "texture_v.gxp")) and os.path.exists(
            os.path.join(shader_dir, "texture_f.gxp")):
        draw_path = os.path.join(out_dir, "guest_gxm_draw.elf")
        with open(draw_path, "wb") as f:
            f.write(build_gxm_draw_elf(shader_dir))
        print(f"wrote {draw_path}")
    else:
        print(f"skipping guest_gxm_draw.elf: no GXP shaders in {shader_dir}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
