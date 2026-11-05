/*
 * SceProcessmgr: process identity, clocks, power ticks and termination hooks.
 *
 * There is exactly one process, so identity queries are constants. The libc
 * wrappers are real implementations; the termination callbacks are stored and
 * invoked when the guest calls sceKernelExitProcess().
 */

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2common/kernel/processmgr.h>

#include "host.h"
#include "module.h"
#include "modules/SceLibKernel.h"
#include "modules/SceProcessmgr.h"

#define VITA_TITLE_ID         "V2N000001"
#define VITA_PROCESS_NAME     "vita2native"
#define VITA_MAX_CALLBACKS    8

typedef struct {
    SceUInt32 size;
    SceUInt32 magic;
    SceUInt32 version;
    SceUInt32 fw_version;
    const char *main_thread_name;
    SceInt32 main_thread_priority;
    SceUInt32 main_thread_stacksize;
    SceUInt32 main_thread_attribute;
    const char *process_name;
    SceUInt32 process_preload_disabled;
    SceUInt32 main_thread_cpu_affinity_mask;
    const void *sce_libc_param;
    SceUInt32 unk;
} VitaProcessParam;

static VitaProcessParam g_process_param = {
    .size = sizeof(VitaProcessParam),
    .magic = 0x5052504D, /* "PRPM" */
    .version = 1,
    .main_thread_name = "main",
    .main_thread_priority = 160,
    .main_thread_stacksize = 256 * 1024,
    .process_name = VITA_PROCESS_NAME,
    .main_thread_cpu_affinity_mask = 0x00070000,
};

typedef void (*VitaTerminationCallback)(void *arg);

typedef struct {
    bool used;
    VitaTerminationCallback callback;
    void *arg;
} VitaTerminationEntry;

static VitaTerminationEntry g_termination_callbacks[VITA_MAX_CALLBACKS];
static pthread_mutex_t g_termination_lock = PTHREAD_MUTEX_INITIALIZER;

void SceProcessmgr_run_termination_callbacks(void)
{
    VitaTerminationEntry callbacks[VITA_MAX_CALLBACKS];

    pthread_mutex_lock(&g_termination_lock);
    memcpy(callbacks, g_termination_callbacks, sizeof(callbacks));
    memset(g_termination_callbacks, 0, sizeof(g_termination_callbacks));
    pthread_mutex_unlock(&g_termination_lock);

    for (int i = 0; i < VITA_MAX_CALLBACKS; i++) {
        if (callbacks[i].used && callbacks[i].callback)
            callbacks[i].callback(callbacks[i].arg);
    }
}

int SceProcessmgr_init(void)
{
    return 0;
}

int SceProcessmgr_finish(void)
{
    memset(g_termination_callbacks, 0, sizeof(g_termination_callbacks));
    return 0;
}

EXPORT(SceProcessmgr, 0xC053DC6B, int, _sceKernelExitProcessForUser, int res)
{
    return sceKernelExitProcess(res);
}

/* ARM coprocessor timer 5 is not readable from user mode; report zero. */
EXPORT(SceProcessmgr, 0x2F73D72F, SceUInt32, _sceKernelGetTimer5Reg, void)
{
    return 0;
}

EXPORT(SceProcessmgr, 0x56C2E8FF, int, _sceKernelRegisterLibkernelAddresses, const void *addresses)
{
    (void)addresses;
    return 0;
}

EXPORT(SceProcessmgr, 0xDB4CC1D0, int, sceKernelCDialogSessionClose, int session_id)
{
    (void)session_id;
    return 0;
}

EXPORT(SceProcessmgr, 0xEC8DDAAD, int, sceKernelCDialogSetLeaseLimit, int limit)
{
    (void)limit;
    return 0;
}

EXPORT(SceProcessmgr, 0xEB6E50BB, int, sceKernelCallAbortHandler, void)
{
    return 0;
}

EXPORT(SceProcessmgr, 0xCD248267, SceUID, sceKernelGetCurrentProcess, void)
{
    return 1;
}

