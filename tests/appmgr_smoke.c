/*
 * Checks the host-backed SceAppMgrUser surface: device capacity queries and
 * the no-event system event queue.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/appmgr.h>

#include "fs.h"

int sceAppMgrGetDevInfo(const char *dev, uint64_t *max_size, uint64_t *free_size);
int sceAppMgrReceiveSystemEvent(SceAppMgrSystemEvent *system_event);
int sceAppMgrUmount(const char *mount_point);
int sceAppMgrLaunchAppByUri(int flags, const char *uri);

int main(void)
{
    char root[] = "/tmp/v2n-appmgr-XXXXXX";
    SceAppMgrSystemEvent event;
    uint64_t max_size = 0;
    uint64_t free_size = 0;

    if (!mkdtemp(root)) {
        puts("appmgr_smoke: mkdtemp failed");
        return 1;
    }
    if (host_fs_init(root, root) != 0) {
        puts("appmgr_smoke: host_fs_init failed");
        return 1;
    }

    if (sceAppMgrGetDevInfo("ux0:", &max_size, &free_size) != 0 || max_size == 0 ||
        free_size > max_size) {
        printf("appmgr_smoke: dev info wrong (max %llu free %llu)\n",
               (unsigned long long)max_size, (unsigned long long)free_size);
        return 1;
    }

    memset(&event, 0xFF, sizeof(event));
    if (sceAppMgrReceiveSystemEvent(&event) != 0 || event.systemEvent != 0) {
        puts("appmgr_smoke: system event should be empty");
        return 1;
    }
    if (sceAppMgrReceiveSystemEvent(NULL) >= 0) {
        puts("appmgr_smoke: null system event accepted");
        return 1;
    }

    if (sceAppMgrUmount("app0:") != 0) {
        puts("appmgr_smoke: umount failed");
        return 1;
    }
    if (sceAppMgrLaunchAppByUri(0x20000, "psgm:play") >= 0) {
        puts("appmgr_smoke: launch should be unsupported");
        return 1;
    }

    rmdir(root);
    puts("appmgr_smoke: ok");
    return 0;
}
