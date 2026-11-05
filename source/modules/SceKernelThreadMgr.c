#include <m-dict.h>
#include <errno.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/msgpipe.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "util.h"

#include "modules/SceKernelThreadMgr.h"
#include "modules/SceLibKernel.h"
#include "modules/SceSysmem.h"

#define SCE_KERNEL_HIGHEST_PRIORITY_USER               64
#define SCE_KERNEL_LOWEST_PRIORITY_USER                191
#define SCE_KERNEL_DEFAULT_PRIORITY                    ((SceInt32)0x10000100)
#define SCE_KERNEL_DEFAULT_PRIORITY_GAME_APP           160
#define SCE_KERNEL_DEFAULT_PRIORITY_USER               SCE_KERNEL_DEFAULT_PRIORITY
#define SCE_KERNEL_THREAD_STACK_SIZE_DEFAULT_USER_MAIN (256 * 1024)
#define SCE_KERNEL_TLS_SIZE                            0x800

#define HOST_HIGHEST_PRIORITY 28
#define HOST_LOWEST_PRIORITY  59

static s32 g_vita_thread_info_tls_slot_id;
static SceUID g_main_thread_uid;

DICT_DEF2(vita_thread_info_dict, SceUID, M_DEFAULT_OPLIST, VitaThreadInfo *, M_POD_OPLIST)
static vita_thread_info_dict_t g_vita_thread_infos;
static RwLock g_vita_thread_infos_lock;

typedef struct {
    SceUID uid;
    char name[32];
    pthread_mutex_t mutex;
} VitaMutexInfo;

typedef struct {
    SceUID uid;
    char name[32];
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t used;
    uint32_t head;
    uint32_t tail;
    Mutex lock;
    CondVar not_empty;
    CondVar not_full;
} VitaMsgPipeInfo;

DICT_DEF2(vita_mutex_info_dict, SceUID, M_DEFAULT_OPLIST, VitaMutexInfo *, M_POD_OPLIST)
DICT_DEF2(vita_msgpipe_info_dict, SceUID, M_DEFAULT_OPLIST, VitaMsgPipeInfo *, M_POD_OPLIST)
static vita_mutex_info_dict_t g_vita_mutex_infos;
static vita_msgpipe_info_dict_t g_vita_msgpipe_infos;
static RwLock g_vita_sync_objects_lock;

static inline int vita_priority_to_host_priority(int priority)
{
    if ((priority & SCE_KERNEL_DEFAULT_PRIORITY) == SCE_KERNEL_DEFAULT_PRIORITY)
        priority = SCE_KERNEL_DEFAULT_PRIORITY_GAME_APP + (priority & ~SCE_KERNEL_DEFAULT_PRIORITY);

    if ((priority < SCE_KERNEL_HIGHEST_PRIORITY_USER) ||
        (priority > SCE_KERNEL_LOWEST_PRIORITY_USER))
        return SCE_KERNEL_ERROR_ILLEGAL_PRIORITY;

    return HOST_HIGHEST_PRIORITY +
           ((priority - SCE_KERNEL_HIGHEST_PRIORITY_USER) *
            (HOST_LOWEST_PRIORITY - HOST_HIGHEST_PRIORITY)) /
               (SCE_KERNEL_LOWEST_PRIORITY_USER - SCE_KERNEL_HIGHEST_PRIORITY_USER);
}

static VitaThreadInfo *get_thread_info_for_uid(SceUID thid)
{
    VitaThreadInfo *ti;

    rwlockReadLock(&g_vita_thread_infos_lock);
    ti = *vita_thread_info_dict_get(g_vita_thread_infos, thid);
    rwlockReadUnlock(&g_vita_thread_infos_lock);

    return ti;
}

static void NORETURN thread_entry_wrapper(void *arg)
{
    VitaThreadInfo *ti = arg;
    int ret;

    threadTlsSet(g_vita_thread_info_tls_slot_id, ti);

    ret = ti->entry(ti->arglen, ti->argp);

    LOG("Thread 0x%x returned with: 0x%x", ti->uid, ret);

    /*
     * Returning from the Vita main thread is a normal way for simple
     * homebrew to finish. Treat it like sceKernelExitProcess() so the host
     * main loop does not wait forever.
     */
    if (g_main_thread_uid != 0 && ti->uid == g_main_thread_uid)
        ueventSignal(SceLibKernel_get_process_exit_uevent());

    threadExit();
}

