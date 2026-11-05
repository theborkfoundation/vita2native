/*
 * SceIofilemgr asynchronous entry points.
 *
 * The Vita async API is a queued-I/O interface: the operation call returns
 * immediately and the caller later collects the result. This host performs the
 * operation synchronously and records the result, so every operation is
 * already complete when the caller asks. That preserves the observable
 * ordering and avoids a worker thread while keeping the guest contract.
 *
 * The internal _sceIo* exports are thin wrappers around the public calls; the
 * extra option blocks are ignored, as they are on FIRMWARE 3.60.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/types.h>

#include "modules/SceLibKernel.h"
#include "module.h"

/* Not exposed by vita-headers, but exported by SceIofilemgr. */
int sceIoChstatByFd(SceUID fd, const SceIoStat *stat, unsigned int bits);
int sceIoGetstat(const char *file, SceIoStat *st);
int sceIoGetstatByFd(SceUID fd, SceIoStat *st);
int sceIoChstat(const char *file, SceIoStat *st, int bits);
int sceIoMkdir(const char *dir, SceMode mode);
int sceIoRmdir(const char *dir);

#define ASYNC_SLOTS 128

typedef struct {
    SceUID fd;
    int result;
    bool used;
} AsyncResult;

static AsyncResult g_async[ASYNC_SLOTS];
static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;

static void async_store(SceUID fd, int result)
{
    pthread_mutex_lock(&g_async_lock);

    for (int i = 0; i < ASYNC_SLOTS; i++) {
        if (g_async[i].used && g_async[i].fd == fd) {
            g_async[i].result = result;
            pthread_mutex_unlock(&g_async_lock);
            return;
        }
    }

    for (int i = 0; i < ASYNC_SLOTS; i++) {
        if (!g_async[i].used) {
            g_async[i].used = true;
            g_async[i].fd = fd;
            g_async[i].result = result;
            break;
        }
    }

    pthread_mutex_unlock(&g_async_lock);
}

