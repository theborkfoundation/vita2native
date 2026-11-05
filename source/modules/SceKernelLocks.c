/*
 * Reader/writer locks and lightweight condition variables.
 *
 * RW locks are real pthread rwlocks behind a SceUID table. Lightweight
 * condition variables share the pthread mutex owned by the associated
 * sceKernelLwMutex work area, so sceKernelWaitLwCond really blocks and releases
 * the lock for the duration of the wait.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2common/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/lw_cond.h>
#include <psp2/kernel/threadmgr/rwlock.h>

#include "module.h"
#include "modules/SceKernelSync.h"
#include "modules/SceKernelThreadMgr.h"
#include "modules/SceSysmem.h"

#define VITA_RWLOCK_SLOTS 64
#define VITA_LWCOND_SLOTS 64

/* ------------------------------------------------------------------------- */
/* Reader/writer locks                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool used;
    SceUID uid;
    int refs;
    int lock_count;
    SceUID write_owner;
    uint32_t attr;
    char name[32];
    pthread_rwlock_t lock;
} VitaRWLock;

static VitaRWLock g_rwlocks[VITA_RWLOCK_SLOTS];
static pthread_mutex_t g_rwlock_table_lock = PTHREAD_MUTEX_INITIALIZER;

static VitaRWLock *rwlock_lookup(SceUID uid)
{
    pthread_mutex_lock(&g_rwlock_table_lock);
    for (int i = 0; i < VITA_RWLOCK_SLOTS; i++) {
        if (g_rwlocks[i].used && g_rwlocks[i].uid == uid) {
            pthread_mutex_unlock(&g_rwlock_table_lock);
            return &g_rwlocks[i];
        }
    }
    pthread_mutex_unlock(&g_rwlock_table_lock);

    return NULL;
}

static SceUID rwlock_create(const char *name, uint32_t attr)
{
    SceUID uid = 0;

    pthread_mutex_lock(&g_rwlock_table_lock);
    for (int i = 0; i < VITA_RWLOCK_SLOTS; i++) {
        VitaRWLock *lock = &g_rwlocks[i];

        if (lock->used)
            continue;

        memset(lock, 0, sizeof(*lock));
        if (pthread_rwlock_init(&lock->lock, NULL) != 0)
            break;

        lock->used = true;
        lock->uid = SceSysmem_get_next_uid();
        lock->refs = 1;
        lock->attr = attr;
        if (name)
            strncpy(lock->name, name, sizeof(lock->name) - 1);
        uid = lock->uid;
        break;
    }
    pthread_mutex_unlock(&g_rwlock_table_lock);

    return uid;
}

static int rwlock_open(const char *name)
{
    int uid = SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (!name)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&g_rwlock_table_lock);
    for (int i = 0; i < VITA_RWLOCK_SLOTS; i++) {
        if (g_rwlocks[i].used && strncmp(g_rwlocks[i].name, name, sizeof(g_rwlocks[i].name)) == 0) {
            g_rwlocks[i].refs++;
            uid = (int)g_rwlocks[i].uid;
            break;
        }
    }
    pthread_mutex_unlock(&g_rwlock_table_lock);

    return uid;
}

static int rwlock_close(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    pthread_mutex_lock(&g_rwlock_table_lock);
    if (lock->refs > 0)
        lock->refs--;
    pthread_mutex_unlock(&g_rwlock_table_lock);

    return 0;
}

static int rwlock_delete(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    pthread_mutex_lock(&g_rwlock_table_lock);
    pthread_rwlock_destroy(&lock->lock);
    memset(lock, 0, sizeof(*lock));
    pthread_mutex_unlock(&g_rwlock_table_lock);

    return 0;
}

static int rwlock_lock_read(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (pthread_rwlock_rdlock(&lock->lock) != 0)
        return SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_LOCK;

    lock->lock_count++;
    return 0;
}

static SceUID current_thread_uid(void)
{
    VitaThreadInfo *info = SceKernelThreadMgr_get_thread_info();

    return info ? info->uid : 0;
}

static int rwlock_lock_write(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (pthread_rwlock_wrlock(&lock->lock) != 0)
        return SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_LOCK;

    lock->write_owner = current_thread_uid();
    lock->lock_count++;
    return 0;
}

static int rwlock_try_read(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (pthread_rwlock_tryrdlock(&lock->lock) != 0)
        return SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_LOCK;

    lock->lock_count++;
    return 0;
}

static int rwlock_try_write(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (pthread_rwlock_trywrlock(&lock->lock) != 0)
        return SCE_KERNEL_ERROR_RW_LOCK_FAILED_TO_LOCK;

    lock->write_owner = current_thread_uid();
    lock->lock_count++;
    return 0;
}

static int rwlock_unlock(SceUID uid)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;

    if (pthread_rwlock_unlock(&lock->lock) != 0)
        return SCE_KERNEL_ERROR_RW_LOCK_NOT_OWNED;

    if (lock->lock_count > 0)
        lock->lock_count--;
    if (lock->lock_count == 0)
        lock->write_owner = 0;

    return 0;
}

static int rwlock_get_info(SceUID uid, SceKernelRWLockInfo *info)
{
    VitaRWLock *lock = rwlock_lookup(uid);

    if (!lock)
        return SCE_KERNEL_ERROR_UNKNOWN_RW_LOCK_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->rwLockId = lock->uid;
    memcpy(info->name, lock->name, sizeof(info->name));
    info->attr = lock->attr;
    info->lockCount = lock->lock_count;
    info->writeOwnerId = lock->write_owner;

    return 0;
}

EXPORT(SceLibKernel, 0x8667951D, SceUID, sceKernelCreateRWLock, const char *name, SceUInt32 attr,
       const SceKernelRWLockOptParam *opt_param)
{
    (void)opt_param;
    return rwlock_create(name, attr);
}

EXPORT(SceLibKernel, 0x190CA94B, int, sceKernelCancelRWLock, SceUID rwlock_id)
{
    return rwlock_unlock(rwlock_id);
}

EXPORT(SceLibKernel, 0x079A573B, int, sceKernelGetRWLockInfo, SceUID rwlock_id,
       SceKernelRWLockInfo *info)
{
    return rwlock_get_info(rwlock_id, info);
}

EXPORT(SceLibKernel, 0xFA670F0F, int, sceKernelLockReadRWLock, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_read(rwlock_id);
}

EXPORT(SceLibKernel, 0x2D4A62B7, int, sceKernelLockReadRWLockCB, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_read(rwlock_id);
}

EXPORT(SceLibKernel, 0x67A187BB, int, sceKernelLockWriteRWLock, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_write(rwlock_id);
}

EXPORT(SceLibKernel, 0xA4777082, int, sceKernelLockWriteRWLockCB, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_write(rwlock_id);
}

EXPORT(SceThreadmgr, 0xE73649CA, int, sceKernelDeleteRWLock, SceUID rwlock_id)
{
    return rwlock_delete(rwlock_id);
}

EXPORT(SceThreadmgr, 0xCE510196, SceUID, sceKernelOpenRWLock, const char *name)
{
    return rwlock_open(name);
}

EXPORT(SceThreadmgr, 0xFD5BD5C1, int, sceKernelCloseRWLock, SceUID rwlock_id)
{
    return rwlock_close(rwlock_id);
}

EXPORT(SceThreadmgr, 0xEFDDA456, int, sceKernelTryLockReadRWLock, SceUID rwlock_id)
{
    return rwlock_try_read(rwlock_id);
}

EXPORT(SceThreadmgr, 0x206CBB66, int, sceKernelTryLockWriteRWLock, SceUID rwlock_id)
{
    return rwlock_try_write(rwlock_id);
}

EXPORT(SceThreadmgr, 0x3EF91145, int, sceKernelUnlockReadRWLock, SceUID rwlock_id)
{
    return rwlock_unlock(rwlock_id);
}

EXPORT(SceThreadmgr, 0xB4151397, int, sceKernelUnlockWriteRWLock, SceUID rwlock_id)
{
    return rwlock_unlock(rwlock_id);
}

EXPORT(SceThreadmgr, 0xB1877F5E, SceUID, _sceKernelCreateRWLock, const char *name, SceUInt32 attr,
       const SceKernelRWLockOptParam *opt_param)
{
    (void)opt_param;
    return rwlock_create(name, attr);
}

EXPORT(SceThreadmgr, 0xD004EA15, int, _sceKernelCancelRWLock, SceUID rwlock_id)
{
    return rwlock_unlock(rwlock_id);
}

EXPORT(SceThreadmgr, 0x8CE3AFC7, int, _sceKernelGetRWLockInfo, SceUID rwlock_id,
       SceKernelRWLockInfo *info)
{
    return rwlock_get_info(rwlock_id, info);
}

EXPORT(SceThreadmgr, 0x7EB9E8B5, int, _sceKernelLockReadRWLock, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_read(rwlock_id);
}

EXPORT(SceThreadmgr, 0x5D86D763, int, _sceKernelLockReadRWLockCB, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_read(rwlock_id);
}

EXPORT(SceThreadmgr, 0x80191FAA, int, _sceKernelLockWriteRWLock, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_write(rwlock_id);
}

EXPORT(SceThreadmgr, 0xDBD09B09, int, _sceKernelLockWriteRWLockCB, SceUID rwlock_id,
       unsigned int *timeout)
{
    (void)timeout;
    return rwlock_lock_write(rwlock_id);
}

/* ------------------------------------------------------------------------- */
/* Lightweight condition variables                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    pthread_cond_t cond;
    VitaLwMutex *mutex;
    SceUID uid;
    char name[32];
    uint32_t attr;
    SceKernelLwCondWork *work;
    int waiting;
} VitaLwCond;

#define VITA_LWCOND_SLOTS 64

static VitaLwCond *g_lwconds[VITA_LWCOND_SLOTS];
static pthread_mutex_t g_lwcond_table_lock = PTHREAD_MUTEX_INITIALIZER;

static VitaLwCond *lwcond_from_work(SceKernelLwCondWork *work)
{
    void *pointer;

    if (!work)
        return NULL;

    memcpy(&pointer, &work->data[0], sizeof(pointer));
    return pointer;
}

static VitaLwCond *lwcond_lookup(SceUID uid)
{
    VitaLwCond *result = NULL;

    pthread_mutex_lock(&g_lwcond_table_lock);
    for (int i = 0; i < VITA_LWCOND_SLOTS; i++) {
        if (g_lwconds[i] && g_lwconds[i]->uid == uid) {
            result = g_lwconds[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_lwcond_table_lock);

    return result;
}

int SceKernelLocks_classify_uid(SceUID uid)
{
    (void)uid;
    return -1;
}

static int lwcond_wait(SceKernelLwCondWork *work, unsigned int *timeout)
{
    VitaLwCond *cond = lwcond_from_work(work);
    int result;

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
    if (!cond->mutex)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    cond->waiting++;
    if (timeout) {
        struct timespec ts;
        uint64_t micros = (uint64_t)(*timeout);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(micros / 1000000ull);
        ts.tv_nsec += (long)(micros % 1000000ull) * 1000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        result = pthread_cond_timedwait(&cond->cond, &cond->mutex->mutex, &ts);
        cond->waiting--;
        if (result == ETIMEDOUT)
            return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
        return result == 0 ? 0 : SCE_KERNEL_ERROR_LW_COND_ERROR;
    }

    result = pthread_cond_wait(&cond->cond, &cond->mutex->mutex);
    cond->waiting--;
    return result == 0 ? 0 : SCE_KERNEL_ERROR_LW_COND_ERROR;
}

static void lwcond_fill_info(const VitaLwCond *cond, VitaLwCondInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->lwcond_id = cond->uid;
    memcpy(info->name, cond->name, sizeof(info->name));
    info->attr = (int)cond->attr;
    info->work = cond->work;
    info->lwmutex = cond->mutex ? cond->mutex->work : NULL;
    info->num_wait_threads = cond->waiting;
}

static int lwcond_get_info(SceUID uid, VitaLwCondInfo *info, SceSize size)
{
    VitaLwCond *cond = lwcond_lookup(uid);

    if (!cond)
        return SCE_KERNEL_ERROR_UNKNOWN_LW_COND_ID;
    if (!info)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    if (size != 0 && size < sizeof(VitaLwCondInfo)) {
        VitaLwCondInfo local;

        lwcond_fill_info(cond, &local);
        local.size = size;
        memcpy(info, &local, size);
    } else {
        lwcond_fill_info(cond, info);
    }

    return 0;
}

EXPORT(SceLibKernel, 0x48C7EAE6, int, sceKernelCreateLwCond, SceKernelLwCondWork *work,
       const char *name, unsigned int attr, SceKernelLwMutexWork *lw_mutex,
       const SceKernelLwCondOptParam *opt_param)
{
    VitaLwCond *cond;
    int slot = -1;

    (void)opt_param;

    if (!work || !lw_mutex)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    cond = calloc(1, sizeof(*cond));
    if (!cond)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    if (pthread_cond_init(&cond->cond, NULL) != 0) {
        free(cond);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    cond->mutex = SceKernelSync_lw_mutex_from_work(lw_mutex);
    cond->uid = SceSysmem_get_next_uid();
    cond->attr = attr;
    cond->work = work;
    if (name)
        strncpy(cond->name, name, sizeof(cond->name) - 1);

    pthread_mutex_lock(&g_lwcond_table_lock);
    for (int i = 0; i < VITA_LWCOND_SLOTS; i++) {
        if (!g_lwconds[i]) {
            g_lwconds[i] = cond;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_lwcond_table_lock);

    if (slot < 0) {
        pthread_cond_destroy(&cond->cond);
        free(cond);
        return SCE_KERNEL_ERROR_NO_MEMORY;
    }

    memset(work, 0, sizeof(*work));
    memcpy(&work->data[0], &cond, sizeof(cond));

    return 0;
}

EXPORT(SceLibKernel, 0x721F6CB3, int, sceKernelDeleteLwCond, SceKernelLwCondWork *work)
{
    VitaLwCond *cond = lwcond_from_work(work);

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_mutex_lock(&g_lwcond_table_lock);
    for (int i = 0; i < VITA_LWCOND_SLOTS; i++) {
        if (g_lwconds[i] == cond) {
            g_lwconds[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_lwcond_table_lock);

    pthread_cond_destroy(&cond->cond);
    free(cond);
    memset(work, 0, sizeof(*work));

    return 0;
}

EXPORT(SceLibKernel, 0x3AC63B9A, int, sceKernelSignalLwCond, SceKernelLwCondWork *work)
{
    VitaLwCond *cond = lwcond_from_work(work);

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_cond_signal(&cond->cond);
    return 0;
}

EXPORT(SceLibKernel, 0xE5241A0C, int, sceKernelSignalLwCondAll, SceKernelLwCondWork *work)
{
    VitaLwCond *cond = lwcond_from_work(work);

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_cond_broadcast(&cond->cond);
    return 0;
}

EXPORT(SceLibKernel, 0xFC1A48EB, int, sceKernelSignalLwCondTo, SceKernelLwCondWork *work,
       SceUID thread_id)
{
    VitaLwCond *cond = lwcond_from_work(work);

    (void)thread_id;

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    pthread_cond_signal(&cond->cond);
    return 0;
}

EXPORT(SceLibKernel, 0xE1878282, int, sceKernelWaitLwCond, SceKernelLwCondWork *work,
       unsigned int *timeout)
{
    return lwcond_wait(work, timeout);
}

EXPORT(SceLibKernel, 0x8FA54B07, int, sceKernelWaitLwCondCB, SceKernelLwCondWork *work,
       unsigned int *timeout)
{
    return lwcond_wait(work, timeout);
}

EXPORT(SceThreadmgr, 0x940B9EBE, int, _sceKernelCreateLwCond, SceKernelLwCondWork *work,
       const char *name, unsigned int attr, SceKernelLwMutexWork *lw_mutex,
       const SceKernelLwCondOptParam *opt_param)
{
    return sceKernelCreateLwCond(work, name, attr, lw_mutex, opt_param);
}

EXPORT(SceThreadmgr, 0x75FCF058, int, _sceKernelDeleteLwCond, SceKernelLwCondWork *work)
{
    return sceKernelDeleteLwCond(work);
}

EXPORT(SceThreadmgr, 0xC37F6983, int, _sceKernelSignalLwCond, SceKernelLwCondWork *work)
{
    return sceKernelSignalLwCond(work);
}

EXPORT(SceThreadmgr, 0x07D2584A, int, _sceKernelSignalLwCondAll, SceKernelLwCondWork *work)
{
    return sceKernelSignalLwCondAll(work);
}

EXPORT(SceThreadmgr, 0x6F1A4A2E, int, _sceKernelSignalLwCondTo, SceKernelLwCondWork *work,
       SceUID thread_id)
{
    return sceKernelSignalLwCondTo(work, thread_id);
}

EXPORT(SceThreadmgr, 0x18C65756, int, _sceKernelWaitLwCond, SceKernelLwCondWork *work,
       unsigned int *timeout)
{
    return lwcond_wait(work, timeout);
}

EXPORT(SceThreadmgr, 0x72DBB96B, int, _sceKernelWaitLwCondCB, SceKernelLwCondWork *work,
       unsigned int *timeout)
{
    return lwcond_wait(work, timeout);
}

/* ------------------------------------------------------ lw cond introspection */

