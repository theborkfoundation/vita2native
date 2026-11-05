/*
 * SceThreadmgr synchronization primitives: semaphores, event flags and
 * lightweight mutexes.
 *
 * These are exported under the same library names as the thread functions in
 * SceKernelThreadMgr.c; the linker merges both files' export tables.
 *
 * The user-visible entry points live in SceLibKernel, the rest in SceThreadmgr,
 * matching the firmware. Waiting uses host condition variables, so a blocked
 * guest thread really blocks and costs nothing.
 */

#include <errno.h>
#include <m-dict.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <psp2/kernel/threadmgr/semaphore.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"
#include "util.h"

#define SCE_KERNEL_ERROR_EVF_MULTI 0x800280E4

/* ------------------------------------------------------------- semaphores */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    int count;
    int init_count;
    int max_count;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaSemaInfo;

DICT_DEF2(vita_sema_dict, SceUID, M_DEFAULT_OPLIST, VitaSemaInfo *, M_POD_OPLIST)
static vita_sema_dict_t g_semas;
static RwLock g_semas_lock;

static VitaSemaInfo *sema_lookup(SceUID uid)
{
    VitaSemaInfo *sema;

    rwlockReadLock(&g_semas_lock);
    sema = *vita_sema_dict_get(g_semas, uid);
    rwlockReadUnlock(&g_semas_lock);

    return sema;
}

static void deadline_from_us(struct timespec *ts, SceUInt microseconds)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += (time_t)(microseconds / 1000000u);
    ts->tv_nsec += (long)(microseconds % 1000000u) * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

/* Returns 0, or a negative SCE error. `timeout` is in microseconds. */
static int sema_wait(VitaSemaInfo *sema, int need, const SceUInt *timeout)
{
    int result = 0;

    if (need <= 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    while (sema->count < need) {
        sema->waiting++;

        if (timeout) {
            struct timespec ts;

            deadline_from_us(&ts, *timeout);
            result = pthread_cond_timedwait(&sema->cond, &sema->mutex, &ts) == ETIMEDOUT
                         ? SCE_KERNEL_ERROR_WAIT_TIMEOUT
                         : 0;
        } else {
            pthread_cond_wait(&sema->cond, &sema->mutex);
            result = 0;
        }

        sema->waiting--;
        if (result != 0)
            break;
    }

    if (result == 0)
        sema->count -= need;

    pthread_mutex_unlock(&sema->mutex);
    return result;
}

EXPORT(SceLibKernel, 0x1BD67366, SceUID, sceKernelCreateSema, const char *name, SceUInt attr,
       int init_count, int max_count, SceKernelSemaOptParam *option)
{
    VitaSemaInfo *sema;

    (void)option;

    if (init_count < 0 || max_count <= 0 || init_count > max_count)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    sema = calloc(1, sizeof(*sema));
    if (!sema)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    sema->uid = SceSysmem_get_next_uid();
    sema->attr = attr;
    sema->count = init_count;
    sema->init_count = init_count;
    sema->max_count = max_count;
    if (name)
        strncpy(sema->name, name, sizeof(sema->name) - 1);

    pthread_mutex_init(&sema->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&sema->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_semas_lock);
    vita_sema_dict_set_at(g_semas, sema->uid, sema);
    rwlockWriteUnlock(&g_semas_lock);

    return sema->uid;
}

EXPORT(SceThreadmgr, 0xDB32948A, int, sceKernelDeleteSema, SceUID uid)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;

    rwlockWriteLock(&g_semas_lock);
    vita_sema_dict_erase(g_semas, uid);
    rwlockWriteUnlock(&g_semas_lock);

    pthread_mutex_lock(&sema->mutex);
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    pthread_cond_destroy(&sema->cond);
    pthread_mutex_destroy(&sema->mutex);
    free(sema);

    return 0;
}

