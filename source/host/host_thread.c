#include "host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#define HOST_MAX_LOCKS   1024
#define HOST_MAX_THREADS 1024
#define HOST_MAX_TLS     128

/*
 * Guest threads are host threads, and their guest-requested stack sizes are
 * often small (64 KiB). That is fine for guest code, but host-side code that
 * runs on those threads (Mesa's llvmpipe JIT compiles shaders on the calling
 * thread) needs a lot more stack; LLVM overflows a 64 KiB stack in
 * TargetLoweringBase::initActions. The guest's stack size is not emulated, so
 * a roomier host stack is harmless.
 */
#define HOST_THREAD_STACK_MIN (1024 * 1024)
#define HOST_THREAD_STACK_MAX (8 * 1024 * 1024)

#if defined(__linux__)
#define HOST_COND_CLOCK CLOCK_MONOTONIC
#else
#define HOST_COND_CLOCK CLOCK_REALTIME
#endif

typedef struct {
    bool used;
    pthread_mutex_t mutex;
} HostMutexSlot;

typedef struct {
    bool used;
    pthread_cond_t cond;
} HostCondSlot;

typedef struct {
    bool used;
    pthread_rwlock_t lock;
} HostRwLockSlot;

typedef struct {
    bool used;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool autoclear;
    bool signaled;
} HostEventSlot;

typedef struct {
    bool used;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t done;
    bool started;
    bool finished;
    int status;
    void (*entry)(void *);
    void *arg;
    size_t stack_size;
    int priority;
    int core;
} HostThreadSlot;

static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static HostMutexSlot g_mutexes[HOST_MAX_LOCKS];
static HostCondSlot g_conds[HOST_MAX_LOCKS];
static HostRwLockSlot g_rwlocks[HOST_MAX_LOCKS];
static HostEventSlot g_uevents[HOST_MAX_LOCKS];
static HostEventSlot g_levents[HOST_MAX_LOCKS];
static HostThreadSlot g_threads[HOST_MAX_THREADS];

static pthread_key_t g_tls_keys[HOST_MAX_TLS];
static bool g_tls_used[HOST_MAX_TLS];

static int host_cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    int ret;

    ret = pthread_condattr_init(&attr);
    if (ret != 0)
        return ret;

#if defined(__linux__)
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif

    ret = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);

    return ret;
}

static void host_deadline(uint64_t timeout_ns, struct timespec *ts)
{
    uint64_t sec = timeout_ns / 1000000000ull;
    uint64_t nsec = timeout_ns % 1000000000ull;

    clock_gettime(HOST_COND_CLOCK, ts);

    ts->tv_sec += (time_t)sec;
    ts->tv_nsec += (long)nsec;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int host_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t timeout_ns)
{
    struct timespec ts;

    if (timeout_ns == UINT64_MAX)
        return pthread_cond_wait(cond, mutex);

    host_deadline(timeout_ns, &ts);

    return pthread_cond_timedwait(cond, mutex, &ts);
}

static HostMutexSlot *host_mutex_slot(Mutex *mutex)
{
    HostMutexSlot *slot;

    if (!mutex || mutex->magic != HOST_MUTEX_MAGIC || mutex->id == 0 ||
        mutex->id > HOST_MAX_LOCKS)
        return NULL;

    slot = &g_mutexes[mutex->id - 1];
    return slot->used ? slot : NULL;
}

static HostCondSlot *host_cond_slot(CondVar *condvar)
{
    HostCondSlot *slot;

    if (!condvar || condvar->id == 0 || condvar->id > HOST_MAX_LOCKS)
        return NULL;

    slot = &g_conds[condvar->id - 1];
    return slot->used ? slot : NULL;
}

static HostRwLockSlot *host_rwlock_slot(RwLock *lock)
{
    HostRwLockSlot *slot;

    if (!lock || lock->id == 0 || lock->id > HOST_MAX_LOCKS)
        return NULL;

    slot = &g_rwlocks[lock->id - 1];
    return slot->used ? slot : NULL;
}

static HostEventSlot *host_event_slot(uint32_t id, HostEventSlot *table)
{
    HostEventSlot *slot;

    if (id == 0 || id > HOST_MAX_LOCKS)
        return NULL;

    slot = &table[id - 1];
    return slot->used ? slot : NULL;
}

