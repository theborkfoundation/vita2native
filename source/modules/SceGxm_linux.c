#include "log.h"
#include "modules/SceGxm.h"

/*
 * GXM command translation is the next graphics milestone. The GLES 3.x device
 * itself already exists (see source/host/host_gfx_gl.c); until the SceGxm entry
 * points are translated, CPU-rendered homebrew can run and 3D programs get the
 * normal unresolved-import diagnostics from the loader.
 */

int SceGxm_init(HostGpuDevice device)
{
    (void)device;
    LOG("SceGxm: GXM command translation is not implemented yet; 3D rendering is disabled");
    return 0;
}

int SceGxm_finish(void)
{
    return 0;
}