EXPORT(SceThreadmgr, 0xE6B761D1, int, sceKernelSignalSema, SceUID uid, int signal)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (signal < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (sema->count + signal > sema->max_count) {
        pthread_mutex_unlock(&sema->mutex);
        return SCE_KERNEL_ERROR_SEMA_OVF;
    }
    sema->count += signal;
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x0C7B834B, int, sceKernelWaitSema, SceUID uid, int signal,
       SceUInt *timeout)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;

    return sema_wait(sema, signal, timeout);
}

EXPORT(SceLibKernel, 0x174692B4, int, sceKernelWaitSemaCB, SceUID uid, int signal,
       SceUInt *timeout)
{
    return sceKernelWaitSema(uid, signal, timeout);
}

EXPORT(SceThreadmgr, 0x866EF048, int, sceKernelPollSema, SceUID uid, int signal)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (signal <= 0)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (sema->count < signal) {
        pthread_mutex_unlock(&sema->mutex);
        return SCE_KERNEL_ERROR_SEMA_ZERO;
    }
    sema->count -= signal;
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x66D6BF05, int, sceKernelCancelSema, SceUID uid, int set_count,
       int *num_wait_threads)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (set_count < 0 || set_count > sema->max_count)
        return SCE_KERNEL_ERROR_ILLEGAL_COUNT;

    pthread_mutex_lock(&sema->mutex);
    if (num_wait_threads)
        *num_wait_threads = sema->waiting;
    sema->count = set_count;
    pthread_cond_broadcast(&sema->cond);
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x595D3FA6, int, sceKernelGetSemaInfo, SceUID uid, SceKernelSemaInfo *info)
{
    VitaSemaInfo *sema = sema_lookup(uid);

    if (!sema)
        return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->semaId = sema->uid;
    memcpy(info->name, sema->name, sizeof(info->name));
    info->attr = sema->attr;
    info->initCount = sema->init_count;
    info->maxCount = sema->max_count;

    pthread_mutex_lock(&sema->mutex);
    info->currentCount = sema->count;
    info->numWaitThreads = sema->waiting;
    pthread_mutex_unlock(&sema->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xCBE235C7, SceUID, sceKernelOpenSema, const char *name)
{
    (void)name;

    /* Opening by name is not supported; every object is per-process here. */
    return SCE_KERNEL_ERROR_UNKNOWN_SEMA_ID;
}

EXPORT(SceThreadmgr, 0xA2D81F9E, int, sceKernelCloseSema, SceUID uid)
{
    (void)uid;

    return 0;
}

/* ------------------------------------------------------------ event flags */

typedef struct {
    SceUID uid;
    char name[32];
    SceUInt attr;
    uint32_t init_pattern;
    uint32_t pattern;
    int waiting;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} VitaEventFlagInfo;

DICT_DEF2(vita_event_flag_dict, SceUID, M_DEFAULT_OPLIST, VitaEventFlagInfo *, M_POD_OPLIST)
static vita_event_flag_dict_t g_event_flags;
static RwLock g_event_flags_lock;

static VitaEventFlagInfo *event_flag_lookup(SceUID uid)
{
    VitaEventFlagInfo *evf;

    rwlockReadLock(&g_event_flags_lock);
    evf = *vita_event_flag_dict_get(g_event_flags, uid);
    rwlockReadUnlock(&g_event_flags_lock);

    return evf;
}

static bool event_flag_matches(const VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait)
{
    if (wait & SCE_EVENT_WAITOR)
        return (evf->pattern & bits) != 0;

    return (evf->pattern & bits) == bits;
}

static void event_flag_consume(VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait)
{
    if (wait & SCE_EVENT_WAITCLEAR)
        evf->pattern = 0;
    else if (wait & SCE_EVENT_WAITCLEAR_PAT)
        evf->pattern &= ~bits;
}

static int event_flag_wait(VitaEventFlagInfo *evf, uint32_t bits, SceUInt wait,
                           uint32_t *out_bits, const SceUInt *timeout, bool blocking)
{
    int result = 0;

    pthread_mutex_lock(&evf->mutex);

    for (;;) {
        if (event_flag_matches(evf, bits, wait)) {
            if (out_bits)
                *out_bits = evf->pattern;
            event_flag_consume(evf, bits, wait);
            break;
        }

        if (!blocking) {
            if (out_bits)
                *out_bits = evf->pattern;
            result = SCE_KERNEL_ERROR_EVF_COND;
            break;
        }

        evf->waiting++;
        if (timeout) {
            struct timespec ts;

            deadline_from_us(&ts, *timeout);
            if (pthread_cond_timedwait(&evf->cond, &evf->mutex, &ts) == ETIMEDOUT)
                result = SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        } else {
            pthread_cond_wait(&evf->cond, &evf->mutex);
        }
        evf->waiting--;

        if (result != 0) {
            if (out_bits)
                *out_bits = evf->pattern;
            break;
        }
    }

    pthread_mutex_unlock(&evf->mutex);
    return result;
}

EXPORT(SceLibKernel, 0x8516D040, SceUID, sceKernelCreateEventFlag, const char *name, int attr,
       int init_pattern, SceKernelEventFlagOptParam *option)
{
    VitaEventFlagInfo *evf;

    (void)option;

    evf = calloc(1, sizeof(*evf));
    if (!evf)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    evf->uid = SceSysmem_get_next_uid();
    evf->attr = (SceUInt)attr;
    evf->init_pattern = (uint32_t)init_pattern;
    evf->pattern = (uint32_t)init_pattern;
    if (name)
        strncpy(evf->name, name, sizeof(evf->name) - 1);

    pthread_mutex_init(&evf->mutex, NULL);
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&evf->cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    rwlockWriteLock(&g_event_flags_lock);
    vita_event_flag_dict_set_at(g_event_flags, evf->uid, evf);
    rwlockWriteUnlock(&g_event_flags_lock);

    return evf->uid;
}

EXPORT(SceThreadmgr, 0x5840162C, int, sceKernelDeleteEventFlag, SceUID uid)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    rwlockWriteLock(&g_event_flags_lock);
    vita_event_flag_dict_erase(g_event_flags, uid);
    rwlockWriteUnlock(&g_event_flags_lock);

    pthread_mutex_lock(&evf->mutex);
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    pthread_cond_destroy(&evf->cond);
    pthread_mutex_destroy(&evf->mutex);
    free(evf);

    return 0;
}

