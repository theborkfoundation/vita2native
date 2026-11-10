#include "config.h"

#include <limits.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "display.h"
#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "jit.h"
#include "load.h"
#include "log.h"
#include "module.h"
#include "netlog.h"
#include "vpk.h"

#include "modules/SceCtrl.h"
#include "modules/SceAudio.h"
#include "modules/SceAudiodec.h"
#include "modules/SceFios2.h"
#include "modules/SceAtrac.h"
#include "modules/SceDisplay.h"
#include "modules/SceGxm.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceModulemgr.h"
#include "modules/SceProcessmgr.h"
#include "modules/SceSqlite.h"
#include "modules/SceSysmem.h"
#include "modules/SceTouch.h"

#if defined(__arm__)
/*
 * Call-target tracing.
 *
 * Vita binaries - Unity in particular - reach their allocator through
 * `movw ip,#lo / movt ip,#hi / ... / blx ip`, so a call site has no
 * PC-relative branch to retarget, and the callee can be more than 16 MiB away
 * from any scratch space after the image (the 32-bit Thumb `bl` range).
 *
 * The pairs hold absolute addresses, so every call site can be repointed at
 * one shared stub placed after the image. The stub records the arguments,
 * calls the original function with a return trampoline as its link register,
 * and the trampoline records the result before resuming the guest.
 *
 * VITA2NATIVE_TRACE_TARGET=<vita address>[,<address>...]
 */
#define TRACE_RING_SIZE 32
#define TRACE_STACK_SIZE 16

typedef struct {
    uint32_t tag;
    uint32_t r0, r1, r2, r3;
    uint32_t a5, a6;
    uint32_t lr;
    uint32_t result;
} TraceCall;

static TraceCall g_trace_ring[TRACE_RING_SIZE];
static unsigned g_trace_count;

static __thread TraceCall t_trace_pending[TRACE_STACK_SIZE];
static __thread unsigned t_trace_depth;

static void __attribute__((noinline)) trace_call_enter(const uint32_t *frame, uint32_t tag)
{
    TraceCall *call;

    if (t_trace_depth >= TRACE_STACK_SIZE)
        return;

    call = &t_trace_pending[t_trace_depth++];
    call->tag = tag;
    call->r0 = frame[0];
    call->r1 = frame[1];
    call->r2 = frame[2];
    call->r3 = frame[3];
    call->a5 = frame[6];
    call->a6 = frame[7];
    call->lr = frame[5];
    call->result = 0;
}

/* Returns the address the traced function has to return to. */
static uint32_t __attribute__((noinline)) trace_call_leave(const uint32_t *frame)
{
    TraceCall *action;
    uint32_t result = frame[0];

    if (t_trace_depth == 0)
        return 0;

    action = &t_trace_pending[--t_trace_depth];
    action->result = result;
    g_trace_ring[g_trace_count++ % TRACE_RING_SIZE] = *action;

    return action->lr;
}

static void trace_dump_ring(void)
{
    char line[224];
    unsigned count = g_trace_count < TRACE_RING_SIZE ? g_trace_count : TRACE_RING_SIZE;
    unsigned start = g_trace_count - count;

    for (unsigned i = 0; i < count; i++) {
        const TraceCall *call = &g_trace_ring[(start + i) % TRACE_RING_SIZE];
        int length = snprintf(line, sizeof(line),
                              "vita2native: trace image+0x%06" PRIx32
                              ": r0=%08" PRIx32 " r1=%08" PRIx32 " r2=%08" PRIx32
                              " r3=%08" PRIx32 " a5=%08" PRIx32 " a6=%08" PRIx32
                              " lr=%08" PRIx32 " -> %08" PRIx32 "\n",
                              call->tag, call->r0, call->r1, call->r2, call->r3, call->a5,
                              call->a6, call->lr, call->result);

        if (length > 0)
            write(STDERR_FILENO, line, (size_t)length);
    }
}

static uint16_t trace_read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void write_thumb_movw(uint8_t *p, unsigned reg, uint16_t imm)
{
    uint16_t upper = (uint16_t)(0xF240 | (((imm >> 11) & 1) << 10) | ((imm >> 12) & 0xF));
    uint16_t lower =
        (uint16_t)((((imm >> 8) & 7) << 12) | ((reg & 0xF) << 8) | (imm & 0xFF));

    memcpy(p, &upper, 2);
    memcpy(p + 2, &lower, 2);
}

static void write_thumb_movt(uint8_t *p, unsigned reg, uint16_t imm)
{
    uint16_t upper = (uint16_t)(0xF2C0 | (((imm >> 11) & 1) << 10) | ((imm >> 12) & 0xF));
    uint16_t lower =
        (uint16_t)((((imm >> 8) & 7) << 12) | ((reg & 0xF) << 8) | (imm & 0xFF));

    memcpy(p, &upper, 2);
    memcpy(p + 2, &lower, 2);
}

