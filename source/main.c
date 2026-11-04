#include "config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
#include "modules/SceDisplay.h"
#include "modules/SceGxm.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceSysmem.h"
#include "modules/SceTouch.h"

#if !defined(VITA2NATIVE_LINUX)
#include <deko3d.h>
#include <switch.h>
#endif

#if defined(VITA2NATIVE_LINUX) && defined(__arm__)
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

static void guest_signal_handler(int signal, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    char message[256];
    int length;

    (void)info;

    length = snprintf(message, sizeof(message),
                      "vita2native: guest signal %d at pc=0x%08lx lr=0x%08lx sp=0x%08lx\n", signal,
                      (unsigned long)uc->uc_mcontext.arm_pc,
                      (unsigned long)uc->uc_mcontext.arm_lr,
                      (unsigned long)uc->uc_mcontext.arm_sp);
    if (length > 0)
        write(STDERR_FILENO, message, (size_t)length);

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

#if !defined(VITA2NATIVE_LINUX)
static void dk_debug_callback(void *userData, const char *context, DkResult result,
                              const char *message)
{
    char description[256];

    if (result == DkResult_Success) {
        LOG("deko3d debug callback: context: %s, message: %s, result %d", context, message, result);
    } else {
        snprintf(description, sizeof(description), "context: %s, message: %s, result %d", context,
                 message, result);
        fatal_error("deko3d fatal error.", description);
    }
}
#endif

static int launch(SceKernelThreadEntry entry)
{
    HostGpuDevice device;
    int ret;

#if !defined(VITA2NATIVE_LINUX)
    DkDeviceMaker device_maker;
    DkDevice dk_device;

    /* Initialize deko3d device */
    dkDeviceMakerDefaults(&device_maker);
    device_maker.userData = NULL;
    device_maker.cbDebug = dk_debug_callback;
    device_maker.flags = DkDeviceFlags_YAxisPointsDown;
    dk_device = dkDeviceCreate(&device_maker);
    device = dk_device;
#else
    HostGpuConfig gpu_config = {960, 544, true};
    device = host_gpu_create(&gpu_config);
    if (!device)
        return -1;
    LOG("GPU: %s", host_gpu_description(device));
#endif

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

    LOG("Entering SceKernelThreadMgr...");

    ret = SceKernelThreadMgr_main_entry(entry, 0, NULL);

    LOG("SceKernelThreadMgr returned...");

    SceGxm_finish();
    SceDisplay_finish();
    SceKernelThreadMgr_finish();

#if !defined(VITA2NATIVE_LINUX)
    dkDeviceDestroy(dk_device);
#endif

    LOG("Process finished! Returned: %d", ret);

done:
    return ret;
}

static void create_vita2hos_paths()
{
    mkdir(VITA2HOS_ROOT_PATH, 0755);
    mkdir(VITA2HOS_DUMP_PATH, 0755);
    mkdir(VITA2HOS_DUMP_SHADER_PATH, 0755);
}

#if defined(VITA2NATIVE_LINUX)
/*
 * app0: is either the directory containing the executable or a directory
 * populated from the VPK. Returns 0 on success.
 */
static int prepare_app_dir(const char *exe_path, char *out, size_t out_size)
{
    const char *slash;
    size_t len;

    if (vpk_is_archive(exe_path)) {
        if (snprintf(out, out_size, "%s/app0", VITA2HOS_ROOT_PATH) >= (int)out_size)
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
#endif

int main(int argc, char *argv[])
{
    const char *exe_path = VITA2HOS_EXE_FILE;
    time_t now;
    char timestr[32];
#if defined(VITA2NATIVE_LINUX)
    char app_dir[PATH_MAX];
#endif
    Jit jit;
    void *entry;
    int ret;

#if defined(VITA2NATIVE_LINUX)
    if (argc > 1)
        exe_path = argv[1];
#endif

    create_vita2hos_paths();

#if defined(VITA2NATIVE_LINUX)
    if (prepare_app_dir(exe_path, app_dir, sizeof(app_dir)) != 0)
        fatal_error("Could not prepare the application directory.", exe_path);

#if defined(__arm__)
    check_host_cpu();
    install_guest_signal_handlers();
#endif
    if (host_fs_init(VITA2HOS_ROOT_PATH, app_dir) != 0)
        fatal_error("Could not initialize the Vita filesystem.", "Check the vita2native directory.");
#else
    /* Initialize the socket library */
    socketInitializeDefault();
#endif

    /* Initialize netlog */
    netlog_init();

    log_init(LOG_DEVICE_SVC | LOG_DEVICE_NETLOG);

#if defined(VITA2NATIVE_LINUX)
    LOG("vita2native " VITA2HOS_MAJOR "." VITA2HOS_MINOR "." VITA2HOS_PATCH "-" VITA2HOS_HASH
        " (" __DATE__ " " __TIME__ ")");
#else
    LOG("vita2hos " VITA2HOS_MAJOR "." VITA2HOS_MINOR "." VITA2HOS_PATCH "-" VITA2HOS_HASH
        " (" __DATE__ " " __TIME__ ")");
#endif

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

void NORETURN fatal_error(const char *dialog_message, const char *fullscreen_message)
{
#if defined(VITA2NATIVE_LINUX)
    LOG("%s %s", dialog_message, fullscreen_message);
    fprintf(stderr, "vita2native: %s: %s\n", dialog_message, fullscreen_message);
    fflush(stderr);
    exit(1);
#else
    extern u32 __nx_applet_exit_mode;
    ErrorApplicationConfig c;

    LOG("%s %s", dialog_message, fullscreen_message);

    errorApplicationCreate(&c, dialog_message, fullscreen_message);
    errorApplicationShow(&c);

    __nx_applet_exit_mode = 1;
    exit(1);
#endif
}

void NORETURN __assert_func(const char *file, int line, const char *func, const char *failedexpr)
{
    char message[256];

    snprintf(message, sizeof(message), "assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
             failedexpr, file, line, func ? ", function: " : "", func ? func : "");

    fatal_error("Assertion failed.", message);
}
