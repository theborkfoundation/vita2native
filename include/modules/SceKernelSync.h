#ifndef VITA2NATIVE_KERNEL_SYNC_H
#define VITA2NATIVE_KERNEL_SYNC_H

#include <pthread.h>

#include <psp2/kernel/threadmgr.h>

/*
 * Lightweight mutexes live inside a guest work area. The work area stores a
 * pointer to this host object in its first word; LwCond needs the same object
 * to drive pthread_cond_wait, so the type is shared between the two modules.
 */
typedef struct VitaLwMutex {
    pthread_mutex_t mutex;
    SceUID uid;
    char name[32];
    uint32_t attr;
    SceKernelLwMutexWork *work;
    int init_count;
    int current_count;
    SceUID owner;
    uint32_t waiting;
    struct VitaLwMutex *next;
} VitaLwMutex;

/*
 * Introspection layout. VitaSDK exposes these only as kernel types, so the
 * 3.60 user-space layout is mirrored here (0x40 and 0x38 bytes respectively).
 */
typedef struct {
    SceSize size;
    SceUID uid;
    char name[32];
    int attr;
    SceKernelLwMutexWork *work;
    int init_count;
    int current_count;
    SceUID current_owner_id;
    SceUInt32 num_wait_threads;
} VitaLwMutexInfo;

typedef struct {
    SceSize size;
    SceUID lwcond_id;
    char name[32];
    int attr;
    SceKernelLwCondWork *work;
    SceKernelLwMutexWork *lwmutex;
    int num_wait_threads;
} VitaLwCondInfo;

VitaLwMutex *SceKernelSync_lw_mutex_from_work(SceKernelLwMutexWork *work);
VitaLwMutex *SceKernelSync_lw_mutex_lookup(SceUID uid);
void SceKernelSync_lw_mutex_fill_info(const VitaLwMutex *lw, VitaLwMutexInfo *info);
int SceKernelSync_lw_mutex_get_info(SceUID uid, VitaLwMutexInfo *info, SceSize size);

/* Returns a SceKernelIdListType or -1 when the UID is not a sync object. */
int SceKernelSync_classify_uid(SceUID uid);
int SceKernelEvent_classify_uid(SceUID uid);

#endif /* VITA2NATIVE_KERNEL_SYNC_H */
