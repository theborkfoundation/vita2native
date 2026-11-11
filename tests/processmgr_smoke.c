/*
 * Exercises SceProcessmgr: identity queries, clocks, libc time wrappers and
 * process-termination callbacks.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/types.h>
#include <psp2/kernel/processmgr.h>

#include "modules/SceProcessmgr.h"
#include "test.h"

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

SceUID sceKernelGetCurrentProcess(void);
int sceKernelGetProcessName(char *name, SceUInt32 length);
int sceKernelGetProcessTitleId(char *title, SceUInt32 length);
const void *sceKernelGetProcessParam(void);
int sceKernelGetProcessTimeCore(SceKernelSysClock *clock);
SceUInt32 sceKernelGetProcessTimeLowCore(void);
SceUInt64 sceKernelGetProcessTimeWideCore(void);
int sceKernelIsCDialogAvailable(void);
int sceKernelIsGameBudget(void);
VitaTm *sceKernelLibcGmtime_r(const SceKernelTime *timer, VitaTm *result);
VitaTm *sceKernelLibcLocaltime_r(const SceKernelTime *timer, VitaTm *result);
SceKernelTime sceKernelLibcMktime(const VitaTm *time_ptr);
int sceKernelRegisterProcessTerminationCallback(void (*callback)(void *), void *arg);
int sceKernelUnregisterProcessTerminationCallback(void (*callback)(void *));

/* Referenced by _sceKernelExitProcessForUser, which this test does not call. */
int sceKernelExitProcess(int res)
{
    (void)res;
    return 0;
}

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
} TestProcessParam;

static int g_callback_count;
static void *g_callback_arg;

static void termination_callback(void *arg)
{
    g_callback_count++;
    g_callback_arg = arg;
}

int main(void)
{
    const TestProcessParam *param;
    SceKernelSysClock clock = 0;
    SceUInt64 before, after;
    VitaTm tm;
    char buffer[32];
    SceKernelTime now;
    int marker;

    REQUIRE(SceProcessmgr_init() == 0, "init failed");

    REQUIRE(sceKernelGetCurrentProcess() == 1, "current process is not 1");

    memset(buffer, 0, sizeof(buffer));
    REQUIRE(sceKernelGetProcessName(buffer, sizeof(buffer)) == 0, "GetProcessName failed");
    REQUIRE(strcmp(buffer, "vita2native") == 0, "unexpected process name");
    REQUIRE(sceKernelGetProcessName(buffer, 0) < 0, "a zero-length name buffer was accepted");

    memset(buffer, 0, sizeof(buffer));
    REQUIRE(sceKernelGetProcessTitleId(buffer, sizeof(buffer)) == 0, "GetProcessTitleId failed");
    REQUIRE(strcmp(buffer, "V2N000001") == 0, "unexpected title id");

    param = sceKernelGetProcessParam();
    REQUIRE(param != NULL, "GetProcessParam returned NULL");
    REQUIRE(param->size == sizeof(TestProcessParam), "the process param size is wrong");
    REQUIRE(strcmp(param->process_name, "vita2native") == 0, "the process name pointer is wrong");

    REQUIRE(sceKernelGetProcessTimeCore(&clock) == 0, "GetProcessTimeCore failed");
    before = sceKernelGetProcessTimeWideCore();
    REQUIRE(before > 0, "the process time is zero");
    after = sceKernelGetProcessTimeWideCore();
    REQUIRE(after >= before, "the process time went backwards");
    REQUIRE(sceKernelGetProcessTimeLowCore() >= (SceUInt32)before,
            "the low process time went backwards");

    REQUIRE(sceKernelIsCDialogAvailable() == 1, "common dialogs are reported unavailable");
    REQUIRE(sceKernelIsGameBudget() == 1, "no game budget");

    REQUIRE(sceKernelLibcClock() > 0, "LibcClock returned zero");

    now = sceKernelLibcTime(NULL);
    REQUIRE(now > 0, "LibcTime returned zero");

    memset(&tm, 0, sizeof(tm));
    REQUIRE(sceKernelLibcGmtime_r(&now, &tm) == &tm, "Gmtime_r failed");
    REQUIRE(tm.tm_year >= 120 && tm.tm_mon >= 0 && tm.tm_mon < 12, "Gmtime_r filled bad fields");
    REQUIRE(sceKernelLibcMktime(&tm) > 0, "Mktime failed");

    memset(&tm, 0, sizeof(tm));
    REQUIRE(sceKernelLibcLocaltime_r(&now, &tm) == &tm, "Localtime_r failed");
    REQUIRE(tm.tm_year >= 120, "Localtime_r filled bad fields");

    REQUIRE(sceKernelPowerLock(SCE_KERNEL_POWER_TICK_DEFAULT) == 0, "PowerLock failed");
    REQUIRE(sceKernelPowerUnlock(SCE_KERNEL_POWER_TICK_DEFAULT) == 0, "PowerUnlock failed");
    REQUIRE(sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND) == 0,
            "PowerTick failed");

    g_callback_count = 0;
    marker = 42;
    REQUIRE(sceKernelRegisterProcessTerminationCallback(termination_callback, &marker) == 0,
            "could not register the termination callback");
    SceProcessmgr_run_termination_callbacks();
    REQUIRE(g_callback_count == 1, "the termination callback did not run");
    REQUIRE(g_callback_arg == &marker, "the termination callback lost its argument");

    REQUIRE(sceKernelRegisterProcessTerminationCallback(termination_callback, &marker) == 0,
            "could not re-register the termination callback");
    REQUIRE(sceKernelUnregisterProcessTerminationCallback(termination_callback) == 0,
            "could not unregister the termination callback");
    g_callback_count = 0;
    SceProcessmgr_run_termination_callbacks();
    REQUIRE(g_callback_count == 0, "an unregistered callback still ran");

    REQUIRE(SceProcessmgr_finish() == 0, "finish failed");

    puts("processmgr_smoke: ok");
    return 0;
}
