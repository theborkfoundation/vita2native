#include "log.h"
#include "modules/SceGxm.h"

/*
 * The GLES backend is the next milestone. Until it lands, CPU-rendered
 * PlayStation Vita homebrew can run; any program that imports SceGxm will get
 * the normal unresolved-import diagnostics from the loader.
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
