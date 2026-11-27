#ifndef SCE_IO_ASYNC_H
#define SCE_IO_ASYNC_H

#include <psp2/types.h>

/* Drops any recorded result for a descriptor that is going away. */
void SceIoAsync_forget(SceUID fd);

#endif
