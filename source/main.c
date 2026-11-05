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
#include "modules/SceDisplay.h"
#include "modules/SceGxm.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceSysmem.h"
#include "modules/SceTouch.h"

#if defined(__arm__)
#include <execinfo.h>
#include <signal.h>
#include <sys/auxv.h>
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

    dump_maps_around(info ? (unsigned long)info->si_addr : 0UL);

    {
        void *frames[24];
        int count = backtrace(frames, (int)ARRAY_SIZE(frames));

        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }

    _exit(128 + signal);
}

static void install_guest_signal_handlers(void)
{
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guest_signal_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);

    for (size_t i = 0; i < ARRAY_SIZE(signals); i++)
        sigaction(signals[i], &action, NULL);
}
#endif

static bool g_headless;

static int launch(SceKernelThreadEntry entry)
{
    HostGpuConfig gpu_config = { 960, 544, g_headless };
    HostGpuDevice device;
    int ret;

    device = host_gpu_create(&gpu_config);
    if (!device)
        return -1;
    LOG("GPU: %s", host_gpu_description(device));

    /* Init modules */
    ret = SceSysmem_init(device);
    if (ret != 0)
        goto done;
    ret = SceLibKernel_init();
    if (ret != 0)
        goto done;
    ret = SceKernelThreadMgr_init();
    if (ret != 0)
        goto done;
    ret = SceDisplay_init(device);
    if (ret != 0)
        goto done;
    ret = SceGxm_init(device);
    if (ret != 0)
        goto done;
    ret = SceCtrl_init();
    if (ret != 0)
        goto done;
    ret = SceTouch_init();
    if (ret != 0)
        goto done;
    ret = SceAudio_init();
    if (ret != 0)
        goto done;

    LOG("Entering SceKernelThreadMgr...");

    ret = SceKernelThreadMgr_main_entry(entry, 0, NULL);

    LOG("SceKernelThreadMgr returned...");

    SceGxm_finish();
    SceDisplay_finish();
    SceAudio_finish();
    SceKernelThreadMgr_finish();

    LOG("Process finished! Returned: %d", ret);

done:
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
    int ret;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--headless") == 0)
                g_headless = true;
            else if (argv[i][0] != '-')
                exe_path = argv[i];
        }
    }

    if (!g_headless && !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
        g_headless = true;

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
        VITA2NATIVE_HASH " (" __DATE__ " " __TIME__ ")");

    now = time(NULL);
    strftime(timestr, sizeof(timestr), "%c", localtime(&now));
    LOG("Starting at %s", timestr);

    ret = load_file(&jit, exe_path, &entry);
    if (ret == 0) {
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
