/* Diagnostic: try the EGL display types a vendor blob may expect. */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

typedef void *(*gbm_create_device_fn)(int fd);

static void try_display(const char *label, EGLDisplay display, PFNEGLINITIALIZEPROC init,
                        PFNEGLGETERRORPROC get_error, PFNEGLQUERYSTRINGPROC query_string)
{
    EGLint major = 0, minor = 0;

    printf("%s: display=%p\n", label, (void *)display);
    if (display == EGL_NO_DISPLAY) {
        printf("  (no display) err=0x%x\n", get_error());
        return;
    }
    if (!init(display, &major, &minor)) {
        printf("  eglInitialize failed err=0x%x\n", get_error());
        return;
    }
    printf("  ok: EGL %d.%d vendor=%s version=%s\n", major, minor,
           query_string(display, EGL_VENDOR), query_string(display, EGL_VERSION));
    printf("  client apis=%s\n", query_string(display, EGL_CLIENT_APIS));
}

int main(void)
{
    void *egl = dlopen("libEGL.so.1", RTLD_NOW);
    PFNEGLGETDISPLAYPROC get_display;
    PFNEGLINITIALIZEPROC init;
    PFNEGLGETERRORPROC get_error;
    PFNEGLQUERYSTRINGPROC query_string;
    PFNEGLGETPROCADDRESSPROC get_proc;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_ext;
    void *gbm_lib;
    gbm_create_device_fn create_device;
    void *drm = NULL;

    if (!egl) {
        printf("no libEGL.so.1: %s\n", dlerror());
        return 1;
    }

    get_display = (PFNEGLGETDISPLAYPROC)dlsym(egl, "eglGetDisplay");
    init = (PFNEGLINITIALIZEPROC)dlsym(egl, "eglInitialize");
    get_error = (PFNEGLGETERRORPROC)dlsym(egl, "eglGetError");
    query_string = (PFNEGLQUERYSTRINGPROC)dlsym(egl, "eglQueryString");
    get_proc = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
    get_platform_ext = (PFNEGLGETPLATFORMDISPLAYEXTPROC)get_proc("eglGetPlatformDisplayEXT");

    printf("eglGetPlatformDisplayEXT=%p\n", (void *)get_platform_ext);
    printf("client extensions: %s\n", query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS));

    try_display("eglGetDisplay(DEFAULT)", get_display(EGL_DEFAULT_DISPLAY), init, get_error,
                query_string);

    if (get_platform_ext) {
        try_display("PLATFORM_SURFACELESS",
                    get_platform_ext(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL), init,
                    get_error, query_string);
    }

    gbm_lib = dlopen("libgbm.so.1", RTLD_NOW);
    printf("libgbm=%p\n", gbm_lib);
    if (gbm_lib) {
        create_device = (gbm_create_device_fn)dlsym(gbm_lib, "gbm_create_device");
        printf("gbm_create_device=%p\n", (void *)create_device);
        if (create_device) {
            int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

            printf("/dev/dri/card0 fd=%d\n", fd);
            if (fd >= 0)
                drm = create_device(fd);
            printf("gbm device=%p\n", drm);
            if (drm) {
                try_display("eglGetDisplay(gbm)", get_display((EGLNativeDisplayType)drm), init,
                            get_error, query_string);
                if (get_platform_ext) {
                    try_display("PLATFORM_GBM_KHR(gbm)",
                                get_platform_ext(EGL_PLATFORM_GBM_KHR,
                                                 (EGLNativeDisplayType)drm, NULL),
                                init, get_error, query_string);
                }
            }
        }
    }

    return 0;
}