static SceUID create_thread(const char *name, SceKernelThreadEntry entry, int initPriority,
                            SceSize stackSize)
{
    VitaThreadInfo *ti;
    Result res;
    int priority = vita_priority_to_host_priority(initPriority);

    if (priority < 0)
        return priority;

    ti = malloc(sizeof(*ti));
    if (!ti) {
        LOG("Could not allocate thread info for thread \"%s\"", name);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    memset(ti, 0, sizeof(*ti));
    ti->uid = SceSysmem_get_next_uid();
    strncpy(ti->name, name, sizeof(ti->name) - 1);
    ti->entry = entry;
    ti->init_priority = initPriority;
    ti->current_priority = initPriority;
    ti->stack_size = stackSize;
    ti->vita_tls = malloc(SCE_KERNEL_TLS_SIZE);

    res = threadCreate(&ti->thread, thread_entry_wrapper, ti, NULL, stackSize, priority, -2);
    if (R_FAILED(res)) {
        LOG("Error creating thread: 0x%" PRIx32, res);
        free(ti->vita_tls);
        free(ti);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    LOG("Created thread \"%s\", thid: 0x%x", name, ti->uid);

    rwlockWriteLock(&g_vita_thread_infos_lock);
    vita_thread_info_dict_set_at(g_vita_thread_infos, ti->uid, ti);
    rwlockWriteUnlock(&g_vita_thread_infos_lock);

    return ti->uid;
}

static int start_thread(SceUID thid, SceSize arglen, void *argp)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    if (arglen && argp) {
        ti->argp = malloc(arglen);
        if (!ti->argp)
            return SCE_KERNEL_ERROR_NO_MEMORY;

        memcpy(ti->argp, argp, arglen);
        ti->arglen = arglen;
    }

    res = threadStart(&ti->thread);
    if (R_FAILED(res)) {
        LOG("Error starting thread 0x%x: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    return 0;
}

EXPORT(SceLibKernel, 0xC5C11EE7, SceUID, sceKernelCreateThread, const char *name,
       SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr,
       int cpuAffinityMask, const SceKernelThreadOptParam *option)
{
    return create_thread(name, entry, initPriority, stackSize);
}

EXPORT(SceThreadmgr, 0x1BBDE3D9, int, sceKernelDeleteThread, SceUID thid)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    res = threadClose(&ti->thread);
    if (R_FAILED(res)) {
        LOG("Error closing thread 0x%x exit: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    free(ti->argp);
    free(ti->vita_tls);
    rwlockWriteLock(&g_vita_thread_infos_lock);
    vita_thread_info_dict_erase(g_vita_thread_infos, thid);
    rwlockWriteUnlock(&g_vita_thread_infos_lock);
    free(ti);
    LOG("Deleted thread 0x%x", thid);

    return 0;
}

EXPORT(SceLibKernel, 0xF08DE149, int, sceKernelStartThread, SceUID thid, SceSize arglen, void *argp)
{
    return start_thread(thid, arglen, argp);
}

EXPORT(SceThreadmgrCoredumpTime, 0x0C8A38E1, int NORETURN, sceKernelExitThread, int status)
{
    VitaThreadInfo *ti = SceKernelThreadMgr_get_thread_info();

    ti->return_status = status;
    threadExit();
}

EXPORT(SceThreadmgr, 0x1D17DECF, int NORETURN, sceKernelExitDeleteThread, int status)
{
    sceKernelExitThread(status);
    // TODO: Delete
}

EXPORT(SceLibKernel, 0xDDB395A9, int, sceKernelWaitThreadEnd, SceUID thid, int *stat,
       SceUInt *timeout)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);
    uint64_t ns;
    Result res;

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;

    if (!timeout)
        ns = UINT64_MAX;
    else
        ns = *timeout * 1000ull;

    res = waitSingle(waiterForThread(&ti->thread), ns);
    if (R_FAILED(res)) {
        LOG("Error waiting for thread 0x%x exit: 0x%" PRIx32, thid, res);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    if (stat)
        *stat = ti->return_status;

    return 0;
}

int SceKernelThreadMgr_main_entry(SceKernelThreadEntry entry, int args, void *argp)
{
    SceUID thid;
    UEvent *process_exit_event;
    int ret;
    Result res;

    thid = create_thread("<main>", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER,
                         SCE_KERNEL_THREAD_STACK_SIZE_DEFAULT_USER_MAIN);
    if (thid < 0)
        return thid;

    g_main_thread_uid = thid;

    ret = start_thread(thid, args, argp);
    if (ret < 0) {
        sceKernelDeleteThread(thid);
        return SCE_KERNEL_ERROR_THREAD_ERROR;
    }

    process_exit_event = SceLibKernel_get_process_exit_uevent();

    res = waitSingle(waiterForUEvent(process_exit_event), -1);
    if (R_FAILED(res)) {
        LOG("Error waiting for the process to finish: 0x%" PRIx32, res);
        return -1;
    }

    // TODO: Also wait for all threads to finish?
    sceKernelWaitThreadEnd(thid, NULL, NULL);
    if (thid == g_main_thread_uid)
        g_main_thread_uid = 0;
    sceKernelDeleteThread(thid);

    return 0;
}

EXPORT(SceThreadmgr, 0x4B675D05, int, sceKernelDelayThread, SceUInt delay)
{
    host_sleep_ns((s64)delay * 1000);
    return 0;
}

EXPORT(SceThreadmgr, 0xBACA6891, void *, sceKernelGetThreadTLSAddr, SceUID thid, int key)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);

    if (!ti)
        return NULL;

    if (key >= 0 && key <= 0x100)
        return &ti->vita_tls[key];

    return NULL;
}

/* Serialization objects. */

static VitaMutexInfo *get_mutex_info(SceUID uid)
{
    VitaMutexInfo *info;

    rwlockReadLock(&g_vita_sync_objects_lock);
    info = *vita_mutex_info_dict_get(g_vita_mutex_infos, uid);
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return info;
}

static VitaMsgPipeInfo *get_msgpipe_info(SceUID uid)
{
    VitaMsgPipeInfo *info;

    rwlockReadLock(&g_vita_sync_objects_lock);
    info = *vita_msgpipe_info_dict_get(g_vita_msgpipe_infos, uid);
    rwlockReadUnlock(&g_vita_sync_objects_lock);

    return info;
}

EXPORT(SceLibKernel, 0xED53334A, SceUID, sceKernelCreateMutex, const char *name, SceUInt attr,
       int initCount, SceKernelMutexOptParam *option)
{
    pthread_mutexattr_t mutex_attr;
    VitaMutexInfo *info = calloc(1, sizeof(*info));

    (void)attr;
    (void)option;

    if (!info)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    info->uid = SceSysmem_get_next_uid();
    if (name)
        strncpy(info->name, name, sizeof(info->name) - 1);

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&info->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    for (int i = 0; i < initCount; i++)
        pthread_mutex_lock(&info->mutex);

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_mutex_info_dict_set_at(g_vita_mutex_infos, info->uid, info);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    return info->uid;
}

EXPORT(SceThreadmgr, 0xCB78710D, int, sceKernelDeleteMutex, SceUID mutexid)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_mutex_info_dict_erase(g_vita_mutex_infos, mutexid);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    pthread_mutex_destroy(&info->mutex);
    free(info);
    return 0;
}