EXPORT(SceThreadmgr, 0xEC94DFF7, int, sceKernelSetEventFlag, SceUID uid, unsigned int bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    evf->pattern |= bits;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0x4CB87CA7, int, sceKernelClearEventFlag, SceUID uid, unsigned int bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    evf->pattern &= ~bits;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x83C0E2AF, int, sceKernelWaitEventFlag, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    return event_flag_wait(evf, bits, wait, out_bits, timeout, true);
}

EXPORT(SceLibKernel, 0xE737B1DF, int, sceKernelWaitEventFlagCB, SceUID uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits, SceUInt *timeout)
{
    return sceKernelWaitEventFlag(uid, bits, wait, out_bits, timeout);
}

EXPORT(SceLibKernel, 0x1FBB0FE1, int, sceKernelPollEventFlag, int uid, unsigned int bits,
       unsigned int wait, unsigned int *out_bits)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    return event_flag_wait(evf, bits, wait, out_bits, NULL, false);
}

EXPORT(SceLibKernel, 0x8BA4C0C1, int, sceKernelGetEventFlagInfo, SceUID uid,
       SceKernelEventFlagInfo *info)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->evfId = evf->uid;
    memcpy(info->name, evf->name, sizeof(info->name));
    info->attr = evf->attr;
    info->initPattern = evf->init_pattern;

    pthread_mutex_lock(&evf->mutex);
    info->currentPattern = evf->pattern;
    info->numWaitThreads = evf->waiting;
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0x2A12D9B7, int, sceKernelCancelEventFlag, SceUID uid, unsigned int set_pattern,
       int *num_wait_threads)
{
    VitaEventFlagInfo *evf = event_flag_lookup(uid);

    if (!evf)
        return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;

    pthread_mutex_lock(&evf->mutex);
    if (num_wait_threads)
        *num_wait_threads = evf->waiting;
    evf->pattern = set_pattern;
    pthread_cond_broadcast(&evf->cond);
    pthread_mutex_unlock(&evf->mutex);

    return 0;
}