EXPORT(SceProcessmgr, 0x2D635A00, SceUID, sceKernelGetExtraTty, void)
{
    return SCE_ERROR_ERRNO_EBADF;
}

EXPORT(SceProcessmgr, 0x10C52C95, int, sceKernelGetProcessName, char *process_name,
       SceUInt32 length)
{
    if (!process_name || length == 0 || length > 32)
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;

    strncpy(process_name, VITA_PROCESS_NAME, length);
    process_name[length - 1] = '\0';
    return 0;
}

EXPORT(SceProcessmgr, 0x2BE3E066, const void *, sceKernelGetProcessParam, void)
{
    return &g_process_param;
}

static void process_time_clock(SceKernelSysClock *clock)
{
    if (clock)
        *clock = (SceKernelSysClock)(host_monotonic_ns() / 1000ull);
}

EXPORT(SceProcessmgr, 0xD37A8437, int, sceKernelGetProcessTimeCore, SceKernelSysClock *clock)
{
    process_time_clock(clock);
    return 0;
}

EXPORT(SceProcessmgr, 0xF5D0D4C6, SceUInt32, sceKernelGetProcessTimeLowCore, void)
{
    return (SceUInt32)(host_monotonic_ns() / 1000ull);
}

EXPORT(SceProcessmgr, 0x89DA0967, SceUInt64, sceKernelGetProcessTimeWideCore, void)
{
    return host_monotonic_ns() / 1000ull;
}

EXPORT(SceProcessmgr, 0x03A48771, int, sceKernelGetProcessTitleId, char *title_id,
       SceUInt32 length)
{
    if (!title_id || length == 0 || length > 32)
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;

    strncpy(title_id, VITA_TITLE_ID, length);
    title_id[length - 1] = '\0';
    return 0;
}

EXPORT(SceProcessmgr, 0xE6E9FCA3, int, sceKernelGetRemoteProcessTime, SceUID process_id,
       SceKernelSysClock *clock)
{
    (void)process_id;

    process_time_clock(clock);
    return 0;
}

/*
 * There is no tty device on the host. Reporting an error is better than a
 * descriptor that sceIoWrite() would reject anyway.
 */
EXPORT(SceProcessmgr, 0xFA5E3ADA, SceUID, sceKernelGetStderr, void)
{
    return SCE_ERROR_ERRNO_EBADF;
}

EXPORT(SceProcessmgr, 0xC1727F59, SceUID, sceKernelGetStdin, void)
{
    return SCE_ERROR_ERRNO_EBADF;
}

EXPORT(SceProcessmgr, 0xE5AA625C, SceUID, sceKernelGetStdout, void)
{
    return SCE_ERROR_ERRNO_EBADF;
}

/* SceCommonDialog is implemented (dialogs complete immediately). */
EXPORT(SceProcessmgr, 0x143BC4D6, int, sceKernelIsCDialogAvailable, void)
{
    return 1;
}

EXPORT(SceProcessmgr, 0xCE0F02F0, int, sceKernelIsGameBudget, void)
{
    return 1;
}

EXPORT(SceProcessmgr, 0x9E45DA09, SceKernelClock, sceKernelLibcClock, void)
{
    return (SceKernelClock)clock();
}

EXPORT(SceProcessmgr, 0x0039BE45, SceKernelTime, sceKernelLibcTime, SceKernelTime *time_location)
{
    time_t now = time(NULL);

    if (time_location)
        *time_location = (SceKernelTime)now;
    return (SceKernelTime)now;
}

/*
 * The guest's `struct tm` is the Newlib layout: nine ints. The host's struct
 * has extra fields, so translate through a local copy.
 */
typedef struct {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
} VitaTm;

static void host_tm_to_vita(const struct tm *src, VitaTm *dst)
{
    dst->tm_sec = src->tm_sec;
    dst->tm_min = src->tm_min;
    dst->tm_hour = src->tm_hour;
    dst->tm_mday = src->tm_mday;
    dst->tm_mon = src->tm_mon;
    dst->tm_year = src->tm_year;
    dst->tm_wday = src->tm_wday;
    dst->tm_yday = src->tm_yday;
    dst->tm_isdst = src->tm_isdst;
}