static uint32_t thumb_mov_imm(uint16_t upper, uint16_t lower)
{
    return ((uint32_t)(upper & 0xF) << 12) | (((upper >> 10) & 1) << 11) |
           (((lower >> 12) & 7) << 8) | (lower & 0xFF);
}

/* Extent of the executable segments inside the JIT mapping. */
static size_t image_code_end(const LoadedModuleInfo *info, uintptr_t rx)
{
    size_t end = 0;

    for (int i = 0; i < info->num_segments; i++) {
        const LoadedSegment *segment = &info->segments[i];
        size_t segment_end;

        if (!(segment->perms & 1))
            continue;

        segment_end = ((uintptr_t)segment->vaddr - rx) + segment->memsz;
        if (segment_end > end)
            end = segment_end;
    }

    return end;
}

static const LoadedSegment *find_code_segment(const LoadedModuleInfo *info, uint32_t address,
                                              uint32_t *offset)
{
    for (int i = 0; i < info->num_segments; i++) {
        const LoadedSegment *segment = &info->segments[i];

        if (!(segment->perms & 1))
            continue;

        if (address >= segment->vita_vaddr && address - segment->vita_vaddr < segment->memsz) {
            *offset = address - segment->vita_vaddr;
            return segment;
        }
    }

    return NULL;
}

/*
 * The probe frame is {r0-r3, r12, lr} followed by the caller's stack
 * arguments, so frame[6] is the fifth argument.
 *
 * Layout: [0] entry stub (48 bytes), [48] return trampoline (28 bytes).
 */
#define TRACE_STUB_SIZE 80

static void write_call_probe_stub(uint8_t *stub, uintptr_t stub_rx, uintptr_t target,
                                  uint32_t tag)
{
    /* The host build may be ARM or Thumb; let the function pointer's bit pick. */
    const uintptr_t enter = (uintptr_t)&trace_call_enter;
    const uintptr_t leave = (uintptr_t)&trace_call_leave;
    const uint32_t trampoline_addr = (uint32_t)(stub_rx + 48) | 1;
    const uint16_t push_frame[] = {0xE92D, 0x500F}; /* push.w {r0-r3, r12, lr} */
    const uint16_t pop_frame[] = {0xE8BD, 0x500F};  /* pop.w {r0-r3, r12, lr} */
    const uint16_t mov_r0_sp = 0x4668;              /* mov r0, sp */
    const uint16_t blx_r12 = 0x47E0;                /* blx r12 */
    const uint16_t bx_r12 = 0x4760;                 /* bx r12 */
    const uint16_t bx_lr = 0x4770;                  /* bx lr */
    const uint16_t str_r0_sp = 0x9005;              /* str r0, [sp, #20] */
    const uint16_t nop = 0xBF00;

    memcpy(stub, push_frame, sizeof(push_frame));
    memcpy(stub + 4, &mov_r0_sp, 2);
    write_thumb_movw(stub + 6, 1, (uint16_t)tag);
    write_thumb_movt(stub + 10, 1, (uint16_t)(tag >> 16));
    write_thumb_movw(stub + 14, 12, (uint16_t)enter);
    write_thumb_movt(stub + 18, 12, (uint16_t)(enter >> 16));
    memcpy(stub + 22, &blx_r12, 2);
    memcpy(stub + 24, pop_frame, sizeof(pop_frame));
    write_thumb_movw(stub + 28, 12, (uint16_t)target);
    write_thumb_movt(stub + 32, 12, (uint16_t)(target >> 16));
    write_thumb_movw(stub + 36, 14, (uint16_t)trampoline_addr);
    write_thumb_movt(stub + 40, 14, (uint16_t)(trampoline_addr >> 16));
    memcpy(stub + 44, &bx_r12, 2);

    memcpy(stub + 48, push_frame, sizeof(push_frame));
    memcpy(stub + 52, &mov_r0_sp, 2);
    write_thumb_movw(stub + 54, 12, (uint16_t)leave);
    write_thumb_movt(stub + 58, 12, (uint16_t)(leave >> 16));
    memcpy(stub + 62, &blx_r12, 2);
    memcpy(stub + 64, &str_r0_sp, 2);
    memcpy(stub + 66, pop_frame, sizeof(pop_frame));
    memcpy(stub + 70, &bx_lr, 2);
    memcpy(stub + 72, &nop, 2);
    memcpy(stub + 74, &nop, 2);
}

