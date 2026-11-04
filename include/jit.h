#ifndef VITA2NATIVE_JIT_H
#define VITA2NATIVE_JIT_H

#include <stdbool.h>
#include <stddef.h>

#include "host.h"

#if defined(VITA2NATIVE_LINUX)

typedef struct Jit {
    void *rw;
    void *rx;
    size_t size;
    int fd;
    bool dual_mapped;
    bool executable;
} Jit;

Result jitCreate(Jit *jit, size_t size);
void *jitGetRwAddr(Jit *jit);
void *jitGetRxAddr(Jit *jit);
Result jitTransitionToWritable(Jit *jit);
Result jitTransitionToExecutable(Jit *jit);
Result jitClose(Jit *jit);

#else

#include <switch.h>

#endif

#endif
