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

    emit(0xE92D4030)                 # push {r4, r5, lr}
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
    emit(0xE8BD8030)                 # pop {r4, r5, pc}

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
