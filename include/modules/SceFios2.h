#ifndef SCE_FIOS2_H
#define SCE_FIOS2_H

#include <stddef.h>
#include <stdint.h>

/*
 * User-space SceFios2 ABI.
 *
 * VitaSDK does not ship a public header for this library, so the layouts here
 * follow the definitions used by VitaShell's psarc support (the main public
 * consumer) and the values observed in Vita 3.60 titles.
 */

#define SCE_FIOS2_LIBRARY_NID 0x3AD4D630

typedef int32_t SceFiosFH;
typedef int32_t SceFiosDH;
typedef uint64_t SceFiosDate;
typedef int64_t SceFiosOffset;
typedef int64_t SceFiosSize;

/* SceFiosDate is microseconds since the Unix epoch. */
#define SCE_FIOS_DATE_MICROSECONDS 1000000ull

typedef struct SceFiosBuffer {
    void *pPtr;
    size_t length;
} SceFiosBuffer;

typedef struct SceFiosParams {
    uint32_t initialized : 1;
    uint32_t paramsSize : 15;
    uint32_t pathMax : 16;
    uint32_t profiling;
    uint32_t ioThreadCount;
    uint32_t threadsPerScheduler;
    uint32_t extraFlag1 : 1;
    uint32_t extraFlags : 31;
    uint32_t maxChunk;
    uint8_t maxDecompressorThreadCount;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    intptr_t reserved4;
    intptr_t reserved5;
    SceFiosBuffer opStorage;
    SceFiosBuffer fhStorage;
    SceFiosBuffer dhStorage;
    SceFiosBuffer chunkStorage;
    void *pVprintf;
    void *pMemcpy;
    void *pProfileCallback;
    int threadPriority[3];
    int threadAffinity[3];
    int threadStackSize[3];
} SceFiosParams;

typedef struct SceFiosStat {
    SceFiosOffset fileSize;
    SceFiosDate accessDate;
    SceFiosDate modificationDate;
    SceFiosDate creationDate;
    uint32_t statFlags; /* bit 0: directory */
    uint32_t reserved;
    int64_t uid;
    int64_t gid;
    int64_t dev;
    int64_t ino;
    int64_t mode;
} SceFiosStat;

#define SCE_FIOS_STAT_DIRECTORY 0x1u

typedef struct SceFiosDirEntry {
    SceFiosOffset fileSize;
    uint32_t statFlags;
    uint16_t nameLength;
    uint16_t fullPathLength;
    uint16_t offsetToName;
    uint16_t reserved[3];
    char fullPath[1024];
} SceFiosDirEntry;

typedef struct SceFiosPsarcDearchiverContext {
    size_t sizeOfContext;
    size_t workBufferSize;
    void *pWorkBuffer;
    intptr_t flags;
    intptr_t reserved[3];
} SceFiosPsarcDearchiverContext;

/* Open flags used by SceFiosOpenParams.openFlags. */
#define SCE_FIOS_O_RDONLY 0x00000001
#define SCE_FIOS_O_WRONLY 0x00000002
#define SCE_FIOS_O_RDWR (SCE_FIOS_O_RDONLY | SCE_FIOS_O_WRONLY)
#define SCE_FIOS_O_CREAT 0x00000010
#define SCE_FIOS_O_TRUNC 0x00000020
#define SCE_FIOS_O_APPEND 0x00000040

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
_Static_assert(sizeof(SceFiosStat) == 0x50, "SceFiosStat layout");
#endif

int SceFios2_init(void);
int SceFios2_finish(void);

#endif