EXPORT(SceThreadmgr, 0x6C79F2F2, int, _sceKernelGetLwCondInfo, SceUID lwcond_id,
       VitaLwCondInfo *info)
{
    return lwcond_get_info(lwcond_id, info, 0);
}

EXPORT(SceThreadmgr, 0xD9E78D30, int, _sceKernelGetLwCondInfoById, SceUID lwcond_id,
       VitaLwCondInfo *info, SceSize size)
{
    return lwcond_get_info(lwcond_id, info, size);
}

EXPORT(SceLibKernel, 0xE4DF36A0, int, sceKernelGetLwCondInfo, SceKernelLwCondWork *work,
       VitaLwCondInfo *info)
{
    VitaLwCond *cond = lwcond_from_work(work);

    if (!cond)
        return SCE_KERNEL_ERROR_ILLEGAL_ADDR;

    return lwcond_get_info(cond->uid, info, 0);
}

EXPORT(SceLibKernel, 0x971F1DE8, int, sceKernelGetLwCondInfoById, SceUID lwcond_id,
       VitaLwCondInfo *info)
{
    SceSize size = info ? info->size : 0;

    return lwcond_get_info(lwcond_id, info, size);
}

/* ------------------------------------------------------------------------- */
/* Remaining lightweight mutex entry points                                  */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibKernel, 0xB84EF718, int, _sceKernelCreateLwMutex, SceKernelLwMutexWork *work,
       const char *name, unsigned int attr, int init_count, const SceKernelLwMutexOptParam *option)
{
    return sceKernelCreateLwMutex(work, name, attr, init_count, option);
}

