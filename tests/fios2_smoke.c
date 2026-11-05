/*
 * Exercises the synchronous SceFios2 surface against a temporary host root:
 * file create/write/stat/read/seek/rename/delete, directory enumeration, and
 * SceFiosDate <-> SceDateTime conversion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2common/types.h>

#include "fs.h"
#include "modules/SceFios2.h"

int sceFiosInitialize(const SceFiosParams *params);
int sceFiosTerminate(void);
int sceFiosIsInitialized(void);
int sceFiosDirectoryCreateSync(const void *attr, const char *path);
int sceFiosDirectoryDeleteSync(const void *attr, const char *path);
int sceFiosExistsSync(const void *attr, const char *path);
int sceFiosFileExistsSync(const void *attr, const char *path);
int sceFiosDirectoryExistsSync(const void *attr, const char *path);
int sceFiosFileDeleteSync(const void *attr, const char *path);
int sceFiosRenameSync(const void *attr, const char *old_path, const char *new_path);
int sceFiosStatSync(const void *attr, const char *path, SceFiosStat *stat);
SceFiosSize sceFiosFileGetSizeSync(const void *attr, const char *path);
int sceFiosFHOpenSync(const void *attr, SceFiosFH *fh, const char *path, const void *params);
int sceFiosFHCloseSync(const void *attr, SceFiosFH fh);
SceFiosSize sceFiosFHReadSync(const void *attr, SceFiosFH fh, void *data, SceFiosSize size);
SceFiosSize sceFiosFHWriteSync(const void *attr, SceFiosFH fh, const void *data, SceFiosSize size);
SceFiosOffset sceFiosFHSeek(SceFiosFH fh, SceFiosOffset offset, int whence);
SceFiosOffset sceFiosFHTell(SceFiosFH fh);
int sceFiosDHOpenSync(const void *attr, SceFiosDH *dh, const char *path, SceFiosBuffer buffer);
int sceFiosDHReadSync(const void *attr, SceFiosDH dh, SceFiosDirEntry *entry);
int sceFiosDHCloseSync(const void *attr, SceFiosDH dh);
SceFiosDate sceFiosDateGetCurrent(void);
SceFiosDate sceFiosDateFromSceDateTime(const SceDateTime *date);
SceDateTime *sceFiosDateToSceDateTime(SceFiosDate date, SceDateTime *out);

static int fail(const char *what)
{
    printf("fios2_smoke: %s\n", what);
    return 1;
}

int main(void)
{
    char root[] = "/tmp/v2n-fios-XXXXXX";
    SceFiosParams params;
    SceFiosStat stat;
    SceFiosDirEntry entry;
    SceFiosFH fh = -1;
    SceFiosDH dh = -1;
    SceDateTime date_time;
    SceFiosDate now, roundtrip;
    uint32_t open_flags;
    char buffer[64];
    const char *text = "hello world";
    int entries = 0;
    int ret = 1;

    if (!mkdtemp(root)) {
        puts("fios2_smoke: mkdtemp failed");
        return 1;
    }
    if (host_fs_init(root, root) != 0) {
        puts("fios2_smoke: host_fs_init failed");
        return 1;
    }
    SceFios2_init();

    memset(&params, 0, sizeof(params));
    params.paramsSize = sizeof(params);
    params.pathMax = 1024;
    if (sceFiosInitialize(&params) != 0)
        return fail("initialize failed");
    if (!sceFiosIsInitialized())
        return fail("is initialized reported false");

    if (sceFiosDirectoryCreateSync(NULL, "ux0:/fios_test") != 0)
        return fail("directory create failed");
    if (sceFiosDirectoryCreateSync(NULL, "ux0:/fios_test") >= 0)
        return fail("duplicate directory create succeeded");
    if (sceFiosDirectoryExistsSync(NULL, "ux0:/fios_test") != 0)
        return fail("directory does not exist");

    open_flags = SCE_FIOS_O_RDWR | SCE_FIOS_O_CREAT | SCE_FIOS_O_TRUNC;
    if (sceFiosFHOpenSync(NULL, &fh, "ux0:/fios_test/a.txt", &open_flags) != 0)
        return fail("open for write failed");
    if (sceFiosFHWriteSync(NULL, fh, text, (SceFiosSize)strlen(text)) !=
        (SceFiosSize)strlen(text))
        return fail("write failed");
    sceFiosFHCloseSync(NULL, fh);

    if (sceFiosFileExistsSync(NULL, "ux0:/fios_test/a.txt") != 0)
        return fail("file does not exist");
    if (sceFiosDirectoryExistsSync(NULL, "ux0:/fios_test/a.txt") >= 0)
        return fail("file reported as directory");
    if (sceFiosFileGetSizeSync(NULL, "ux0:/fios_test/a.txt") !=
        (SceFiosSize)strlen(text))
        return fail("file size mismatch");

    memset(&stat, 0, sizeof(stat));
    if (sceFiosStatSync(NULL, "ux0:/fios_test/a.txt", &stat) != 0)
        return fail("stat failed");
    if (stat.fileSize != (SceFiosOffset)strlen(text) ||
        (stat.statFlags & SCE_FIOS_STAT_DIRECTORY))
        return fail("stat fields wrong");
    if (stat.modificationDate == 0)
        return fail("stat has no modification date");

    if (sceFiosFHOpenSync(NULL, &fh, "ux0:/fios_test/a.txt", NULL) != 0)
        return fail("open for read failed");
    memset(buffer, 0, sizeof(buffer));
    if (sceFiosFHReadSync(NULL, fh, buffer, 5) != 5 || memcmp(buffer, "hello", 5) != 0)
        return fail("read failed");
    if (sceFiosFHTell(fh) != 5)
        return fail("tell after read wrong");
    if (sceFiosFHSeek(fh, 6, 0) != 6)
        return fail("seek failed");
    memset(buffer, 0, sizeof(buffer));
    if (sceFiosFHReadSync(NULL, fh, buffer, 5) != 5 || memcmp(buffer, "world", 5) != 0)
        return fail("read after seek failed");
    sceFiosFHCloseSync(NULL, fh);

    if (sceFiosRenameSync(NULL, "ux0:/fios_test/a.txt", "ux0:/fios_test/b.txt") != 0)
        return fail("rename failed");
    if (sceFiosExistsSync(NULL, "ux0:/fios_test/a.txt") >= 0)
        return fail("old name still exists");
    if (sceFiosExistsSync(NULL, "ux0:/fios_test/b.txt") != 0)
        return fail("new name missing");

    memset(&dh, 0, sizeof(dh));
    if (sceFiosDHOpenSync(NULL, &dh, "ux0:/fios_test", (SceFiosBuffer){ NULL, 0 }) != 0)
        return fail("directory open failed");
    while (sceFiosDHReadSync(NULL, dh, &entry) >= 0) {
        if (strcmp(entry.fullPath, "ux0:/fios_test/b.txt") != 0)
            return fail("directory entry path wrong");
        if (strcmp(entry.fullPath + entry.offsetToName, "b.txt") != 0)
            return fail("directory entry name offset wrong");
        if (entry.nameLength != 5 || entry.fullPathLength != strlen(entry.fullPath))
            return fail("directory entry lengths wrong");
        if (entry.fileSize != (SceFiosOffset)strlen(text) ||
            (entry.statFlags & SCE_FIOS_STAT_DIRECTORY))
            return fail("directory entry stat wrong");
        entries++;
    }
    if (entries != 1)
        return fail("wrong directory entry count");
    sceFiosDHCloseSync(NULL, dh);

    now = sceFiosDateGetCurrent();
    if (!sceFiosDateToSceDateTime(now, &date_time) || date_time.year < 2020)
        return fail("date conversion failed");
    roundtrip = sceFiosDateFromSceDateTime(&date_time);
    if (roundtrip > now || now - roundtrip > 1000000)
        return fail("date round trip failed");
    if (!sceFiosDateToSceDateTime(0, &date_time) || date_time.year != 1970 ||
        date_time.month != 1 || date_time.day != 1)
        return fail("epoch conversion failed");

    if (sceFiosFileDeleteSync(NULL, "ux0:/fios_test/b.txt") != 0)
        return fail("file delete failed");
    if (sceFiosDirectoryDeleteSync(NULL, "ux0:/fios_test") != 0)
        return fail("directory delete failed");
    if (sceFiosStatSync(NULL, "ux0:/fios_test", &stat) >= 0)
        return fail("stat on deleted directory succeeded");

    sceFiosTerminate();
    SceFios2_finish();
    rmdir(root);
    puts("fios2_smoke: ok");
    ret = 0;
    return ret;
}
