#ifndef VITA2NATIVE_KERNEL_SYNC_H
#define VITA2NATIVE_KERNEL_SYNC_H

#include <pthread.h>

#include <psp2/kernel/threadmgr.h>

/*
 * Lightweight mutexes live inside a guest work area. The work area stores a
 * pointer to this host object in its first word; LwCond needs the same object
 * to drive pthread_cond_wait, so the type is shared between the two modules.
 */
typedef struct {
    pthread_mutex_t mutex;
} VitaLwMutex;

VitaLwMutex *SceKernelSync_lw_mutex_from_work(SceKernelLwMutexWork *work);

#endif /* VITA2NATIVE_KERNEL_SYNC_H */