/* Repoints every `movw/movt ip` pair that builds `address` at `stub`. */
static unsigned trace_repoint(const LoadedModuleInfo *info, uintptr_t stub, uint32_t address,
                              uint32_t tag)
{
    uintptr_t rx = (uintptr_t)info->code_rx;
    uintptr_t rw = (uintptr_t)info->code_rw;
    unsigned patched = 0;

    for (int i = 0; i < info->num_segments; i++) {
        const LoadedSegment *segment = &info->segments[i];
        uint8_t *base;
        size_t off;

        if (!(segment->perms & 1))
            continue;

        base = (uint8_t *)rw + ((uintptr_t)segment->vaddr - rx);

        for (off = 0; off + 8 <= segment->memsz; off += 2) {
            uint16_t upper = trace_read_u16(base + off);
            uint16_t lower;
            size_t next;
            bool found = false;
            uint32_t value;

            if ((upper & 0xFBF0) != 0xF240)
                continue;

            lower = trace_read_u16(base + off + 2);
            if (lower & 0x8000)
                continue; /* not a MOVW */

            /* The matching MOVT can be separated by a few instructions. */
            for (next = off + 4; next + 4 <= segment->memsz && next <= off + 24; next += 2) {
                uint16_t next_upper = trace_read_u16(base + next);
                uint16_t next_lower = trace_read_u16(base + next + 2);

                if ((next_upper & 0xFBF0) != 0xF2C0)
                    continue;
                if (((next_lower >> 8) & 0xF) != ((lower >> 8) & 0xF))
                    continue;

                found = true;
                break;
            }

            if (!found)
                continue;

            value = thumb_mov_imm(upper, lower) |
                    (thumb_mov_imm(trace_read_u16(base + next),
                                   trace_read_u16(base + next + 2)) << 16);

            /* The instructions have already been relocated to host addresses. */
            if (value != address && value != (address | 1))
                continue;

            write_thumb_movw(base + off, 12, (uint16_t)stub);
            write_thumb_movt(base + next, 12, (uint16_t)(stub >> 16));
            patched++;
        }
    }

    if (patched)
        LOG("Trace: repointed %u call(s) to image+0x%06" PRIx32, patched, tag);

    return patched;
}

static void install_target_trace(Jit *jit, const LoadedModuleInfo *info, uint32_t target,
                                 size_t *stub_cursor)
{
    uintptr_t rx = (uintptr_t)info->code_rx;
    uintptr_t rw = (uintptr_t)info->code_rw;
    size_t stub_offset = (*stub_cursor + 3) & ~(size_t)3;
    const LoadedSegment *segment;
    uint32_t offset = 0;
    uint32_t host_target;

    segment = find_code_segment(info, target, &offset);
    if (!segment) {
        LOG("Trace: 0x%08" PRIx32 " is not inside an executable segment", target);
        return;
    }

    if (!rx || !rw || stub_offset + TRACE_STUB_SIZE > jit->size) {
        LOG("Trace: no room for a stub (offset 0x%zx, jit 0x%zx)", stub_offset, jit->size);
        return;
    }

    host_target = (uint32_t)((uintptr_t)segment->vaddr + offset);
    write_call_probe_stub((uint8_t *)rw + stub_offset, rx + stub_offset, host_target, offset);

    if (trace_repoint(info, (uintptr_t)rx + stub_offset + 1, host_target, offset))
        *stub_cursor = stub_offset + TRACE_STUB_SIZE;
}
#endif

/* Base of the main executable's image, for crash-time diagnostics. */
static uintptr_t g_main_image_rw;
static size_t g_main_image_size;

#if defined(__arm__)
#include <signal.h>
#include <execinfo.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef HWCAP_NEON
#define HWCAP_NEON (1UL << 12)
#endif

static void check_host_cpu(void)
{
    if (!(getauxval(AT_HWCAP) & HWCAP_NEON))
        fatal_error("This CPU does not support NEON.",
                    "A 32-bit ARMv7-A CPU with NEON is required to run Vita code.");
}

/* Prints the mappings around the faulting address; the guest runs inside a
   host process, so this is the only reliable way to tell which module or
   allocation the fault belongs to. */
static void dump_maps_around(unsigned long address)
{
    static const char header[] = "vita2native: mappings around the fault:\n";
    char line[512];
    int printed = 0;
    FILE *maps = fopen("/proc/self/maps", "r");

    (void)address;

    if (!maps)
        return;

    write(STDERR_FILENO, header, sizeof(header) - 1);
    while (fgets(line, sizeof(line), maps)) {
        if (printed++ > 400)
            break;
        write(STDERR_FILENO, line, strlen(line));
    }

    fclose(maps);
}

static bool read_guest_u32(unsigned long address, uint32_t *out)
{
    struct iovec local = { out, sizeof(*out) };
    struct iovec remote = { (void *)address, sizeof(*out) };

    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)sizeof(*out);
}

