/*
 * Exercises the SceThreadmgr surface that is not covered by sync_smoke:
 * thread signals, VM hooks, priority/VFP attributes, UID classification,
 * lightweight-object introspection and the underscore message-pipe vectors.
 */

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/lw_cond.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <psp2common/kernel/threadmgr.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "modules/SceKernelSync.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"

/* The module allocates UIDs this way; SceSysmem itself is not linked here. */
static _Atomic SceUID g_next_uid = 1;

SceUID SceSysmem_get_next_uid(void)
{
    return atomic_fetch_add(&g_next_uid, 1);
}

static UEvent g_exit_event;

UEvent *SceLibKernel_get_process_exit_uevent(void)
{
    return &g_exit_event;
}

void SceProcessmgr_run_termination_callbacks(void)
{
}

static atomic_int g_wait_signal_done;
static atomic_int g_signal_timeout_result;
static atomic_int g_vfp_old_first;
static atomic_int g_vfp_current;
static atomic_int g_vfp_old_second;

int sceKernelGetCurrentThreadVfpException(void);

static int wait_signal_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    if (sceKernelWaitSignal(0, 0, NULL) == 0)
        atomic_store(&g_wait_signal_done, 1);

    return 0;
}

static int vfp_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    atomic_store(&g_vfp_old_first, sceKernelChangeThreadVfpException(0, 0x1));
    atomic_store(&g_vfp_current, sceKernelGetCurrentThreadVfpException());
    atomic_store(&g_vfp_old_second, sceKernelChangeThreadVfpException(0x1, 0x80));
    atomic_store(&g_vfp_current, sceKernelGetCurrentThreadVfpException());

    return 0;
}

static int signal_timeout_entry(SceSize args, void *argp)
{
    SceUInt32 timeout = 5000;

    (void)args;
    (void)argp;

    atomic_store(&g_signal_timeout_result, sceKernelWaitSignal(0, 0, &timeout));
    return 0;
}

static int idle_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    return 0;
}

static int sleep_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    sceKernelDelayThread(50 * 1000);
    return 0;
}