static HostThreadSlot *host_thread_slot(Thread *thread)
{
    HostThreadSlot *slot;

    if (!thread || thread->id == 0 || thread->id > HOST_MAX_THREADS)
        return NULL;

    slot = &g_threads[thread->id - 1];
    return slot->used ? slot : NULL;
}

void mutexInit(Mutex *mutex)
{
    if (!mutex)
        return;

    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_LOCKS; i++) {
        if (!g_mutexes[i].used) {
            g_mutexes[i].used = true;
            pthread_mutex_init(&g_mutexes[i].mutex, NULL);
            mutex->magic = HOST_MUTEX_MAGIC;
            mutex->id = (uint32_t)i + 1;
            pthread_mutex_unlock(&g_table_lock);
            return;
        }
    }
    mutex->magic = 0;
    mutex->id = 0;
    pthread_mutex_unlock(&g_table_lock);
}

void mutexLock(Mutex *mutex)
{
    HostMutexSlot *slot = host_mutex_slot(mutex);

    if (slot)
        pthread_mutex_lock(&slot->mutex);
}

bool mutexTryLock(Mutex *mutex)
{
    HostMutexSlot *slot = host_mutex_slot(mutex);

    return slot ? pthread_mutex_trylock(&slot->mutex) == 0 : false;
}

void mutexUnlock(Mutex *mutex)
{
    HostMutexSlot *slot = host_mutex_slot(mutex);

    if (slot)
        pthread_mutex_unlock(&slot->mutex);
}

void mutexDestroy(Mutex *mutex)
{
    HostMutexSlot *slot = host_mutex_slot(mutex);

    if (!slot)
        return;

    pthread_mutex_destroy(&slot->mutex);
    slot->used = false;
    mutex->magic = 0;
    mutex->id = 0;
}

void condvarInit(CondVar *condvar)
{
    if (!condvar)
        return;

    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_LOCKS; i++) {
        if (!g_conds[i].used) {
            g_conds[i].used = true;
            host_cond_init(&g_conds[i].cond);
            condvar->id = (uint32_t)i + 1;
            pthread_mutex_unlock(&g_table_lock);
            return;
        }
    }
    condvar->id = 0;
    pthread_mutex_unlock(&g_table_lock);
}

void condvarWait(CondVar *condvar, Mutex *mutex)
{
    HostCondSlot *cond = host_cond_slot(condvar);
    HostMutexSlot *m = host_mutex_slot(mutex);

    if (!cond || !m)
        return;

    /*
     * Modules call condvarWait() without holding the mutex. Keep that contract
     * on the host: lock, wait, unlock.
     */
    pthread_mutex_lock(&m->mutex);
    pthread_cond_wait(&cond->cond, &m->mutex);
    pthread_mutex_unlock(&m->mutex);
}

int condvarWaitTimeout(CondVar *condvar, Mutex *mutex, uint64_t timeout_ns)
{
    HostCondSlot *cond = host_cond_slot(condvar);
    HostMutexSlot *m = host_mutex_slot(mutex);
    int ret;

    if (!cond || !m)
        return -1;

    pthread_mutex_lock(&m->mutex);
    ret = host_cond_wait(&cond->cond, &m->mutex, timeout_ns);
    pthread_mutex_unlock(&m->mutex);

    return ret == ETIMEDOUT ? -1 : 0;
}

void condvarWaitLocked(CondVar *condvar, Mutex *mutex)
{
    HostCondSlot *cond = host_cond_slot(condvar);
    HostMutexSlot *m = host_mutex_slot(mutex);

    if (!cond || !m)
        return;

    pthread_cond_wait(&cond->cond, &m->mutex);
}

int condvarWaitTimeoutLocked(CondVar *condvar, Mutex *mutex, uint64_t timeout_ns)
{
    HostCondSlot *cond = host_cond_slot(condvar);
    HostMutexSlot *m = host_mutex_slot(mutex);
    int ret;

    if (!cond || !m)
        return -1;

    ret = host_cond_wait(&cond->cond, &m->mutex, timeout_ns);

    return ret == ETIMEDOUT ? -1 : 0;
}

void condvarWakeAll(CondVar *condvar)
{
    HostCondSlot *cond = host_cond_slot(condvar);

    if (cond)
        pthread_cond_broadcast(&cond->cond);
}