static void dump_guest_words(unsigned long address, int count)
{
    uint32_t words[64];
    char message[1024];
    struct iovec local;
    struct iovec remote;
    int length;

    if (count > (int)ARRAY_SIZE(words))
        count = (int)ARRAY_SIZE(words);

    local.iov_base = words;
    local.iov_len = sizeof(uint32_t) * (size_t)count;
    remote.iov_base = (void *)address;
    remote.iov_len = local.iov_len;

    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)local.iov_len)
        return;

    length = snprintf(message, sizeof(message), "vita2native: [0x%08lx]", address);
    for (int i = 0; i < count && length > 0 && length < (int)sizeof(message) - 16; i++)
        length += snprintf(message + length, sizeof(message) - (size_t)length, " %08" PRIx32,
                           words[i]);
    if (length > 0) {
        message[length++] = '\n';
        write(STDERR_FILENO, message, (size_t)length);
    }
}

/* Anonymous read/write mappings of at least 1 MiB, with their first words.
   Used to tell host-allocated arenas from guest ones when tracking down heap
   problems. */
static void dump_big_anon_mappings(const char *tag)
{
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");

    if (!maps)
        return;

    while (fgets(line, sizeof(line), maps)) {
        unsigned long lo, hi;
        char message[160];
        int length;

        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
            continue;
        if (hi - lo < 0x100000)
            continue;
        if (!strstr(line, "rw-p 00000000 00:00 0"))
            continue;

        length = snprintf(message, sizeof(message),
                          "vita2native: %s anon %08lx-%08lx (0x%lx)\n", tag, lo, hi, hi - lo);
        if (length > 0)
            write(STDERR_FILENO, message, (size_t)length);
        dump_guest_words(lo, 8);
    }

    fclose(maps);
}

static void dump_guest_bytes(const char *path, unsigned long address, size_t length)
{
    struct iovec local;
    struct iovec remote;
    uint8_t *buffer;
    FILE *file;

    buffer = malloc(length);
    if (!buffer)
        return;

    local.iov_base = buffer;
    local.iov_len = length;
    remote.iov_base = (void *)address;
    remote.iov_len = length;

    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)length) {
        free(buffer);
        return;
    }

    file = fopen(path, "wb");
    if (file) {
        fwrite(buffer, 1, length, file);
        fclose(file);
    }

    free(buffer);
}

static unsigned long mapping_start(unsigned long address)
{
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned long start = 0;

    if (!maps)
        return 0;

    while (fgets(line, sizeof(line), maps)) {
        unsigned long lo, hi;

        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && address >= lo && address < hi) {
            start = lo;
            break;
        }
    }

    fclose(maps);
    return start;
}

