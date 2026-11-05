#ifndef SCE_LIBKERNEL_H
#define SCE_LIBKERNEL_H

#include "host.h"

#define SCE_ERROR_ERRNO_ENODEV 0x80010013
#define SCE_ERROR_ERRNO_EMFILE 0x80010018
#define SCE_ERROR_ERRNO_EBADF  0x80010009
#define SCE_ERROR_ERRNO_EINVAL 0x80010016

int SceLibKernel_init(void);
UEvent *SceLibKernel_get_process_exit_uevent(void);
int SceLibKernel_get_process_exit_res();

/* Helpers for the SQLite VFS, which lives outside this translation unit. */
int SceLibKernel_fd_truncate(SceUID fd, int64_t size);
int SceLibKernel_fd_setlock(SceUID fd, int exclusive, int wait);

#endif