void condvarDestroy(CondVar *condvar)
{
    HostCondSlot *cond = host_cond_slot(condvar);

    if (!cond)
        return;

    pthread_cond_destroy(&cond->cond);
    cond->used = false;
    condvar->id = 0;
}

void rwlockInit(RwLock *lock)
{
    if (!lock)
        return;

    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_LOCKS; i++) {
        if (!g_rwlocks[i].used) {
            g_rwlocks[i].used = true;
            pthread_rwlock_init(&g_rwlocks[i].lock, NULL);
            lock->id = (uint32_t)i + 1;
            pthread_mutex_unlock(&g_table_lock);
            return;
        }
    }
    lock->id = 0;
    pthread_mutex_unlock(&g_table_lock);
}

void rwlockReadLock(RwLock *lock)
{
    HostRwLockSlot *slot = host_rwlock_slot(lock);

    if (slot)
        pthread_rwlock_rdlock(&slot->lock);
}

void rwlockReadUnlock(RwLock *lock)
{
    HostRwLockSlot *slot = host_rwlock_slot(lock);

    if (slot)
        pthread_rwlock_unlock(&slot->lock);
}

void rwlockWriteLock(RwLock *lock)
{
    HostRwLockSlot *slot = host_rwlock_slot(lock);

    if (slot)
        pthread_rwlock_wrlock(&slot->lock);
}

void rwlockWriteUnlock(RwLock *lock)
{
    HostRwLockSlot *slot = host_rwlock_slot(lock);

    if (slot)
        pthread_rwlock_unlock(&slot->lock);
}

void rwlockDestroy(RwLock *lock)
{
    HostRwLockSlot *slot = host_rwlock_slot(lock);

    if (!slot)
        return;

    pthread_rwlock_destroy(&slot->lock);
    slot->used = false;
    lock->id = 0;
}

static void host_event_create(HostEventSlot *table, uint32_t *id, bool autoclear)
{
    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_LOCKS; i++) {
        if (!table[i].used) {
            HostEventSlot *slot = &table[i];
            slot->used = true;
            pthread_mutex_init(&slot->mutex, NULL);
            host_cond_init(&slot->cond);
            slot->autoclear = autoclear;
            slot->signaled = false;
            *id = (uint32_t)i + 1;
            pthread_mutex_unlock(&g_table_lock);
            return;
        }
    }
    *id = 0;
    pthread_mutex_unlock(&g_table_lock);
}

static void host_event_destroy(HostEventSlot *table, uint32_t *id)
{
    HostEventSlot *slot = host_event_slot(*id, table);

    if (!slot)
        return;

    pthread_cond_destroy(&slot->cond);
    pthread_mutex_destroy(&slot->mutex);
    slot->used = false;
    *id = 0;
}

void ueventCreate(UEvent *event, bool autoclear)
{
    if (event)
        host_event_create(g_uevents, &event->id, autoclear);
}

