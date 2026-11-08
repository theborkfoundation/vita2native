#ifndef SCE_KERNEL_THREAD_MGR_H
#define SCE_KERNEL_THREAD_MGR_H

#include "host.h"

#include <psp2/kernel/threadmgr.h>

typedef struct {
    uint32_t index;
    Thread thread;
    SceUID uid;
    int host_tid;
    char name[32];
    SceKernelThreadEntry entry;
    SceSize arglen;
    void *argp;
    int return_status;
    void **vita_tls;
    int init_priority;
    int current_priority;
    SceSize stack_size;
    uintptr_t stack_base;
    SceUInt attr;
    int affinity_mask;
    uint32_t vfp_exception;
    uint32_t suspend_count;
    bool signal_pending;
    Mutex signal_lock;
    CondVar signal_cond;
} VitaThreadInfo;

int SceKernelThreadMgr_init(void);
int SceKernelThreadMgr_finish(void);

/* Semaphores, event flags and lightweight mutexes. */
int SceKernelSync_init(void);
int SceKernelSync_finish(void);

VitaThreadInfo *SceKernelThreadMgr_get_thread_info(void);
VitaThreadInfo *SceKernelThreadMgr_get_fallback_thread_info(void);
/* Runs callbacks queued for the calling thread. The Vita does this when the
   thread enters a blocking wait; titles (Unity's pthread.suprx among them)
   rely on it to make progress. */
int SceKernelThreadMgr_run_pending_callbacks(void);
int SceKernelThreadMgr_main_entry(SceKernelThreadEntry entry, int args, void *argp);
void SceKernelThreadMgr_notify_event_callbacks(SceUID event_id, int notify_arg);

#endif