static void guest_signal_handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    char message[320];
    int length;

    length = snprintf(message, sizeof(message),
                      "vita2native: guest signal %d at pc=0x%08lx lr=0x%08lx sp=0x%08lx "
                      "fault=0x%08lx\n",
                      signal, (unsigned long)uc->uc_mcontext.arm_pc,
                      (unsigned long)uc->uc_mcontext.arm_lr,
                      (unsigned long)uc->uc_mcontext.arm_sp,
                      info ? (unsigned long)info->si_addr : 0UL);
    if (length > 0)
        write(STDERR_FILENO, message, (size_t)length);

    length = snprintf(message, sizeof(message),
                      "vita2native: r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx r5=%08lx "
                      "r6=%08lx r7=%08lx r8=%08lx r9=%08lx r10=%08lx r11=%08lx r12=%08lx\n",
                      (unsigned long)uc->uc_mcontext.arm_r0, (unsigned long)uc->uc_mcontext.arm_r1,
                      (unsigned long)uc->uc_mcontext.arm_r2, (unsigned long)uc->uc_mcontext.arm_r3,
                      (unsigned long)uc->uc_mcontext.arm_r4, (unsigned long)uc->uc_mcontext.arm_r5,
                      (unsigned long)uc->uc_mcontext.arm_r6, (unsigned long)uc->uc_mcontext.arm_r7,
                      (unsigned long)uc->uc_mcontext.arm_r8, (unsigned long)uc->uc_mcontext.arm_r9,
                      (unsigned long)uc->uc_mcontext.arm_r10, (unsigned long)uc->uc_mcontext.arm_fp,
                      (unsigned long)uc->uc_mcontext.arm_ip);
    if (length > 0)
        write(STDERR_FILENO, message, (size_t)length);

    /* Optional guest-memory dump at crash: offsets are relative to the image. */
    trace_dump_ring();
    if (getenv("VITA2NATIVE_DUMP_MAPS"))
        dump_big_anon_mappings("crash");

    {
        const char *dump = getenv("VITA2NATIVE_CRASH_DUMP");

        if (dump && g_main_image_rw) {
            const char *cursor = dump;

            while (*cursor) {
                char *end = NULL;
                unsigned long offset = strtoul(cursor, &end, 0);

                if (offset + 64 <= g_main_image_size)
                    dump_guest_words(g_main_image_rw + offset, 16);

                cursor = (*end == ',') ? end + 1 : end;
                if (!*cursor)
                    break;
            }
        }
    }

    dump_maps_around(info ? (unsigned long)info->si_addr : 0UL);

    {
        const char *code_dump = getenv("VITA2NATIVE_CRASH_CODE_DUMP");

        if (code_dump && uc->uc_mcontext.arm_pc > 0x100) {
            unsigned long start = mapping_start((unsigned long)uc->uc_mcontext.arm_pc);

            if (start)
                dump_guest_bytes(code_dump, start, 0x400000);
        }
    }

    {
        const char *array_slot = getenv("VITA2NATIVE_CRASH_DUMP_ARRAY");

        if (array_slot) {
            uint32_t array = 0;
            struct iovec local = { &array, sizeof(array) };
            struct iovec remote = { (void *)((uintptr_t)uc->uc_mcontext.arm_sp +
                                             strtoul(array_slot, NULL, 0)),
                                    sizeof(array) };

            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
                (ssize_t)sizeof(array)) {
                char message[128];
                int length = snprintf(message, sizeof(message),
                                      "vita2native: array[sp+%s] = %08" PRIx32 "\n",
                                      array_slot, array);

                if (length > 0)
                    write(STDERR_FILENO, message, (size_t)length);
                dump_guest_words(array, 16);

                {
                    uint32_t klass = 0;
                    uint32_t count = 0;
                    char name[64];
                    struct iovec ln = { name, sizeof(name) - 1 };
                    struct iovec rn;

                    local.iov_base = &klass;
                    remote.iov_base = (void *)(uintptr_t)array;
                    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
                        (ssize_t)sizeof(klass)) {
                        uint32_t name_ptr = 0;

                        local.iov_base = &name_ptr;
                        remote.iov_base = (void *)((uintptr_t)klass + 8);
                        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
                            (ssize_t)sizeof(name_ptr)) {
                            rn.iov_base = (void *)(uintptr_t)name_ptr;
                            rn.iov_len = sizeof(name) - 1;
                            if (process_vm_readv(getpid(), &ln, 1, &rn, 1, 0) > 0) {
                                name[sizeof(name) - 1] = '\0';
                                length = snprintf(message, sizeof(message),
                                                  "vita2native: array class %s\n", name);
                                if (length > 0)
                                    write(STDERR_FILENO, message, (size_t)length);
                            }
                        }
                    }

                    local.iov_base = &count;
                    remote.iov_base = (void *)((uintptr_t)array + 12);
                    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
                        (ssize_t)sizeof(count))
                        count = 0;

                    if (count > 32)
                        count = 32;

                    for (uint32_t i = 0; i < count; i++) {
                        uint32_t element = 0;
                        uint32_t field8 = 0;
                        uint32_t field0 = 0;
                        struct iovec le = { &element, sizeof(element) };
                        struct iovec re = { (void *)((uintptr_t)array + 16 + i * 4),
                                            sizeof(element) };

                        if (process_vm_readv(getpid(), &le, 1, &re, 1, 0) !=
                            (ssize_t)sizeof(element))
                            break;

                        if (element >= 0x1000) {
                            local.iov_base = &field8;
                            remote.iov_base = (void *)((uintptr_t)element + 8);
                            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
                                (ssize_t)sizeof(field8))
                                field8 = 0;

                            local.iov_base = &field0;
                            remote.iov_base = (void *)(uintptr_t)element;
                            if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) !=
                                (ssize_t)sizeof(field0))
                                field0 = 0;
                        }

                        length = snprintf(message, sizeof(message),
                                          "vita2native:   [%u] = %08" PRIx32
                                          " [0]=%08" PRIx32 " [+8]=%08" PRIx32 "\n",
                                          i, element, field0, field8);
                        if (length > 0)
                            write(STDERR_FILENO, message, (size_t)length);
                    }
                }
            }
        }
    }

    {
        /*
         * VITA2NATIVE_CRASH_DUMP_VEC=<offset from sp> dumps a growable pointer
         * vector whose {begin} lives at sp+offset. The image used by the
         * IL2CPP reflection code keeps {begin, end, capacity} at +4/+8/+0xc,
         * so the begin offset is the one to pass.
         */
        const char *vector_slot = getenv("VITA2NATIVE_CRASH_DUMP_VEC");

        if (vector_slot) {
            unsigned long base = (unsigned long)uc->uc_mcontext.arm_sp +
                                 strtoul(vector_slot, NULL, 0);
            uint32_t begin = 0;
            uint32_t end = 0;

            if (read_guest_u32(base, &begin) && read_guest_u32(base + 4, &end)) {
                char message[192];
                uint32_t count = end > begin ? (end - begin) / 4 : 0;
                int length = snprintf(message, sizeof(message),
                                      "vita2native: vec[sp+%s] %08" PRIx32
                                      "..%08" PRIx32 " count=%" PRIu32 "\n",
                                      vector_slot, begin, end, count);

                if (length > 0)
                    write(STDERR_FILENO, message, (size_t)length);

                if (count > 48)
                    count = 48;

                for (uint32_t i = 0; i < count; i++) {
                    uint32_t element = 0;

                    if (!read_guest_u32((unsigned long)begin + i * 4, &element))
                        break;

                    length = snprintf(message, sizeof(message),
                                      "vita2native:   v[%" PRIu32 "] = %08" PRIx32 "\n", i,
                                      element);
                    if (length > 0)
                        write(STDERR_FILENO, message, (size_t)length);

                    if (element >= 0x1000)
                        dump_guest_words(element, 8);
                }
            }
        }
    }

    /* The first argument often points at the structure being processed. */
    dump_guest_words((unsigned long)uc->uc_mcontext.arm_r0, 32);
    if (uc->uc_mcontext.arm_r1)
        dump_guest_words((unsigned long)uc->uc_mcontext.arm_r1 - 8, 8);
    dump_guest_words((unsigned long)uc->uc_mcontext.arm_sp, 64);

    {
        void *frames[24];
        int count = backtrace(frames, (int)ARRAY_SIZE(frames));

        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }

    /* Optional: let the kernel write a core for post-mortem analysis.  Returning
       from the handler retries the faulting instruction with SIG_DFL, which
       produces the core without needing to unblock the signal here. */
    if (getenv("VITA2NATIVE_CRASH_CORE")) {
        struct sigaction dfl;

        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(signal, &dfl, NULL);
        return;
    }

    _exit(128 + signal);
}