static int async_load(SceUID fd, int *result)
{
    int found = 0;

    pthread_mutex_lock(&g_async_lock);
    for (int i = 0; i < ASYNC_SLOTS; i++) {
        if (g_async[i].used && g_async[i].fd == fd) {
            *result = g_async[i].result;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_async_lock);

    return found;
}

static void async_forget(SceUID fd)
{
    pthread_mutex_lock(&g_async_lock);
    for (int i = 0; i < ASYNC_SLOTS; i++) {
        if (g_async[i].used && g_async[i].fd == fd) {
            g_async[i].used = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_async_lock);
}

/* ------------------------------------------------------------------------- */
/* Public async API                                                          */
/* ------------------------------------------------------------------------- */

EXPORT(SceLibKernel, 0x6A7EA9FD, SceUID, sceIoOpenAsync, const char *file, int flags,
       SceMode mode)
{
    SceUID fd = sceIoOpen(file, flags, mode);

    if (fd >= 0)
        async_store(fd, 0);

    return fd;
}

EXPORT(SceIofilemgr, 0x8EA3616A, int, sceIoCloseAsync, SceUID fd)
{
    int result = sceIoClose(fd);

    async_forget(fd);
    return result;
}

EXPORT(SceIofilemgr, 0x773EBD45, int, sceIoReadAsync, SceUID fd, void *buf, SceSize nbyte)
{
    int result = sceIoRead(fd, buf, nbyte);

    async_store(fd, result);
    return 0;
}

EXPORT(SceIofilemgr, 0xE0D63D2A, int, sceIoWriteAsync, SceUID fd, const void *buf, SceSize nbyte)
{
    int result = sceIoWrite(fd, buf, nbyte);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0xCAC5D672, int, sceIoLseekAsync, SceUID fd, SceOff offset, int whence)
{
    SceOff result = sceIoLseek(fd, offset, whence);

    async_store(fd, (int)result);
    return 0;
}

EXPORT(SceLibKernel, 0xA010141E, int, sceIoPreadAsync, SceUID fd, void *buf, SceSize nbyte,
       SceOff offset)
{
    int result = sceIoPread(fd, buf, nbyte, offset);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0xED25BEEF, int, sceIoPwriteAsync, SceUID fd, const void *buf, SceSize nbyte,
       SceOff offset)
{
    int result = sceIoPwrite(fd, buf, nbyte, offset);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0x82B20B41, int, sceIoGetstatAsync, const char *file, SceIoStat *stat)
{
    int result = sceIoGetstat(file, stat);

    async_store(0, result);
    return 0;
}

EXPORT(SceIofilemgr, 0x5167AC1E, int, sceIoGetstatByFdAsync, SceUID fd, SceIoStat *stat)
{
    int result = sceIoGetstatByFd(fd, stat);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0x9739A5E2, int, sceIoChstatAsync, const char *file, SceIoStat *stat,
       int bits)
{
    int result = sceIoChstat(file, stat, bits);

    async_store(0, result);
    return 0;
}

EXPORT(SceIofilemgr, 0xA9F89275, int, sceIoChstatByFdAsync, SceUID fd, const SceIoStat *stat,
       unsigned int bits)
{
    int result = sceIoChstatByFd(fd, stat, bits);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0x8E5FCBB1, int, sceIoMkdirAsync, const char *dir, SceMode mode)
{
    int result = sceIoMkdir(dir, mode);

    async_store(0, result);
    return 0;
}

EXPORT(SceLibKernel, 0x9694D00F, int, sceIoRmdirAsync, const char *dir)
{
    int result = sceIoRmdir(dir);

    async_store(0, result);
    return 0;
}

EXPORT(SceLibKernel, 0x446A60AC, int, sceIoRemoveAsync, const char *file)
{
    int result = sceIoRemove(file);

    async_store(0, result);
    return 0;
}

EXPORT(SceLibKernel, 0xEE9857CD, int, sceIoRenameAsync, const char *old_path, const char *new_path)
{
    int result = sceIoRename(old_path, new_path);

    async_store(0, result);
    return 0;
}

EXPORT(SceLibKernel, 0xF7C7FBFE, int, sceIoSyncAsync, const char *device, unsigned int unk)
{
    int result = sceIoSync(device, unk);

    async_store(0, result);
    return 0;
}

EXPORT(SceIofilemgr, 0x7E1367CB, int, sceIoSyncByFdAsync, SceUID fd, int flag)
{
    int result = sceIoSyncByFd(fd, flag);

    async_store(fd, result);
    return 0;
}

EXPORT(SceIofilemgr, 0xC49C312F, SceUID, sceIoDopenAsync, const char *dirname)
{
    SceUID fd = sceIoDopen(dirname);

    if (fd >= 0)
        async_store(fd, 0);

    return fd;
}

EXPORT(SceIofilemgr, 0xF59F37B0, int, sceIoDreadAsync, SceUID fd, SceIoDirent *dirent)
{
    int result = sceIoDread(fd, dirent);

    async_store(fd, result);
    return 0;
}

EXPORT(SceIofilemgr, 0xDC2D7D38, int, sceIoDcloseAsync, SceUID fd)
{
    int result = sceIoDclose(fd);

    async_forget(fd);
    return result;
}

EXPORT(SceLibKernel, 0x099C54B9, int, sceIoIoctlAsync, SceUID fd, unsigned int cmd, void *indata,
       int inlen, void *outdata, int outlen)
{
    int result = sceIoIoctl(fd, cmd, indata, inlen, outdata, outlen);

    async_store(fd, result);
    return 0;
}

EXPORT(SceLibKernel, 0x950F78EB, int, sceIoDevctlAsync, const char *dev, unsigned int cmd,
       void *indata, int inlen, void *outdata, int outlen)
{
    int result = sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen);

    async_store(0, result);
    return 0;
}

EXPORT(SceIofilemgr, 0xD1C49D2F, int, sceIoComplete, SceUID fd)
{
    int result = 0;

    (void)async_load(fd, &result);
    return result;
}

EXPORT(SceIofilemgr, 0xCEF48835, int, sceIoCancel, SceUID fd)
{
    (void)fd;
    return 0;
}

EXPORT(SceLibKernel, 0xA792C404, int, sceIoCompleteMultiple, void)
{
    /* Every operation is complete before the call returns. */
    return 0;
}

EXPORT(SceIofilemgr, 0x9111D004, int, _sceIoCompleteMultiple, void)
{
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Internal _sceIo* entry points                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    SceOff offset;
    int32_t whence;
    uint32_t pad;
} SceIoLseekOpt;

typedef struct {
    void *buf;
    SceSize nbyte;
    SceOff offset;
} SceIoPreadArgs;

typedef struct {
    SceSize arglen;
    void *bufp;
    SceSize buflen;
    uint32_t reserved[3];
} SceIoDevctlOpt;

EXPORT(SceIofilemgr, 0xCC67B6FD, SceUID, _sceIoOpen, const char *filename, int flags,
       SceMode mode, const void *opt)
{
    (void)opt;
    return sceIoOpen(filename, flags, mode);
}

EXPORT(SceIofilemgr, 0x09CD0FC8, SceUID, _sceIoOpenAsync, const char *filename, int flags,
       SceMode mode, const void *opt)
{
    (void)opt;
    return sceIoOpenAsync(filename, flags, mode);
}

EXPORT(SceIofilemgr, 0xE6E614B5, SceUID, _sceIoDopen, const char *dirname, const void *opt)
{
    (void)opt;
    return sceIoDopen(dirname);
}

EXPORT(SceIofilemgr, 0x8713D662, int, _sceIoDread, SceUID fd, SceIoDirent *dirent)
{
    return sceIoDread(fd, dirent);
}

EXPORT(SceIofilemgr, 0x8E7E11F2, int, _sceIoGetstat, const char *name, SceIoStat *stat,
       const void *opt)
{
    (void)opt;
    return sceIoGetstat(name, stat);
}

EXPORT(SceIofilemgr, 0xD414C89F, int, _sceIoGetstatAsync, const char *name, SceIoStat *stat)
{
    return sceIoGetstatAsync(name, stat);
}

EXPORT(SceIofilemgr, 0xE6C53567, int, _sceIoGetstatByFd, SceUID fd, SceIoStat *stat)
{
    return sceIoGetstatByFd(fd, stat);
}

EXPORT(SceIofilemgr, 0x8F1ACC32, int, _sceIoMkdir, const char *dirname, SceMode mode,
       const void *opt)
{
    (void)opt;
    return sceIoMkdir(dirname, mode);
}

EXPORT(SceIofilemgr, 0xF5C58B21, int, _sceIoMkdirAsync, const char *dirname, SceMode mode)
{
    return sceIoMkdirAsync(dirname, mode);
}

EXPORT(SceIofilemgr, 0xFFFB4D76, int, _sceIoRmdir, const char *dirname, const void *opt)
{
    (void)opt;
    return sceIoRmdir(dirname);
}

EXPORT(SceIofilemgr, 0x13DC3244, int, _sceIoRmdirAsync, const char *dirname)
{
    return sceIoRmdirAsync(dirname);
}

EXPORT(SceIofilemgr, 0x78955C65, int, _sceIoRemove, const char *filename, const void *opt)
{
    (void)opt;
    return sceIoRemove(filename);
}

EXPORT(SceIofilemgr, 0x5FFA47E2, int, _sceIoRemoveAsync, const char *filename)
{
    return sceIoRemoveAsync(filename);
}

EXPORT(SceIofilemgr, 0x4912F748, int, _sceIoRename, const char *old_name, const char *new_name,
       const void *opt)
{
    (void)opt;
    return sceIoRename(old_name, new_name);
}

EXPORT(SceIofilemgr, 0x81794921, int, _sceIoRenameAsync, const char *old_name,
       const char *new_name)
{
    return sceIoRenameAsync(old_name, new_name);
}

EXPORT(SceIofilemgr, 0x5DD867F7, int, _sceIoSync, const char *device, unsigned int unk,
       const void *opt)
{
    (void)opt;
    return sceIoSync(device, unk);
}

EXPORT(SceIofilemgr, 0x86DB0C0E, int, _sceIoSyncAsync, const char *device, unsigned int unk)
{
    return sceIoSyncAsync(device, unk);
}

EXPORT(SceIofilemgr, 0xD2EE455F, int, _sceIoChstat, const char *name, const SceIoStat *stat,
       unsigned int bits, const void *opt)
{
    (void)opt;
    return sceIoChstat(name, (SceIoStat *)stat, bits);
}

EXPORT(SceIofilemgr, 0xB4B021D9, int, _sceIoChstatAsync, const char *name, const SceIoStat *stat,
       unsigned int bits)
{
    return sceIoChstatAsync(name, (SceIoStat *)stat, bits);
}

EXPORT(SceIofilemgr, 0xE0BE2A30, int, _sceIoChstatByFd, SceUID fd, const SceIoStat *stat,
       unsigned int bits)
{
    return sceIoChstatByFd(fd, stat, bits);
}

EXPORT(SceIofilemgr, 0xA604764A, SceOff, _sceIoLseek, SceUID fd, const SceIoLseekOpt *opt)
{
    if (!opt)
        return SCE_ERROR_ERRNO_EINVAL;

    return sceIoLseek(fd, opt->offset, opt->whence);
}

EXPORT(SceIofilemgr, 0x2300858E, int, _sceIoLseekAsync, SceUID fd, SceOff offset, int whence)
{
    return sceIoLseekAsync(fd, offset, whence);
}

EXPORT(SceIofilemgr, 0x539FD5C4, SceSSize, _sceIoPread, SceUID fd, const SceIoPreadArgs *args)
{
    if (!args)
        return SCE_ERROR_ERRNO_EINVAL;

    return sceIoPread(fd, args->buf, args->nbyte, args->offset);
}

EXPORT(SceIofilemgr, 0xBCF5684D, int, _sceIoPreadAsync, SceUID fd, void *buf, SceSize nbyte,
       SceOff offset)
{
    return sceIoPreadAsync(fd, buf, nbyte, offset);
}

EXPORT(SceIofilemgr, 0x9654094B, SceSSize, _sceIoPwrite, SceUID fd, const SceIoPreadArgs *args)
{
    if (!args)
        return SCE_ERROR_ERRNO_EINVAL;

    return sceIoPwrite(fd, args->buf, args->nbyte, args->offset);
}

EXPORT(SceIofilemgr, 0xB2D0B2F4, int, _sceIoPwriteAsync, SceUID fd, const void *buf, SceSize nbyte,
       SceOff offset)
{
    return sceIoPwriteAsync(fd, buf, nbyte, offset);
}

EXPORT(SceIofilemgr, 0x1D2988F1, int, _sceIoIoctl, SceUID fd, unsigned int cmd, void *indata,
       int inlen, void *outdata, int outlen)
{
    return sceIoIoctl(fd, cmd, indata, inlen, outdata, outlen);
}

EXPORT(SceIofilemgr, 0xE00DC256, int, _sceIoIoctlAsync, SceUID fd, unsigned int cmd, void *indata,
       int inlen, void *outdata, int outlen)
{
    return sceIoIoctlAsync(fd, cmd, indata, inlen, outdata, outlen);
}

EXPORT(SceIofilemgr, 0x515AC017, int, _sceIoDevctl, const char *dev, int cmd, const void *indata,
       const SceIoDevctlOpt *opt)
{
    if (!opt && !indata)
        return sceIoDevctl(dev, (unsigned int)cmd, NULL, 0, NULL, 0);

    return sceIoDevctl(dev, (unsigned int)cmd, (void *)indata,
                       opt ? (int)opt->arglen : 0, opt ? opt->bufp : NULL,
                       opt ? (int)opt->buflen : 0);
}

EXPORT(SceIofilemgr, 0x3EE3F66E, int, _sceIoDevctlAsync, const char *dev, int cmd,
       const void *indata, const SceIoDevctlOpt *opt)
{
    if (!opt && !indata)
        return sceIoDevctlAsync(dev, (unsigned int)cmd, NULL, 0, NULL, 0);

    return sceIoDevctlAsync(dev, (unsigned int)cmd, (void *)indata,
                            opt ? (int)opt->arglen : 0, opt ? opt->bufp : NULL,
                            opt ? (int)opt->buflen : 0);
}

EXPORT(SceIofilemgr, 0xF2A472A1, int, sceIoGetPriority, SceUID fd)
{
    (void)fd;
    return 0;
}

EXPORT(SceIofilemgr, 0xEF5432ED, int, sceIoGetPriorityForSystem, SceUID fd)
{
    (void)fd;
    return 0;
}

EXPORT(SceIofilemgr, 0x0DC4F1BB, int, sceIoGetProcessDefaultPriority, void)
{
    return 0;
}

EXPORT(SceIofilemgr, 0xA176CD03, int, sceIoGetThreadDefaultPriority, void)
{
    return 0;
}

EXPORT(SceIofilemgr, 0xFCBCEAED, int, sceIoGetThreadDefaultPriorityForSystem, void)
{
    return 0;
}

EXPORT(SceIofilemgr, 0x14B2D56C, int, sceIoSetPriority, SceUID fd, int priority)
{
    (void)fd;
    (void)priority;
    return 0;
}

EXPORT(SceIofilemgr, 0x27373135, int, sceIoSetPriorityForSystem, SceUID fd, int priority)
{
    (void)fd;
    (void)priority;
    return 0;
}

EXPORT(SceIofilemgr, 0x7F2ABBAF, int, sceIoSetProcessDefaultPriority, int priority)
{
    (void)priority;
    return 0;
}

EXPORT(SceIofilemgr, 0x49312108, int, sceIoSetThreadDefaultPriority, int priority)
{
    (void)priority;
    return 0;
}

EXPORT(SceIofilemgr, 0xB9C9C9CF, int, sceIoSetThreadDefaultPriorityForSystem, int priority)
{
    (void)priority;
    return 0;
}

EXPORT(SceIofilemgr, 0x3E98E422, int, sceIoFlockForSystem, SceUID fd, int operation)
{
    (void)fd;
    (void)operation;
    return 0;
}

EXPORT(SceLibKernel, 0xF5C6F098, int, sceIoClose2, SceUID fd, int flags)
{
    (void)flags;
    return sceIoClose(fd);
}

EXPORT(SceLibKernel, 0x713523E1, SceSSize, sceIoRead2, SceUID fd, void *buf, SceSize nbyte,
       int flags)
{
    (void)flags;
    return sceIoRead(fd, buf, nbyte);
}

EXPORT(SceLibKernel, 0x11FED231, SceSSize, sceIoWrite2, SceUID fd, const void *buf, SceSize nbyte,
       int flags)
{
    (void)flags;
    return sceIoWrite(fd, buf, nbyte);
}