void ueventSignal(UEvent *event)
{
    HostEventSlot *slot = event ? host_event_slot(event->id, g_uevents) : NULL;

    if (!slot)
        return;

    pthread_mutex_lock(&slot->mutex);
    slot->signaled = true;
    pthread_cond_broadcast(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
}

void ueventClear(UEvent *event)
{
    HostEventSlot *slot = event ? host_event_slot(event->id, g_uevents) : NULL;

    if (!slot)
        return;

    pthread_mutex_lock(&slot->mutex);
    slot->signaled = false;
    pthread_mutex_unlock(&slot->mutex);
}

void ueventDestroy(UEvent *event)
{
    if (event)
        host_event_destroy(g_uevents, &event->id);
}

void leventInit(LEvent *event, bool autoclear, bool initial)
{
    HostEventSlot *slot;

    if (!event)
        return;

    host_event_create(g_levents, &event->id, autoclear);

    slot = host_event_slot(event->id, g_levents);
    if (slot)
        slot->signaled = initial;
}

bool leventTryWait(LEvent *event)
{
    HostEventSlot *slot = event ? host_event_slot(event->id, g_levents) : NULL;
    bool result;

    if (!slot)
        return false;

    pthread_mutex_lock(&slot->mutex);
    result = slot->signaled;
    if (result && slot->autoclear)
        slot->signaled = false;
    pthread_mutex_unlock(&slot->mutex);

    return result;
}

void leventClear(LEvent *event)
{
    HostEventSlot *slot = event ? host_event_slot(event->id, g_levents) : NULL;

    if (!slot)
        return;

    pthread_mutex_lock(&slot->mutex);
    slot->signaled = false;
    pthread_mutex_unlock(&slot->mutex);
}

void leventSignal(LEvent *event)
{
    HostEventSlot *slot = event ? host_event_slot(event->id, g_levents) : NULL;

    if (!slot)
        return;

    pthread_mutex_lock(&slot->mutex);
    slot->signaled = true;
    pthread_cond_broadcast(&slot->cond);
    pthread_mutex_unlock(&slot->mutex);
}

void leventDestroy(LEvent *event)
{
    if (event)
        host_event_destroy(g_levents, &event->id);
}

Waiter waiterForThread(Thread *thread)
{
    Waiter waiter = { HOST_WAITER_THREAD, thread };
    return waiter;
}

Waiter waiterForUEvent(UEvent *event)
{
    Waiter waiter = { HOST_WAITER_UEVENT, event };
    return waiter;
}

Result waitSingle(Waiter waiter, uint64_t timeout_ns)
{
    if (waiter.kind == HOST_WAITER_THREAD) {
        HostThreadSlot *slot = host_thread_slot((Thread *)waiter.object);

        if (!slot)
            return -1;

        pthread_mutex_lock(&slot->mutex);
        while (!slot->finished) {
            int ret = host_cond_wait(&slot->done, &slot->mutex, timeout_ns);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&slot->mutex);
                return -1;
            }
        }
        pthread_mutex_unlock(&slot->mutex);

        return 0;
    }

    if (waiter.kind == HOST_WAITER_UEVENT) {
        UEvent *event = waiter.object;
        HostEventSlot *slot = event ? host_event_slot(event->id, g_uevents) : NULL;

        if (!slot)
            return -1;

        pthread_mutex_lock(&slot->mutex);
        while (!slot->signaled) {
            int ret = host_cond_wait(&slot->cond, &slot->mutex, timeout_ns);
            if (ret == ETIMEDOUT) {
                pthread_mutex_unlock(&slot->mutex);
                return -1;
            }
        }
        if (slot->autoclear)
            slot->signaled = false;
        pthread_mutex_unlock(&slot->mutex);

        return 0;
    }

    return -1;
}

static void host_thread_cleanup(void *arg)
{
    HostThreadSlot *slot = arg;

    pthread_mutex_lock(&slot->mutex);
    slot->finished = true;
    pthread_cond_broadcast(&slot->done);
    pthread_mutex_unlock(&slot->mutex);
}

static void *host_thread_trampoline(void *arg)
{
    HostThreadSlot *slot = arg;

    if (slot->priority >= 0) {
        int nice = -20 + ((slot->priority - 28) * 39) / 31;
        if (nice < -20)
            nice = -20;
        if (nice > 19)
            nice = 19;
        setpriority(PRIO_PROCESS, 0, nice);
    }

    pthread_cleanup_push(host_thread_cleanup, slot);
    slot->entry(slot->arg);
    pthread_cleanup_pop(1);

    return NULL;
}

Result threadCreate(Thread *thread, void (*entry)(void *), void *arg, void *stack,
                    size_t stackSize, int priority, int core)
{
    if (!thread || !entry)
        return -1;

    (void)stack;

    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_THREADS; i++) {
        if (!g_threads[i].used) {
            HostThreadSlot *slot = &g_threads[i];
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->entry = entry;
            slot->arg = arg;
            slot->stack_size = stackSize;
            slot->priority = priority;
            slot->core = core;
            pthread_mutex_init(&slot->mutex, NULL);
            host_cond_init(&slot->done);
            thread->id = (uint32_t)i + 1;
            pthread_mutex_unlock(&g_table_lock);
            return 0;
        }
    }
    thread->id = 0;
    pthread_mutex_unlock(&g_table_lock);

    return -1;
}