static void debug_sample_handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    char message[256];
    int length;
    void *frames[24];
    int count;

    (void)signal;
    (void)info;

    length = snprintf(message, sizeof(message),
                      "vita2native: sample tid=%ld pc=0x%08lx lr=0x%08lx sp=0x%08lx "
                      "fp=0x%08lx\n",
                      (long)syscall(SYS_gettid), (unsigned long)uc->uc_mcontext.arm_pc,
                      (unsigned long)uc->uc_mcontext.arm_lr,
                      (unsigned long)uc->uc_mcontext.arm_sp,
                      (unsigned long)uc->uc_mcontext.arm_fp);
    if (length > 0)
        write(STDERR_FILENO, message, (size_t)length);

    count = backtrace(frames, (int)ARRAY_SIZE(frames));
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
}

static void install_guest_signal_handlers(void)
{
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT };
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guest_signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    for (size_t i = 0; i < ARRAY_SIZE(signals); i++)
        sigaction(signals[i], &action, NULL);

    /* `kill -USR1 <tid>` samples one thread's PC and call stack. */
    if (getenv("VITA2NATIVE_DEBUG_SAMPLE")) {
        struct sigaction sample;

        memset(&sample, 0, sizeof(sample));
        sample.sa_sigaction = debug_sample_handler;
        sample.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sample.sa_mask);
        sigaction(SIGUSR1, &sample, NULL);
    }
}
#endif

static bool g_headless;
static HostGpuDevice g_device;

static int host_init(void)
{
    HostGpuConfig gpu_config = { 960, 544, g_headless };
    int ret;

    g_device = host_gpu_create(&gpu_config);
    if (!g_device)
        return -1;
    LOG("GPU: %s", host_gpu_description(g_device));

    /* Init modules */
    ret = SceSysmem_init(g_device);
    if (ret != 0)
        return ret;
    ret = SceLibKernel_init();
    if (ret != 0)
        return ret;
    ret = SceFios2_init();
    if (ret != 0)
        return ret;
    ret = SceSqlite_init();
    if (ret != 0)
        return ret;
    ret = SceKernelThreadMgr_init();
    if (ret != 0)
        return ret;
    ret = SceKernelSync_init();
    if (ret != 0)
        return ret;
    ret = SceProcessmgr_init();
    if (ret != 0)
        return ret;
    ret = SceDisplay_init(g_device);
    if (ret != 0)
        return ret;
    ret = SceGxm_init(g_device);
    if (ret != 0)
        return ret;
    ret = SceCtrl_init();
    if (ret != 0)
        return ret;
    ret = SceTouch_init();
    if (ret != 0)
        return ret;
    ret = SceAudio_init();
    if (ret != 0)
        return ret;
    ret = SceAudiodec_init();
    if (ret != 0)
        return ret;
    ret = SceAtrac_init();
    if (ret != 0)
        return ret;

    return 0;
}

static void host_finish(void)
{
    SceGxm_finish();
    SceDisplay_finish();
    SceAtrac_finish();
    SceAudiodec_finish();
    SceAudio_finish();
    SceKernelSync_finish();
    SceKernelThreadMgr_finish();
    SceProcessmgr_finish();
    SceSqlite_finish();
}

