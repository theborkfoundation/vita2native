#ifndef VITA2NATIVE_HOST_H
#define VITA2NATIVE_HOST_H

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NORETURN
#define NORETURN __attribute__((noreturn))
#endif

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;

typedef int32_t Result;

#define R_FAILED(res)  ((res) < 0)
#define R_SUCCEEDED(res) ((res) >= 0)
#define BIT(n)         (1u << (n))

/*
 * Opaque, value-sized handles. Keeping these at 4 bytes matters because some
 * of them live inside guest-visible work areas (notably SceKernelLwMutexWork,
 * which is 0x20 bytes). The actual pthread objects live in fixed-size tables
 * in source/host/host_thread.c.
 */
typedef struct {
    uint32_t id;
} Thread;

#define HOST_MUTEX_MAGIC 0x4D555458u

typedef struct {
    uint32_t magic;
    uint32_t id;
} Mutex;

typedef struct {
    uint32_t id;
} CondVar;

typedef struct {
    uint32_t id;
} RwLock;

typedef struct {
    uint32_t id;
} UEvent;

typedef struct {
    uint32_t id;
} LEvent;

typedef struct {
    int kind;
    void *object;
} Waiter;

enum {
    HOST_WAITER_THREAD = 0,
    HOST_WAITER_UEVENT = 1,
};

/* Threads. */
Result threadCreate(Thread *thread, void (*entry)(void *), void *arg, void *stack,
                    size_t stackSize, int priority, int core);
Result threadStart(Thread *thread);
Result threadWaitForExit(Thread *thread);
Result threadClose(Thread *thread);
void threadExit(void) NORETURN;

/* Bounds of the calling thread's stack. `base` is the lowest address and
   `size` the usable size; the initial stack pointer is at base + size. */
Result threadGetStackBounds(void **base, size_t *size);

int threadTlsAlloc(void (*destructor)(void *));
void threadTlsFree(int slot);
void threadTlsSet(int slot, void *value);
void *threadTlsGet(int slot);

void host_sleep_ns(int64_t nanoseconds);

/*
 * Video output. SDL is loaded at runtime and owns driver selection: KMSDRM on
 * handhelds, X11/Wayland on desktops, dummy for headless runs
 * (SDL_VIDEODRIVER=dummy). VITA2NATIVE_STRETCH fills the display instead of
 * letterboxing, and VITA2NATIVE_SDL_WINDOWED keeps a window instead of going
 * fullscreen.
 */
typedef struct HostVideo HostVideo;
HostVideo *host_video_open(void);
void host_video_close(HostVideo *video);
int host_video_width(const HostVideo *video);
int host_video_height(const HostVideo *video);
void host_video_present(HostVideo *video, const void *rgba, int width, int height,
                        int pitch_pixels);

/*
 * Presents a GL texture owned by the GPU device directly on the window (X11)
 * or the scanout plane (KMS/DRM). Returns false when direct presentation is
 * unavailable and the caller should fall back to host_video_present().
 */
bool host_video_present_texture(HostVideo *video, unsigned int texture, int width, int height);

/*
 * Pad state sampled by the video layer from SDL (keyboard and game
 * controllers). Buttons use the SCE_CTRL_* encoding and axes are 0..255 with
 * 128 centred. `pressed` holds the button edges accumulated since the previous
 * call, so a quick tap survives the guest's slow polling. `active` says a game
 * controller (not just a keyboard) is attached, which makes the axes
 * meaningful. Returns false when no SDL video/input is running.
 */
typedef struct {
    uint32_t buttons;
    uint32_t pressed;
    uint8_t lx, ly, rx, ry;
    bool active;
} HostPadState;

bool host_video_pad_state(HostPadState *state);

/*
 * Pointer/touch state sampled by the video layer from SDL's mouse state and
 * mapped through the same letterbox rectangle SDL uses to present the guest
 * frame. Coordinates are in the Vita's 1920x1088 touch-panel space. `down` is
 * true while pressed, or for one query after a tap so a slow guest still sees
 * it. Returns false when no SDL video is running.
 */
typedef struct {
    bool active;
    bool down;
    uint16_t x, y;
} HostTouchState;

bool host_video_touch_state(HostTouchState *state);

/* Drive the attached game controller's motors. `small` and `large` are the
   Vita actuator strengths (0..255); the request is applied on the video
   thread, which owns the SDL controller. */
void host_video_rumble(uint8_t small, uint8_t large);

/* Mutexes. */
void mutexInit(Mutex *mutex);
void mutexLock(Mutex *mutex);
bool mutexTryLock(Mutex *mutex);
void mutexUnlock(Mutex *mutex);
void mutexDestroy(Mutex *mutex);

/* Condition variables. */
void condvarInit(CondVar *condvar);
void condvarWait(CondVar *condvar, Mutex *mutex);
int condvarWaitTimeout(CondVar *condvar, Mutex *mutex, uint64_t timeout_ns);
/* Variants for callers that already hold the mutex; it stays held on return. */
void condvarWaitLocked(CondVar *condvar, Mutex *mutex);
int condvarWaitTimeoutLocked(CondVar *condvar, Mutex *mutex, uint64_t timeout_ns);
void condvarWakeAll(CondVar *condvar);
void condvarDestroy(CondVar *condvar);

/* Reader/writer locks. */
void rwlockInit(RwLock *lock);
void rwlockReadLock(RwLock *lock);
void rwlockReadUnlock(RwLock *lock);
void rwlockWriteLock(RwLock *lock);
void rwlockWriteUnlock(RwLock *lock);
void rwlockDestroy(RwLock *lock);

/* Events. */
void ueventCreate(UEvent *event, bool autoclear);
void ueventSignal(UEvent *event);
void ueventClear(UEvent *event);
void ueventDestroy(UEvent *event);

void leventInit(LEvent *event, bool autoclear, bool initial);
bool leventTryWait(LEvent *event);
void leventClear(LEvent *event);
void leventSignal(LEvent *event);
void leventDestroy(LEvent *event);

Waiter waiterForThread(Thread *thread);
Waiter waiterForUEvent(UEvent *event);
Result waitSingle(Waiter waiter, uint64_t timeout_ns);

uint64_t host_monotonic_ns(void);

void host_debug_string(const char *str, size_t size);

#endif