Result threadStart(Thread *thread)
{
    HostThreadSlot *slot = host_thread_slot(thread);
    pthread_attr_t attr;
    size_t stack_size;
    int ret;

    if (!slot || slot->started)
        return -1;

    ret = pthread_attr_init(&attr);
    if (ret != 0)
        return -1;

    stack_size = slot->stack_size;
    if (stack_size < HOST_THREAD_STACK_MIN)
        stack_size = HOST_THREAD_STACK_MIN;
    if (stack_size > HOST_THREAD_STACK_MAX)
        stack_size = HOST_THREAD_STACK_MAX;
    stack_size = (stack_size + 4095) & ~(size_t)4095;

    /*
     * glibc rejects a stack that is smaller than the executable's static
     * TLS block plus a minimal rest (see allocate_stack() in
     * nptl/allocatestack.c).  That block can be surprisingly large: a Zig
     * -O0 build links a 256 KiB std.debug signal stack into .tbss, so the
     * usual Vita 64 KiB thread stacks are refused with EINVAL.  Grow the
     * host stack until the host accepts it instead of hard-coding a size.
     */
    for (;;) {
        ret = pthread_attr_setstacksize(&attr, stack_size);
        if (ret == 0)
            ret = pthread_create(&slot->thread, &attr, host_thread_trampoline, slot);
        if (ret == 0 || ret != EINVAL || stack_size >= HOST_THREAD_STACK_MAX)
            break;
        stack_size *= 2;
        if (stack_size > HOST_THREAD_STACK_MAX)
            stack_size = HOST_THREAD_STACK_MAX;
    }

    pthread_attr_destroy(&attr);

#if defined(__linux__)
    if (ret == 0 && getenv("VITA2NATIVE_TRACE_THREAD_STACK")) {
        pthread_attr_t got;
        void *stack_addr = NULL;
        size_t got_size = 0;

        if (pthread_getattr_np(slot->thread, &got) == 0) {
            if (pthread_attr_getstack(&got, &stack_addr, &got_size) == 0)
                fprintf(stderr, "vita2native: thread stack %p..%p (0x%zx)\n", stack_addr,
                        (char *)stack_addr + got_size, got_size);
            pthread_attr_destroy(&got);
        }
    }
#endif

    if (ret != 0)
        return -1;

    pthread_detach(slot->thread);
    slot->started = true;

    return 0;
}

Result threadWaitForExit(Thread *thread)
{
    return waitSingle(waiterForThread(thread), UINT64_MAX);
}

Result threadClose(Thread *thread)
{
    HostThreadSlot *slot = host_thread_slot(thread);

    if (!slot)
        return -1;

    if (slot->started)
        waitSingle(waiterForThread(thread), UINT64_MAX);

    pthread_cond_destroy(&slot->done);
    pthread_mutex_destroy(&slot->mutex);
    slot->used = false;
    thread->id = 0;

    return 0;
}

void threadExit(void)
{
    pthread_exit(NULL);
}

int threadTlsAlloc(void (*destructor)(void *))
{
    int result = -1;

    pthread_mutex_lock(&g_table_lock);
    for (size_t i = 0; i < HOST_MAX_TLS; i++) {
        if (!g_tls_used[i]) {
            if (pthread_key_create(&g_tls_keys[i], destructor) == 0) {
                g_tls_used[i] = true;
                result = (int)i + 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_table_lock);

    return result;
}

void threadTlsFree(int slot)
{
    if (slot <= 0 || slot > HOST_MAX_TLS)
        return;

    pthread_mutex_lock(&g_table_lock);
    if (g_tls_used[slot - 1]) {
        pthread_key_delete(g_tls_keys[slot - 1]);
        g_tls_used[slot - 1] = false;
    }
    pthread_mutex_unlock(&g_table_lock);
}

void threadTlsSet(int slot, void *value)
{
    if (slot <= 0 || slot > HOST_MAX_TLS || !g_tls_used[slot - 1])
        return;

    pthread_setspecific(g_tls_keys[slot - 1], value);
}

void *threadTlsGet(int slot)
{
    if (slot <= 0 || slot > HOST_MAX_TLS || !g_tls_used[slot - 1])
        return NULL;

    return pthread_getspecific(g_tls_keys[slot - 1]);
}

void host_sleep_ns(int64_t nanoseconds)
{
    struct timespec request;
    struct timespec remaining;

    if (nanoseconds <= 0)
        return;

    request.tv_sec = nanoseconds / 1000000000ll;
    request.tv_nsec = nanoseconds % 1000000000ll;

    while (nanosleep(&request, &remaining) == -1 && errno == EINTR)
        request = remaining;
}

uint64_t host_monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void host_debug_string(const char *str, size_t size)
{
    if (!str || size == 0)
        return;

    fwrite(str, 1, size, stderr);
    fflush(stderr);
}