static int launch(SceKernelThreadEntry entry)
{
    int ret;

    LOG("Entering SceKernelThreadMgr...");

    ret = SceKernelThreadMgr_main_entry(entry, 0, NULL);

    LOG("SceKernelThreadMgr returned...");

    host_finish();

    LOG("Process finished! Returned: %d", ret);

    return ret;
}

static void create_vita2native_paths(void)
{
    mkdir(VITA2NATIVE_ROOT_PATH, 0755);
    mkdir(VITA2NATIVE_DUMP_PATH, 0755);
    mkdir(VITA2NATIVE_DUMP_SHADER_PATH, 0755);
}

/*
 * app0: is either the directory containing the executable or a directory
 * populated from the VPK. Returns 0 on success.
 */
static int prepare_app_dir(const char *exe_path, char *out, size_t out_size)
{
    const char *slash;
    size_t len;

    if (vpk_is_archive(exe_path)) {
        if (snprintf(out, out_size, "%s/app0", VITA2NATIVE_ROOT_PATH) >= (int)out_size)
            return -1;
        return vpk_extract(exe_path, out);
    }

    slash = strrchr(exe_path, '/');
    if (!slash) {
        if (snprintf(out, out_size, ".") >= (int)out_size)
            return -1;
        return 0;
    }

    len = (size_t)(slash - exe_path);
    if (len == 0)
        len = 1; /* executable in the filesystem root */

    if (snprintf(out, out_size, "%.*s", (int)len, exe_path) >= (int)out_size)
        return -1;

    return 0;
}

int main(int argc, char *argv[])
{
    const char *exe_path = VITA2NATIVE_EXE_FILE;
    char app_dir[PATH_MAX];
    time_t now;
    char timestr[32];
    Jit jit;
    void *entry;
    LoadedModuleInfo main_info;
    int ret;

#if defined(__GLIBC__)
    /* Keep the 32-bit address space and RSS in check on handhelds. This used
       to be a launcher setting (MALLOC_ARENA_MAX=2). */
    mallopt(M_ARENA_MAX, 2);
#endif

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--headless") == 0)
                g_headless = true;
            else if (argv[i][0] != '-')
                exe_path = argv[i];
        }
    }

#if defined(VITA2NATIVE_HEADLESS_BUILD)
    /* The display backend presents through SDL, so the GLES context is always
       offscreen. Direct GPU presentation is the exception: a KMS target needs
       a window-capable EGL display for the scanout surface. */
    if (!g_headless)
        g_headless = !host_display_uses_kms();
#else
    if (!g_headless && !host_display_uses_kms() && !getenv("DISPLAY") &&
        !getenv("WAYLAND_DISPLAY"))
        g_headless = true;
#endif

    create_vita2native_paths();

    if (prepare_app_dir(exe_path, app_dir, sizeof(app_dir)) != 0)
        fatal_error("Could not prepare the application directory.", exe_path);

#if defined(__arm__)
    check_host_cpu();
    install_guest_signal_handlers();
