#include "config.h"

#include <limits.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
#include <execinfo.h>
#include <signal.h>
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
/* Base of the main executable's image, for crash-time diagnostics. */
static uintptr_t g_main_image_rw;
static size_t g_main_image_size;

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

    /* The first argument often points at the structure being processed. */
    dump_guest_words((unsigned long)uc->uc_mcontext.arm_r0, 12);
    dump_guest_words((unsigned long)uc->uc_mcontext.arm_sp, 64);

    {
        void *frames[24];
        int count = backtrace(frames, (int)ARRAY_SIZE(frames));

        backtrace_symbols_fd(frames, count, STDERR_FILENO);
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
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
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
       offscreen. Letting DISPLAY select the windowing EGL platform would make
       every GL call round-trip through the X server. */
    g_headless = true;
#else
    if (!g_headless && !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
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