EXPORT(SceLibKernel, 0x1D8D7945, int, sceKernelLockMutex, SceUID mutexid, int lockCount,
       unsigned int *timeout)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (lockCount < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    for (int i = 0; i < lockCount; i++)
        pthread_mutex_lock(&info->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x2BDAA524, int, sceKernelLockMutexCB, SceUID mutexid, int lockCount,
       unsigned int *timeout)
{
    return sceKernelLockMutex(mutexid, lockCount, timeout);
}

EXPORT(SceLibKernel, 0x72FC1F54, int, sceKernelTryLockMutex, SceUID mutexid, int lockCount)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    for (int i = 0; i < lockCount; i++) {
        if (pthread_mutex_trylock(&info->mutex) != 0)
            return SCE_KERNEL_ERROR_ILLEGAL_COUNT;
    }

    return 0;
}

EXPORT(SceThreadmgr, 0x1A372EC8, int, sceKernelUnlockMutex, SceUID mutexid, int unlockCount)
{
    VitaMutexInfo *info = get_mutex_info(mutexid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;
    if (unlockCount < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    for (int i = 0; i < unlockCount; i++)
        pthread_mutex_unlock(&info->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x0A10C1C8, SceUID, sceKernelCreateMsgPipe, const char *name, int type,
       int attr, unsigned int bufSize, void *option)
{
    VitaMsgPipeInfo *info = calloc(1, sizeof(*info));

    (void)type;
    (void)attr;
    (void)option;

    if (!info)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    info->uid = SceSysmem_get_next_uid();
    if (name)
        strncpy(info->name, name, sizeof(info->name) - 1);
    info->buffer = malloc(bufSize ? bufSize : 1);
    info->capacity = bufSize;
    if (!info->buffer) {
        free(info);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    mutexInit(&info->lock);
    condvarInit(&info->not_empty);
    condvarInit(&info->not_full);

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_msgpipe_info_dict_set_at(g_vita_msgpipe_infos, info->uid, info);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    return info->uid;
}

EXPORT(SceThreadmgr, 0xE78BCCF7, int, sceKernelDeleteMsgPipe, SceUID uid)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    rwlockWriteLock(&g_vita_sync_objects_lock);
    vita_msgpipe_info_dict_erase(g_vita_msgpipe_infos, uid);
    rwlockWriteUnlock(&g_vita_sync_objects_lock);

    mutexDestroy(&info->lock);
    condvarDestroy(&info->not_empty);
    condvarDestroy(&info->not_full);
    free(info->buffer);
    free(info);
    return 0;
}

static int msgpipe_send(VitaMsgPipeInfo *info, const void *message, unsigned int size,
                        unsigned int *result)
{
    const uint8_t *src = message;

    if (size > info->capacity)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    mutexLock(&info->lock);
    while (info->capacity - info->used < size)
        condvarWait(&info->not_full, &info->lock);

    for (unsigned int i = 0; i < size; i++) {
        info->buffer[(info->head + i) % info->capacity] = src[i];
    }
    info->head = (info->head + size) % info->capacity;
    info->used += size;

    if (result)
        *result = size;

    condvarWakeAll(&info->not_empty);
    mutexUnlock(&info->lock);
    return 0;
}

static int msgpipe_receive(VitaMsgPipeInfo *info, void *message, unsigned int size,
                           unsigned int *result)
{
    uint8_t *dst = message;
    unsigned int count;

    mutexLock(&info->lock);
    while (!info->used)
        condvarWait(&info->not_empty, &info->lock);

    count = info->used < size ? info->used : size;
    for (unsigned int i = 0; i < count; i++)
        dst[i] = info->buffer[(info->tail + i) % info->capacity];

    info->tail = (info->tail + count) % info->capacity;
    info->used -= count;

    if (result)
        *result = count;

    condvarWakeAll(&info->not_full);
    mutexUnlock(&info->lock);
    return 0;
}

EXPORT(SceLibKernel, 0x0CA71EA2, int, sceKernelSendMsgPipe, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_send(info, message, size, unk2);
}

EXPORT(SceLibKernel, 0x4E81DD5C, int, sceKernelReceiveMsgPipe, SceUID uid, void *message,
       unsigned int size, int unk1, void *unk2, unsigned int *timeout)
{
    VitaMsgPipeInfo *info = get_msgpipe_info(uid);

    (void)unk1;
    (void)timeout;
    if (!info)
        return SCE_KERNEL_ERROR_UNKNOWN_MSG_PIPE_ID;

    return msgpipe_receive(info, message, size, unk2);
}

EXPORT(SceLibKernel, 0x6E903AB2, int, sceIoChstatByFd, SceUID fd, const SceIoStat *stat,
       unsigned int bits)
{
    (void)fd;
    (void)stat;
    (void)bits;
    return 0;
}

EXPORT(SceLibKernel, 0x04B30CB2, int, sceIoDevctl, const char *dev, unsigned int cmd,
       void *indata, int inlen, void *outdata, int outlen)
{
    (void)dev;
    (void)cmd;
    (void)indata;
    (void)inlen;

    if (outdata && outlen > 0)
        memset(outdata, 0, (size_t)outlen);

    return 0;
}

EXPORT(SceLibKernel, 0x8D9C5461, int, sceKernelGetThreadInfo, SceUID thid,
       SceKernelThreadInfo *info)
{
    VitaThreadInfo *ti = get_thread_info_for_uid(thid);

    if (!ti)
        return SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->processId = 1;
    strncpy(info->name, ti->name, sizeof(info->name) - 1);
    info->attr = ti->attr;
    info->status = 0x2; /* running */
    info->entry = ti->entry;
    info->stackSize = (SceInt32)ti->stack_size;
    info->initPriority = ti->init_priority;
    info->currentPriority = ti->current_priority;

    return 0;
}

EXPORT(SceThreadmgr, 0x9DCB4B7A, SceUID, sceKernelGetProcessId, void)
{
    return 1;
}

EXPORT(SceLibKernel, 0xE9F973B1, SceUInt32, sceKernelGetProcessTimeLow, void)
{
    return (SceUInt32)(host_monotonic_ns() / 1000ull);
}

EXPORT(SceProcessmgr, 0x4B879059, int, sceKernelLibcGettimeofday, SceKernelTimeval *tv,
       SceKernelTimezone *tz)
{
    struct timeval host_time;

    if (!tv)
        return -1;

    gettimeofday(&host_time, NULL);
    tv->sec = (SceInt32)host_time.tv_sec;
    tv->usec = (SceInt32)host_time.tv_usec;

    if (tz)
        tz->value = 0;

    return 0;
}

/* SceLibKernel declared in SceLibKernel.c */
/* --------------------------------------------------- condition variables */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    SceUID mutex_id;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaCondInfo;

DICT_DEF2(vita_cond_dict, SceUID, M_DEFAULT_OPLIST, VitaCondInfo *, M_POD_OPLIST)
static vita_cond_dict_t g_conds;
static RwLock g_conds_lock;

static void sync_deadline_from_us(struct timespec *ts, unsigned int microseconds)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(microseconds / 1000000u);
    ts->tv_nsec += (long)(microseconds % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static VitaCondInfo *cond_lookup(SceUID uid)
{
    VitaCondInfo *cond;

    rwlockReadLock(&g_conds_lock);
    cond = *vita_cond_dict_get(g_conds, uid);
    rwlockReadUnlock(&g_conds_lock);

    return cond;
}

EXPORT(SceLibKernel, 0x50572FDA, SceUID, sceKernelCreateCond, const char *name, SceUInt attr,
       SceUID mutex_id, const SceKernelCondOptParam *option)
{
    VitaCondInfo *cond;

    (void)option;

    cond = calloc(1, sizeof(*cond));
    if (!cond)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    cond->uid = SceSysmem_get_next_uid();
    cond->attr = attr;
    cond->mutex_id = mutex_id;
    if (name)
        strncpy(cond->name, name, sizeof(cond->name) - 1);

    pthread_mutex_init(&cond->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cond->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_conds_lock);
    vita_cond_dict_set_at(g_conds, cond->uid, cond);
    rwlockWriteUnlock(&g_conds_lock);

    return cond->uid;
}

EXPORT(SceThreadmgr, 0x879E6EBD, int, sceKernelDeleteCond, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    rwlockWriteLock(&g_conds_lock);
    vita_cond_dict_erase(g_conds, uid);
    rwlockWriteUnlock(&g_conds_lock);

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_broadcast(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    pthread_cond_destroy(&cond->cond);
    pthread_mutex_destroy(&cond->mutex);
    free(cond);

    return 0;
}

static int cond_wait(VitaCondInfo *cond, unsigned int *timeout)
{
    int result = 0;

    /* The caller holds the associated mutex; release it while waiting. */
    if (sceKernelUnlockMutex(cond->mutex_id, 1) != 0)
        return SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID;

    pthread_mutex_lock(&cond->mutex);
    cond->waiting++;
    if (timeout) {
        struct timespec ts;

        sync_deadline_from_us(&ts, *timeout);
        if (pthread_cond_timedwait(&cond->cond, &cond->mutex, &ts) == ETIMEDOUT)
            result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
    } else {
        pthread_cond_wait(&cond->cond, &cond->mutex);
    }
    cond->waiting--;
    pthread_mutex_unlock(&cond->mutex);

    sceKernelLockMutex(cond->mutex_id, 1, NULL);

    return result;
}

EXPORT(SceLibKernel, 0xC88D44AD, int, sceKernelWaitCond, SceUID uid, unsigned int *timeout)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    return cond_wait(cond, timeout);
}

EXPORT(SceLibKernel, 0x4CE42CE2, int, sceKernelWaitCondCB, SceUID uid, unsigned int *timeout)
{
    return sceKernelWaitCond(uid, timeout);
}

EXPORT(SceThreadmgr, 0x6ED2E2DC, int, sceKernelSignalCond, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_signal(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xC2E7AC22, int, sceKernelSignalCondAll, SceUID uid)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;

    pthread_mutex_lock(&cond->mutex);
    pthread_cond_broadcast(&cond->cond);
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x1269F4EC, int, sceKernelSignalCondTo, SceUID uid, SceUID thread_id)
{
    (void)thread_id;

    return sceKernelSignalCond(uid);
}

EXPORT(SceLibKernel, 0x6864DCE2, int, sceKernelGetCondInfo, SceUID uid, SceKernelCondInfo *info)
{
    VitaCondInfo *cond = cond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->condId = cond->uid;
    memcpy(info->name, cond->name, sizeof(info->name));
    info->attr = cond->attr;
    info->mutexId = cond->mutex_id;

    pthread_mutex_lock(&cond->mutex);
    info->numWaitThreads = cond->waiting;
    pthread_mutex_unlock(&cond->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x76BDA02F, SceUID, sceKernelOpenCond, const char *name)
{
    (void)name;

    return SCE_KERNEL_ERROR_UNKNOWN_COND_ID;
}

EXPORT(SceThreadmgr, 0x15C690E0, int, sceKernelCloseCond, SceUID uid)
{
    (void)uid;

    return 0;
}


DECLARE_LIBRARY(SceThreadmgr, 0x859a24b1);
DECLARE_LIBRARY(SceThreadmgrCoredumpTime, 0x5E8D0E22);
DECLARE_LIBRARY(SceProcessmgr, 0x2dd91812);

int SceKernelThreadMgr_init(void)
{
    g_vita_thread_info_tls_slot_id = threadTlsAlloc(NULL);
    if (g_vita_thread_info_tls_slot_id < 0)
        return -1;

    vita_thread_info_dict_init(g_vita_thread_infos);
    rwlockInit(&g_vita_thread_infos_lock);
    vita_mutex_info_dict_init(g_vita_mutex_infos);
    vita_msgpipe_info_dict_init(g_vita_msgpipe_infos);
    vita_cond_dict_init(g_conds);
    rwlockInit(&g_conds_lock);
    rwlockInit(&g_vita_sync_objects_lock);

    return 0;
}

int SceKernelThreadMgr_finish(void)
{
    threadTlsFree(g_vita_thread_info_tls_slot_id);
    vita_thread_info_dict_clear(g_vita_thread_infos);
    vita_mutex_info_dict_clear(g_vita_mutex_infos);
    vita_msgpipe_info_dict_clear(g_vita_msgpipe_infos);
    vita_cond_dict_clear(g_conds);
    rwlockDestroy(&g_conds_lock);

    return 0;
}

VitaThreadInfo *SceKernelThreadMgr_get_thread_info(void)
{
    return threadTlsGet(g_vita_thread_info_tls_slot_id);
}