EXPORT(SceThreadmgr, 0x4BB3154A, int, __sceKernelCreateLwMutex, SceKernelLwMutexWork *work,
       const char *name, unsigned int attr, int init_count, const SceKernelLwMutexOptParam *option)
{
    return sceKernelCreateLwMutex(work, name, attr, init_count, option);
}

EXPORT(SceThreadmgr, 0x91262C5F, int, _sceKernelDeleteLwMutex, SceKernelLwMutexWork *work)
{
    return sceKernelDeleteLwMutex(work);
}

EXPORT(SceThreadmgr, 0x9C572180, int, _sceKernelLockLwMutex, SceKernelLwMutexWork *work,
       int count, SceUInt *timeout)
{
    return sceKernelLockLwMutex(work, count, timeout);
}

EXPORT(SceLibKernel, 0x3148C6B6, int, sceKernelLockLwMutexCB, SceKernelLwMutexWork *work,
       int count, SceUInt *timeout)
{
    return sceKernelLockLwMutex(work, count, timeout);
}

EXPORT(SceThreadmgr, 0x2ABC41DF, int, _sceKernelUnlockLwMutex, SceKernelLwMutexWork *work,
       int count)
{
    return sceKernelUnlockLwMutex(work, count);
}

EXPORT(SceLibKernel, 0x120AFC8C, int, sceKernelUnlockLwMutex2, SceKernelLwMutexWork *work,
       int count)
{
    return sceKernelUnlockLwMutex(work, count);
}
