#include <psp2/ctrl.h>

#include "host.h"
#include "module.h"
#include "util.h"

EXPORT(SceCtrl, 0xA9C3CED6, int, sceCtrlPeekBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    (void)port;
    (void)count;

    if (!pad_data)
        return -1;

    pad_data->timeStamp = host_monotonic_ns() / 1000;
    pad_data->buttons = 0;
    pad_data->lx = 128;
    pad_data->ly = 128;
    pad_data->rx = 128;
    pad_data->ry = 128;

    return 0;
}

DECLARE_LIBRARY(SceCtrl, 0xD197E3C7);

int SceCtrl_init(void)
{
    return 0;
}