EXPORT(SceThreadmgr, 0xBC19F8A1, SceUID, sceKernelOpenEventFlag, const char *name)
{
    (void)name;

    return SCE_KERNEL_ERROR_UNKNOWN_EVF_ID;
}

EXPORT(SceThreadmgr, 0x9A68F547, int, sceKernelCloseEventFlag, SceUID uid)
{
    (void)uid;

    return 0;
}

/* ------------------------------------------------------------ lw mutexes */

/*
 * Lightweight mutexes live in guest-provided work structures, so there is no
 * UID. A host mutex is allocated on create and its address is kept in the
 * work area's first word.
 */
typedef struct {
    pthread_mutex_t mutex;
} VitaLwMutex;

static VitaLwMutex *lw_mutex_from_work(SceKernelLwMutexWork *work)
{
    void *pointer;

    memcpy(&pointer, &work->data[0], sizeof(pointer));
    return pointer;
}

EXPORT(SceLibKernel, 0xDA6EC8EF, int, sceKernelCreateLwMutex, SceKernelLwMutexWork *work,
       const char *name, unsigned int attr, int init_count, const SceKernelLwMutexOptParam *option)
{
    VitaLwMutex *lw;
    pthread_mutexattr_t mutex_attr;

    (void)name;
    (void)option;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = calloc(1, sizeof(*lw));
    if (!lw)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    pthread_mutexattr_init(&mutex_attr);
    if (attr & SCE_KERNEL_MUTEX_ATTR_RECURSIVE)
        pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lw->mutex, &mutex_attr);
    pthread_mutexattr_destroy(&mutex_attr);

    if (init_count > 0)
        for (int i = 0; i < init_count; i++)
            pthread_mutex_lock(&lw->mutex);

    memset(work, 0, sizeof(*work));
    memcpy(&work->data[0], &lw, sizeof(lw));

    return 0;
}

EXPORT(SceLibKernel, 0x244E76D2, int, sceKernelDeleteLwMutex, SceKernelLwMutexWork *work)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = lw_mutex_from_work(work);
    if (!lw)
        return 0;

    pthread_mutex_destroy(&lw->mutex);
    free(lw);
    memset(work, 0, sizeof(*work));

    return 0;
}

EXPORT(SceLibKernel, 0x46E7BE7B, int, sceKernelLockLwMutex, SceKernelLwMutexWork *work,
       int count, SceUInt *timeout)
{
    VitaLwMutex *lw;

    (void)timeout;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    for (int i = 0; i < count; i++)
        pthread_mutex_lock(&lw->mutex);

    return 0;
}

EXPORT(SceLibKernel, 0xA6A2C915, int, sceKernelTryLockLwMutex, SceKernelLwMutexWork *work,
       int count)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    for (int i = 0; i < count; i++) {
        if (pthread_mutex_trylock(&lw->mutex) != 0)
            return SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN;
    }

    return 0;
}

EXPORT(SceLibKernel, 0x91FA6614, int, sceKernelUnlockLwMutex, SceKernelLwMutexWork *work,
       int count)
{
    VitaLwMutex *lw;

    if (!work)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    lw = lw_mutex_from_work(work);
    if (!lw)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    for (int i = 0; i < count; i++)
        pthread_mutex_unlock(&lw->mutex);

    return 0;
}

int SceKernelSync_init(void)
{
    vita_sema_dict_init(g_semas);
    vita_event_flag_dict_init(g_event_flags);
    rwlockInit(&g_semas_lock);
    rwlockInit(&g_event_flags_lock);
    return 0;
}

int SceKernelSync_finish(void)
{
    vita_sema_dict_clear(g_semas);
    vita_event_flag_dict_clear(g_event_flags);
    rwlockDestroy(&g_semas_lock);
    rwlockDestroy(&g_event_flags_lock);
    return 0;
}