#endif

    if (host_fs_init(VITA2NATIVE_ROOT_PATH, app_dir) != 0)
        fatal_error("Could not initialize the Vita filesystem.", "Check the vita2native directory.");

    /* Initialize netlog */
    netlog_init();

    log_init(LOG_DEVICE_STDERR | LOG_DEVICE_NETLOG);

    LOG("vita2native " VITA2NATIVE_MAJOR "." VITA2NATIVE_MINOR "." VITA2NATIVE_PATCH "-"
        VITA2NATIVE_HASH);

    now = time(NULL);
    strftime(timestr, sizeof(timestr), "%c", localtime(&now));
    LOG("Starting at %s", timestr);

    if (host_init() != 0)
        fatal_error("Could not initialize the host services.", "Check the log for details.");

    /*
     * The firmware loads the main executable first (which publishes the process
     * parameters) and then its user modules. Follow that order: libc.suprx and
     * Unity read the process parameter block while they initialise.
     */
    ret = load_file_info(&jit, exe_path, &entry, &main_info);
    if (ret == 0) {
        void *process_param = load_find_export_var(&main_info, 0x70FBA1E7 /* NID_PROCESS_PARAM */);

        if (process_param) {
            LOG("Main executable process param at %p", process_param);
            SceProcessmgr_set_process_param(process_param);

            if (getenv("VITA2NATIVE_TRACE_PROCESS_PARAM")) {
                const uint32_t *words = process_param;

                for (unsigned i = 0; i < 40; i += 8)
                    LOG("process_param[%02u] %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                        " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                        i, words[i], words[i + 1], words[i + 2], words[i + 3], words[i + 4],
                        words[i + 5], words[i + 6], words[i + 7]);

                if (words[11]) {
                    const uint32_t *libc = (const uint32_t *)(uintptr_t)words[11];

                    for (unsigned i = 0; i < 80; i += 8)
                        LOG("libc_param[%02u] %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                            " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                            i, libc[i], libc[i + 1], libc[i + 2], libc[i + 3], libc[i + 4],
                            libc[i + 5], libc[i + 6], libc[i + 7]);
                }
            }
        }

        /* Load and start the user modules the executable depends on. */
        if (SceModulemgr_load_app_modules() > 0) {
            SceModulemgr_repatch_modules();
            load_repatch_imports(&main_info);
            SceModulemgr_start_modules();
        }
        /*
         * Debug helpers:
         *   VITA2NATIVE_PATCH_BKPT=<offset>   plant `udf #0` in the image
         *   VITA2NATIVE_PATCH=<off>=<hex16>,...  patch 16-bit instructions
         * Both help inspect guest state without a writable JIT mapping.
         */
        const char *breakpoint = getenv("VITA2NATIVE_PATCH_BKPT");
        const char *patch_list = getenv("VITA2NATIVE_PATCH");

        g_main_image_rw = (uintptr_t)main_info.code_rw;
        g_main_image_size = jit.size;

#if defined(__arm__)
        /*
         * VITA2NATIVE_TRACE_TARGET=<vita address>[,<address>...] records the
         * arguments of every call to the given functions. Useful for the
         * allocator entry points, which are only reachable through
         * movw/movt/blx sequences.
         */
        {
            const char *targets = getenv("VITA2NATIVE_TRACE_TARGET");

            LOG("Trace: env=%s rx=%p rw=%p", targets ? targets : "(unset)",
                main_info.code_rx, main_info.code_rw);

            if (targets && jit.rw) {
                size_t stub_cursor =
                    (image_code_end(&main_info, (uintptr_t)main_info.code_rx) + 3) & ~(size_t)3;
                const char *cursor = targets;

                while (*cursor) {
                    char *target_end = NULL;
                    unsigned long target = strtoul(cursor, &target_end, 0);

                    install_target_trace(&jit, &main_info, (uint32_t)target, &stub_cursor);
                    cursor = (*target_end == ',') ? target_end + 1 : target_end;
                    if (!*cursor)
                        break;
                }
            }
        }
#endif

        if (breakpoint && jit.rw) {
            unsigned long offset = strtoul(breakpoint, NULL, 0);

            if (offset + 2 <= jit.size) {
                const uint16_t udf = 0xde00;

                memcpy((uint8_t *)jit.rw + offset, &udf, sizeof(udf));
                __builtin___clear_cache((char *)jit.rw, (char *)jit.rw + jit.size);
                LOG("Patched guest code at 0x%lx with udf #0", offset);
            }
        }

        if (patch_list && jit.rw) {
            const char *cursor = patch_list;

            while (*cursor) {
                char *end = NULL;
                unsigned long offset = strtoul(cursor, &end, 0);

                if (!end || *end != '=') {
                    LOG("VITA2NATIVE_PATCH: expected <offset>=<value> in \"%s\"", cursor);
                    break;
                }

                unsigned long value = strtoul(end + 1, &end, 16);

                if (offset + 2 <= jit.size) {
                    const uint16_t halfword = (uint16_t)value;

                    memcpy((uint8_t *)jit.rw + offset, &halfword, sizeof(halfword));
                    LOG("Patched guest code at 0x%lx with 0x%04lx", offset, value);
                }

                cursor = (*end == ',') ? end + 1 : end;
                if (!*cursor)
                    break;
            }

            __builtin___clear_cache((char *)jit.rw, (char *)jit.rw + jit.size);
        }

        LOG("Launching PlayStation Vita executable!");

#if defined(__arm__)
        /* Snapshot the host mappings before any guest code runs, so heap
           regions created by the title can be told apart from ours. */
        if (getenv("VITA2NATIVE_DUMP_MAPS")) {
            dump_maps_around(0);
            dump_big_anon_mappings("prelaunch");
        }
#endif

        /* Jump to Vita's executable entrypoint */
        ret = launch(entry);

        LOG("Returned from launch with result: %d", ret);

        /* Close the JIT */
        ret = jitClose(&jit);
        LOG("jitClose() returned: 0x%x", ret);
    } else {
        char message[PATH_MAX + 64];

        snprintf(message, sizeof(message), "Make sure to place it at '%s'", exe_path);
        fatal_error("Error loading PlayStation Vita executable.", message);
    }

    return 0;
}

void NORETURN fatal_error(const char *message, const char *detail)
{
    LOG("%s %s", message, detail);
    fprintf(stderr, "vita2native: %s: %s\n", message, detail);
    fflush(stderr);
    exit(1);
}