#define CHECK(cond, message)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("threadmgr_smoke: %s (line %d)\n", message, __LINE__);                         \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);
int sceKernelWaitThreadEnd_089(SceUID thid, int *stat, SceUInt *timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelChangeThreadPriority(SceUID thid, int priority);
int sceKernelCreateCallback(const char *name, SceUInt32 attr, SceKernelCallbackFunction callback,
                            void *user_data);
int sceKernelDeleteCallback(SceUID callback_id);
int _sceKernelSendMsgPipeVector(SceUID uid, const void *vec, unsigned int count, int unk1,
                                void *result, unsigned int *timeout);
int _sceKernelReceiveMsgPipeVector(SceUID uid, const void *vec, unsigned int count, int unk1,
                                   void *result, unsigned int *timeout);
int _sceKernelTryReceiveMsgPipeVector(SceUID uid, const void *vec, SceSize count, int unk1,
                                      void *result);
int sceKernelGetLwMutexInfo(SceKernelLwMutexWork *work, VitaLwMutexInfo *info);
int sceKernelGetLwMutexInfoById(SceUID lwmutex_id, VitaLwMutexInfo *info);
int _sceKernelGetLwMutexInfoById(SceUID lwmutex_id, VitaLwMutexInfo *info, SceSize size);
int sceKernelGetLwCondInfo(SceKernelLwCondWork *work, VitaLwCondInfo *info);
int sceKernelGetLwCondInfoById(SceUID lwcond_id, VitaLwCondInfo *info);

typedef struct {
    void *message;
    SceSize size;
} TestVector;

typedef struct {
    SceSize size;
    SceUInt32 cpsr;
    SceUInt32 reg[16];
    SceUInt32 tpidrurw;
    SceUInt32 teehbr;
    SceUInt32 sb;
    SceUInt32 st;
} CpuRegisters;

typedef struct {
    SceSize size;
    SceUInt32 fpscr;
    float reg[64];
} VfpRegisters;

typedef struct {
    SceSize size;
    int stack_size;
    SceUInt attr;
    int cpu_affinity_mask;
    const SceKernelThreadOptParam *option;
} CreateThreadOptions;

int sceKernelGetThreadContextForVM(SceUID thid, CpuRegisters *cpu, VfpRegisters *vfp);
int sceKernelSetThreadContextForVM(SceUID thid, const CpuRegisters *cpu, const VfpRegisters *vfp);
int sceKernelSuspendThreadForVM(SceUID thid);
int sceKernelResumeThreadForVM(SceUID thid);
int sceKernelPMonThreadGetCounter(SceUID thid, int counter, SceUInt32 *value);
int sceKernelCheckWaitableStatus(void);
int sceKernelChangeActiveCpuMask(SceUInt32 mask);
int sceKernelChangeThreadPriority2(SceUID thid, int priority);
int sceKernelChangeThreadVfpException(SceUInt32 clear, SceUInt32 set);
int sceKernelGetCurrentThreadVfpException(void);
SceUID sceKernelCreateThreadForUser(const char *name, SceKernelThreadEntry entry,
                                    int init_priority, CreateThreadOptions *options);
SceKernelIdListType sceKernelGetThreadmgrUIDClass(SceUID uid);
int sceKernelGetThreadEventInfo(SceUID id, void *info);
int sceKernelWaitException(SceUInt32 unk0, SceUInt32 delay, SceUInt32 *timeout);
int sceKernelWaitMultipleEvents(SceUInt32 unk0, SceUInt32 delay, SceUInt32 *timeout);
int sceKernelSendSignal(SceUID thid);

int main(void)
{
    SceKernelLwMutexWork lw_mutex;
    SceKernelLwCondWork lw_cond;
    CreateThreadOptions options;
    SceKernelCallbackInfo callback_info;
    SceKernelThreadInfo thread_info;
    VitaLwMutexInfo mutex_info;
    VitaLwCondInfo cond_info;
    CpuRegisters cpu;
    VfpRegisters vfp;
    SceUInt32 counter = 0xffffffffu;
    SceUID thid, callback, evf, sema, mpp;
    int ret;

    ueventCreate(&g_exit_event, true);
    SceKernelThreadMgr_init();
    SceKernelSync_init();

    /* --- signals ------------------------------------------------------- */
    atomic_init(&g_wait_signal_done, 0);
    thid = sceKernelCreateThread("signal-waiter", wait_signal_entry, 100, 64 * 1024, 0, 0, NULL);
    CHECK(thid > 0, "could not create the signal waiter");

    /* Queue the signal while the thread is still dormant, so the second send
     * deterministically observes an unconsumed signal. */
    CHECK(sceKernelSendSignal(thid) == 0, "first signal was rejected");
    CHECK(sceKernelSendSignal(thid) == SCE_KERNEL_ERROR_ALREADY_SENT,
          "second signal was not rejected");

    CHECK(sceKernelStartThread(thid, 0, NULL) == 0, "could not start the signal waiter");
    CHECK(sceKernelWaitThreadEnd(thid, NULL, NULL) == 0, "waiting for the signal waiter failed");
    CHECK(atomic_load(&g_wait_signal_done) == 1, "the waiter did not observe the signal");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the signal waiter");

    atomic_init(&g_signal_timeout_result, -1);
    thid = sceKernelCreateThread("signal-timeout", signal_timeout_entry, 100, 64 * 1024, 0, 0,
                                 NULL);
    CHECK(thid > 0, "could not create the signal timeout thread");
    CHECK(sceKernelStartThread(thid, 0, NULL) == 0, "could not start the signal timeout thread");
    CHECK(sceKernelWaitThreadEnd(thid, NULL, NULL) == 0,
          "waiting for the signal timeout thread failed");
    CHECK(atomic_load(&g_signal_timeout_result) == SCE_KERNEL_ERROR_WAIT_TIMEOUT,
          "an unset signal did not time out");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the signal timeout thread");

    /* --- VFP attributes ------------------------------------------------ */
    atomic_init(&g_vfp_old_first, -1);
    atomic_init(&g_vfp_old_second, -1);
    atomic_init(&g_vfp_current, -1);

    thid = sceKernelCreateThread("vfp", vfp_entry, 100, 64 * 1024, 0, 0, NULL);
    CHECK(thid > 0, "could not create the VFP thread");
    CHECK(sceKernelStartThread(thid, 0, NULL) == 0, "could not start the VFP thread");
    CHECK(sceKernelWaitThreadEnd(thid, NULL, NULL) == 0, "waiting for the VFP thread failed");
    CHECK(atomic_load(&g_vfp_old_first) == 0, "the initial VFP exception value was not zero");
    CHECK(atomic_load(&g_vfp_old_second) == 0x1, "changing the VFP exception bits returned the wrong old value");
    CHECK(atomic_load(&g_vfp_current) == 0x80, "the VFP exception bits were not updated");
    CHECK(sceKernelChangeThreadVfpException(0, 0x20) == SCE_KERNEL_ERROR_INVALID_ARGUMENT,
          "an invalid VFP exception mask was accepted");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the VFP thread");

    /* --- priority, VM hooks, suspend/resume ---------------------------- */
    thid = sceKernelCreateThread("vm", sleep_entry, 100, 64 * 1024, 0, 0, NULL);
    CHECK(thid > 0, "could not create the VM thread");
    CHECK(sceKernelStartThread(thid, 0, NULL) == 0, "could not start the VM thread");

    ret = sceKernelChangeThreadPriority2(thid, 120);
    CHECK(ret == 100, "ChangeThreadPriority2 did not return the previous priority");
    ret = sceKernelChangeThreadPriority2(thid, 110);
    CHECK(ret == 120, "ChangeThreadPriority2 did not return the updated priority");
    CHECK(sceKernelChangeThreadPriority2(thid, 500) == SCE_KERNEL_ERROR_ILLEGAL_PRIORITY,
          "an illegal priority was accepted");

    memset(&cpu, 0, sizeof(cpu));
    memset(&vfp, 0, sizeof(vfp));
    cpu.size = sizeof(cpu);
    vfp.size = sizeof(vfp);
    CHECK(sceKernelGetThreadContextForVM(thid, &cpu, &vfp) == 0, "GetThreadContextForVM failed");
    CHECK(cpu.size == sizeof(cpu) && vfp.size == sizeof(vfp), "the context sizes were not written");
    CHECK(cpu.reg[0] == 0 && vfp.reg[0] == 0.0f, "the reported context was not zeroed");
    CHECK(sceKernelSetThreadContextForVM(thid, &cpu, &vfp) == 0, "SetThreadContextForVM failed");
    CHECK(sceKernelGetThreadContextForVM(0x7fff, &cpu, &vfp) ==
              SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID,
          "a bogus thread context request was accepted");

    CHECK(sceKernelSuspendThreadForVM(thid) == 0, "suspending a thread failed");
    CHECK(sceKernelResumeThreadForVM(thid) == 0, "resuming a thread failed");
    CHECK(sceKernelResumeThreadForVM(thid) == SCE_KERNEL_ERROR_NOT_SUSPENDED,
          "resuming a running thread was accepted");

    CHECK(sceKernelPMonThreadGetCounter(thid, 0, &counter) == 0, "PMon counter read failed");
    CHECK(counter == 0, "the PMon counter was not zeroed");
    CHECK(sceKernelCheckWaitableStatus() == 0, "CheckWaitableStatus failed");

    CHECK(sceKernelWaitThreadEnd(thid, NULL, NULL) == 0, "waiting for the VM thread failed");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the VM thread");

    CHECK(sceKernelChangeThreadPriority(0x7fff, 100) == SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID,
          "an unknown thread accepted a priority change");

    /* --- UID classification -------------------------------------------- */
    thid = sceKernelCreateThread("classify", idle_entry, 100, 64 * 1024, 0, 0, NULL);
    CHECK(thid > 0, "could not create the classification thread");
    CHECK(sceKernelGetThreadmgrUIDClass(thid) == SCE_KERNEL_TMID_Thread,
          "a thread UID was not classified as a thread");

    sema = sceKernelCreateSema("classify-sema", 0, 1, 1, NULL);
    CHECK(sema > 0, "could not create the classification semaphore");
    CHECK(sceKernelGetThreadmgrUIDClass(sema) == SCE_KERNEL_TMID_Semaphore,
          "a semaphore UID was not classified as a semaphore");

    evf = sceKernelCreateEventFlag("classify-evf", 0, 0, NULL);
    CHECK(evf > 0, "could not create the classification event flag");
    CHECK(sceKernelGetThreadmgrUIDClass(evf) == SCE_KERNEL_TMID_EventFlag,
          "an event flag UID was not classified as an event flag");

    mpp = sceKernelCreateMsgPipe("classify-mpp", 0, 0, 16, NULL);
    CHECK(mpp > 0, "could not create the classification message pipe");
    CHECK(sceKernelGetThreadmgrUIDClass(mpp) == SCE_KERNEL_TMID_Mpipe,
          "a message pipe UID was not classified as a message pipe");

    callback = sceKernelCreateCallback("classify-cb", 0, NULL, NULL);
    CHECK(callback > 0, "could not create the classification callback");
    CHECK(sceKernelGetThreadmgrUIDClass(callback) == SCE_KERNEL_TMID_Callback,
          "a callback UID was not classified as a callback");

    CHECK(sceKernelGetThreadmgrUIDClass(0x7fff) == (SceKernelIdListType)SCE_KERNEL_ERROR_UNKNOWN_UID,
          "an unknown UID was classified");

    CHECK(sceKernelGetThreadEventInfo(thid, &callback_info) == 0,
          "GetThreadEventInfo rejected a thread");
    CHECK(sceKernelGetThreadEventInfo(0x7fff, &callback_info) == SCE_KERNEL_ERROR_UNKNOWN_UID,
          "GetThreadEventInfo accepted an unknown UID");
    CHECK(sceKernelWaitException(0, 0, NULL) == 0, "WaitException failed");
    CHECK(sceKernelWaitMultipleEvents(0, 0, NULL) == 0, "WaitMultipleEvents failed");

    /* --- lightweight mutex introspection ------------------------------- */
    CHECK(sceKernelCreateLwMutex(&lw_mutex, "info-lw", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL) ==
              0,
          "could not create the lw mutex");
    CHECK(sceKernelLockLwMutex(&lw_mutex, 2, NULL) == 0, "could not lock the lw mutex");

    memset(&mutex_info, 0, sizeof(mutex_info));
    mutex_info.size = sizeof(mutex_info);
    CHECK(sceKernelGetLwMutexInfo(&lw_mutex, &mutex_info) == 0, "GetLwMutexInfo failed");
    CHECK(mutex_info.uid > 0, "the lw mutex UID was not reported");
    CHECK(strcmp(mutex_info.name, "info-lw") == 0, "the lw mutex name was not reported");
    CHECK(mutex_info.current_count == 2, "the lw mutex lock count was not reported");
    CHECK(mutex_info.work == &lw_mutex, "the lw mutex work area was not reported");

    {
        VitaLwMutexInfo by_id;
        uint8_t canary_buffer[0x28];

        memset(&by_id, 0, sizeof(by_id));
        by_id.size = sizeof(by_id);
        CHECK(sceKernelGetLwMutexInfoById(mutex_info.uid, &by_id) == 0,
              "GetLwMutexInfoById failed");
        CHECK(by_id.uid == mutex_info.uid, "the by-id lookup returned a different mutex");

        memset(canary_buffer, 0xa5, sizeof(canary_buffer));
        CHECK(_sceKernelGetLwMutexInfoById(mutex_info.uid, (VitaLwMutexInfo *)canary_buffer, 0x18) ==
                  0,
              "the sized lw mutex lookup failed");
        CHECK(*(SceUID *)(canary_buffer + 4) == mutex_info.uid,
              "the sized lookup did not write the UID");
        for (int i = 0x18; i < (int)sizeof(canary_buffer); i++) {
            CHECK(canary_buffer[i] == 0xa5, "the sized lookup overran the caller's buffer");
        }
    }

    CHECK(sceKernelGetLwMutexInfoById(0x7fff, &mutex_info) ==
              SCE_KERNEL_ERROR_UNKNOWN_LW_MUTEX_ID,
          "an unknown lw mutex was accepted");

    CHECK(sceKernelUnlockLwMutex(&lw_mutex, 2) == 0, "could not unlock the lw mutex");

    /* --- lightweight condition introspection --------------------------- */
    CHECK(sceKernelCreateLwCond(&lw_cond, "info-cond", 0, &lw_mutex, NULL) == 0,
          "could not create the lw cond");
    memset(&cond_info, 0, sizeof(cond_info));
    cond_info.size = sizeof(cond_info);
    CHECK(sceKernelGetLwCondInfo(&lw_cond, &cond_info) == 0, "GetLwCondInfo failed");
    CHECK(cond_info.lwcond_id > 0, "the lw cond UID was not reported");
    CHECK(strcmp(cond_info.name, "info-cond") == 0, "the lw cond name was not reported");
    CHECK(cond_info.lwmutex == &lw_mutex, "the associated lw mutex was not reported");

    {
        VitaLwCondInfo by_id;

        memset(&by_id, 0, sizeof(by_id));
        by_id.size = sizeof(by_id);
        CHECK(sceKernelGetLwCondInfoById(cond_info.lwcond_id, &by_id) == 0,
              "GetLwCondInfoById failed");
        CHECK(by_id.lwcond_id == cond_info.lwcond_id, "the by-id lookup returned another cond");
    }

    CHECK(sceKernelGetLwCondInfoById(0x7fff, &cond_info) == SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID,
          "an unknown lw cond was accepted");

    CHECK(sceKernelDeleteLwCond(&lw_cond) == 0, "could not delete the lw cond");
    CHECK(sceKernelDeleteLwMutex(&lw_mutex) == 0, "could not delete the lw mutex");

    /* --- message pipe vectors ------------------------------------------ */
    {
        const char payload[] = "v2n";
        char received[sizeof(payload)] = { 0 };
        TestVector send_vector = { (void *)payload, sizeof(payload) };
        TestVector receive_vector = { received, sizeof(received) };
        unsigned int result = 0;

        CHECK(_sceKernelSendMsgPipeVector(mpp, (const void *)&send_vector, 1, 0, &result, NULL) ==
                  0,
              "the underscore vector send failed");
        CHECK(result == sizeof(payload), "the underscore vector send reported the wrong length");

        result = 0;
        CHECK(_sceKernelReceiveMsgPipeVector(mpp, (const void *)&receive_vector, 1, 0, &result,
                                             NULL) == 0,
              "the underscore vector receive failed");
        CHECK(result == sizeof(payload), "the underscore vector receive reported the wrong length");
        CHECK(memcmp(received, payload, sizeof(payload)) == 0, "the vector payload was corrupted");

        result = 0;
        CHECK(_sceKernelTryReceiveMsgPipeVector(mpp, (const void *)&receive_vector, 1, 0, &result) ==
                  SCE_KERNEL_ERROR_MSG_PIPE_EMPTY,
              "a try-receive on an empty pipe succeeded");
    }

    CHECK(sceKernelDeleteMsgPipe(mpp) == 0, "could not delete the message pipe");
    CHECK(sceKernelDeleteCallback(callback) == 0, "could not delete the callback");
    CHECK(sceKernelDeleteEventFlag(evf) == 0, "could not delete the event flag");
    CHECK(sceKernelDeleteSema(sema) == 0, "could not delete the semaphore");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the classification thread");

    /* --- CreateThreadForUser and CPU mask ------------------------------ */
    memset(&options, 0, sizeof(options));
    options.size = sizeof(options);
    options.stack_size = 64 * 1024;
    options.cpu_affinity_mask = 0x00070000;
    thid = sceKernelCreateThreadForUser("for-user", idle_entry, 100, &options);
    CHECK(thid > 0, "CreateThreadForUser failed");
    memset(&thread_info, 0, sizeof(thread_info));
    thread_info.size = sizeof(thread_info);
    CHECK(sceKernelGetThreadInfo(thid, &thread_info) == 0, "GetThreadInfo failed");
    CHECK(strcmp(thread_info.name, "for-user") == 0, "CreateThreadForUser lost the name");
    CHECK(sceKernelStartThread(thid, 0, NULL) == 0, "could not start the user thread");
    CHECK(sceKernelWaitThreadEnd_089(thid, NULL, NULL) == 0, "WaitThreadEnd_089 failed");
    CHECK(sceKernelDeleteThread(thid) == 0, "could not delete the user thread");

    options.cpu_affinity_mask = 1;
    CHECK(sceKernelCreateThreadForUser("bad-affinity", idle_entry, 100, &options) ==
              SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK,
          "an invalid CPU affinity mask was accepted");
    CHECK(sceKernelCreateThreadForUser("null-options", idle_entry, 100, NULL) ==
              SCE_KERNEL_ERROR_ILLEGAL_ADDR,
          "a null options pointer was accepted");

    CHECK(sceKernelChangeActiveCpuMask(0x00010000) == 0x00070000,
          "ChangeActiveCpuMask did not return the previous mask");
    CHECK(sceKernelChangeActiveCpuMask(0x8) == SCE_KERNEL_ERROR_ILLEGAL_CPU_AFFINITY_MASK,
          "an invalid active CPU mask was accepted");
    CHECK(sceKernelChangeActiveCpuMask(0x00070000) == 0x00010000,
          "ChangeActiveCpuMask did not preserve the mask");

    SceKernelSync_finish();
    SceKernelThreadMgr_finish();
    ueventDestroy(&g_exit_event);

    puts("threadmgr_smoke: ok");
    return 0;
}