static void vita_tm_to_host(const VitaTm *src, struct tm *dst)
{
    memset(dst, 0, sizeof(*dst));
    dst->tm_sec = src->tm_sec;
    dst->tm_min = src->tm_min;
    dst->tm_hour = src->tm_hour;
    dst->tm_mday = src->tm_mday;
    dst->tm_mon = src->tm_mon;
    dst->tm_year = src->tm_year;
    dst->tm_wday = src->tm_wday;
    dst->tm_yday = src->tm_yday;
    dst->tm_isdst = src->tm_isdst;
}

EXPORT(SceProcessmgr, 0xBCA437CD, VitaTm *, sceKernelLibcGmtime_r, const SceKernelTime *timer,
       VitaTm *result)
{
    time_t now = timer ? (time_t)*timer : time(NULL);
    struct tm host;

    if (!result)
        return NULL;

    gmtime_r(&now, &host);
    host_tm_to_vita(&host, result);
    return result;
}

EXPORT(SceProcessmgr, 0x94F041ED, VitaTm *, sceKernelLibcLocaltime_r, const SceKernelTime *timer,
       VitaTm *result)
{
    time_t now = timer ? (time_t)*timer : time(NULL);
    struct tm host;

    if (!result)
        return NULL;

    localtime_r(&now, &host);
    host_tm_to_vita(&host, result);
    return result;
}

EXPORT(SceProcessmgr, 0x890BDC39, SceKernelTime, sceKernelLibcMktime, const VitaTm *time_ptr)
{
    struct tm host;

    if (!time_ptr)
        return (SceKernelTime)-1;

    vita_tm_to_host(time_ptr, &host);
    return (SceKernelTime)mktime(&host);
}

EXPORT(SceProcessmgr, 0x7AA73378, int, sceKernelPowerLock, SceKernelPowerTickType type)
{
    (void)type;
    return 0;
}

EXPORT(SceProcessmgr, 0x466C0CBD, int, sceKernelPowerUnlock, SceKernelPowerTickType type)
{
    (void)type;
    return 0;
}

EXPORT(SceProcessmgr, 0x2252890C, int, sceKernelPowerTick, SceKernelPowerTickType type)
{
    (void)type;
    return 0;
}

EXPORT(SceProcessmgr, 0x5EC77870, int, sceKernelRegisterProcessTerminationCallback,
       VitaTerminationCallback callback, void *arg)
{
    int ret = SCE_KERNEL_ERROR_NO_MEMORY;

    if (!callback)
        return SCE_KERNEL_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&g_termination_lock);
    for (int i = 0; i < VITA_MAX_CALLBACKS; i++) {
        if (g_termination_callbacks[i].used)
            continue;
        g_termination_callbacks[i].used = true;
        g_termination_callbacks[i].callback = callback;
        g_termination_callbacks[i].arg = arg;
        ret = 0;
        break;
    }
    pthread_mutex_unlock(&g_termination_lock);

    return ret;
}

EXPORT(SceProcessmgr, 0x973A4527, int, sceKernelUnregisterProcessTerminationCallback,
       VitaTerminationCallback callback)
{
    int ret = SCE_KERNEL_ERROR_INVALID_ARGUMENT;

    pthread_mutex_lock(&g_termination_lock);
    for (int i = 0; i < VITA_MAX_CALLBACKS; i++) {
        if (!g_termination_callbacks[i].used || g_termination_callbacks[i].callback != callback)
            continue;
        memset(&g_termination_callbacks[i], 0, sizeof(g_termination_callbacks[i]));
        ret = 0;
        break;
    }
    pthread_mutex_unlock(&g_termination_lock);

    return ret;
}

DECLARE_LIBRARY(SceProcessmgr, 0x2dd91812);
