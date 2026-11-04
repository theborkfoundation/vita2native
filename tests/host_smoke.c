#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "fs.h"
#include "host.h"

static atomic_int g_counter;
static int g_tls_slot;

static void worker(void *arg)
{
    (void)arg;

    threadTlsSet(g_tls_slot, (void *)(uintptr_t)0x1234);
    assert(threadTlsGet(g_tls_slot) == (void *)(uintptr_t)0x1234);
    atomic_fetch_add(&g_counter, 1);
}

int main(void)
{
    char path[PATH_MAX];
    Mutex mutex;
    CondVar condvar;
    RwLock rwlock;
    UEvent event;
    Thread thread;

    mutexInit(&mutex);
    mutexLock(&mutex);
    assert(mutexTryLock(&mutex) == false);
    mutexUnlock(&mutex);

    condvarInit(&condvar);
    condvarWakeAll(&condvar);
    condvarDestroy(&condvar);

    rwlockInit(&rwlock);
    rwlockReadLock(&rwlock);
    rwlockReadUnlock(&rwlock);
    rwlockWriteLock(&rwlock);
    rwlockWriteUnlock(&rwlock);
    rwlockDestroy(&rwlock);

    ueventCreate(&event, true);
    ueventSignal(&event);
    assert(waitSingle(waiterForUEvent(&event), 1000000000ull) == 0);
    assert(waitSingle(waiterForUEvent(&event), 1000000ull) != 0);
    ueventDestroy(&event);

    g_tls_slot = threadTlsAlloc(NULL);
    assert(g_tls_slot > 0);

    assert(threadCreate(&thread, worker, NULL, NULL, 64 * 1024, 32, 0) == 0);
    assert(threadStart(&thread) == 0);
    assert(waitSingle(waiterForThread(&thread), 2000000000ull) == 0);
    assert(atomic_load(&g_counter) == 1);
    assert(threadClose(&thread) == 0);

    threadTlsFree(g_tls_slot);
    mutexDestroy(&mutex);

    mkdir("build", 0755);
    assert(host_fs_init("build/fs_root", "build/fs_root/app") == 0);
    assert(host_fs_resolve_path("ux0:/data/foo", path, sizeof(path)) == 0);
    assert(strcmp(path, "build/fs_root/ux0/data/foo") == 0);
    assert(host_fs_resolve_path("app0:/assets/x", path, sizeof(path)) == 0);
    assert(strcmp(path, "build/fs_root/app/assets/x") == 0);
    assert(host_fs_resolve_path("ux0:/../escape", path, sizeof(path)) != 0);

    host_sleep_ns(1000000);

    puts("host_smoke: ok");
    return 0;
}
