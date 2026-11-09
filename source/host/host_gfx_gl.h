#ifndef VITA2NATIVE_HOST_GFX_GL_H
#define VITA2NATIVE_HOST_GFX_GL_H

#define EGL_NO_PLATFORM_SPECIFIC_TYPES 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "gfx.h"

/*
 * Everything the GXM backend needs from the GLES device. The function table is
 * deliberately public inside the host layer: SceGxm performs its own state
 * translation and buffer/texture management, and going through a wrapper for
 * every GL call would add a lot of indirection for no benefit.
 */
#define HOST_GL_FUNCTIONS(X)                                                                       \
    X(PFNGLGETSTRINGPROC, glGetString)                                                             \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                                         \
    X(PFNGLGETERRORPROC, glGetError)                                                               \
    X(PFNGLISENABLEDPROC, glIsEnabled)                                                             \
    X(PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform)                                               \
    X(PFNGLGETUNIFORMIVPROC, glGetUniformiv)                                                       \
    X(PFNGLCLEARCOLORPROC, glClearColor)                                                           \
    X(PFNGLCLEARPROC, glClear)                                                                     \
    X(PFNGLREADPIXELSPROC, glReadPixels)                                                           \
    X(PFNGLFINISHPROC, glFinish)                                                                   \
    X(PFNGLVIEWPORTPROC, glViewport)                                                               \
    X(PFNGLSCISSORPROC, glScissor)                                                                 \
    X(PFNGLENABLEPROC, glEnable)                                                                   \
    X(PFNGLDISABLEPROC, glDisable)                                                                 \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                         \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                         \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                   \
    X(PFNGLBINDTEXTUREPROC, glBindTexture)                                                         \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                                                           \
    X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D)                                                     \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                                                     \
    X(PFNGLTEXPARAMETERFPROC, glTexParameterf)                                                     \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                                                     \
    X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                                                   \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                                                 \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                           \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                 \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                       \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)                                 \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)                                   \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers)                                               \
    X(PFNGLDELETERENDERBUFFERSPROC, glDeleteRenderbuffers)                                         \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                               \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage)                                         \
    X(PFNGLGENBUFFERSPROC, glGenBuffers)                                                           \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                     \
    X(PFNGLBINDBUFFERPROC, glBindBuffer)                                                           \
    X(PFNGLBUFFERDATAPROC, glBufferData)                                                           \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                                                     \
    X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange)                                                   \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)                                                         \
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                                   \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                                 \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)                                 \
    X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray)                               \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                                         \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                       \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                       \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                     \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                         \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                               \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                     \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                       \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                         \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                       \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                             \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                           \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                       \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                     \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                           \
    X(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex)                                       \
    X(PFNGLUNIFORMBLOCKBINDINGPROC, glUniformBlockBinding)                                         \
    X(PFNGLUNIFORM1IPROC, glUniform1i)                                                             \
    X(PFNGLUNIFORM1FPROC, glUniform1f)                                                             \
    X(PFNGLUNIFORM4FVPROC, glUniform4fv)                                                           \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                                               \
    X(PFNGLDRAWELEMENTSPROC, glDrawElements)                                                       \
    X(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced)                                                       \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                           \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                             \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                             \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                                                     \
    X(PFNGLLINEWIDTHPROC, glLineWidth)                                                             \
    X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                                             \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)                                     \
    X(PFNGLCOLORMASKPROC, glColorMask)                                                             \
    X(PFNGLCULLFACEPROC, glCullFace)                                                               \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                                             \
    X(PFNGLSTENCILFUNCSEPARATEPROC, glStencilFuncSeparate)                                         \
    X(PFNGLSTENCILOPSEPARATEPROC, glStencilOpSeparate)                                             \
    X(PFNGLSTENCILMASKSEPARATEPROC, glStencilMaskSeparate)                                         \
    X(PFNGLCLEARDEPTHFPROC, glClearDepthf)                                                         \
    X(PFNGLCLEARSTENCILPROC, glClearStencil)                                                       \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                                                 \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)                   \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                                                  \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                  \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)

typedef struct HostGles {
    void *egl_lib;
    void *gles_lib;
    void *gbm_lib;
    void *gbm_device;
    int drm_fd;
    bool owns_gbm_device;
    bool owns_drm_fd;
    bool display_is_gbm;

    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    int width;
    int height;

    PFNEGLGETDISPLAYPROC eglGetDisplay;
    PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplay;
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLGETPROCADDRESSPROC eglGetProcAddress;
    PFNEGLINITIALIZEPROC eglInitialize;
    PFNEGLTERMINATEPROC eglTerminate;
    PFNEGLGETERRORPROC eglGetError;
    PFNEGLBINDAPIPROC eglBindAPI;
    PFNEGLCHOOSECONFIGPROC eglChooseConfig;
    PFNEGLCREATECONTEXTPROC eglCreateContext;
    PFNEGLDESTROYCONTEXTPROC eglDestroyContext;
    PFNEGLCREATEPBUFFERSURFACEPROC eglCreatePbufferSurface;
    PFNEGLCREATEWINDOWSURFACEPROC eglCreateWindowSurface;
    PFNEGLSWAPBUFFERSPROC eglSwapBuffers;
    PFNEGLQUERYSURFACEPROC eglQuerySurface;
    PFNEGLSWAPINTERVALPROC eglSwapInterval;
    PFNEGLDESTROYSURFACEPROC eglDestroySurface;
    PFNEGLMAKECURRENTPROC eglMakeCurrent;
    PFNEGLRELEASETHREADPROC eglReleaseThread;

    EGLConfig egl_config;
    bool window_capable;
    EGLSurface window_surface;

    /* KMS/GBM presentation. The surface is created lazily on the presenting
       thread; kms_state holds the private libdrm/GBM state. */
    void *gbm_surface;
    EGLSurface kms_surface;
    int kms_width;
    int kms_height;
    int kms_drm_fd;
    uint32_t kms_crtc_id;
    void *kms_state;
    GLuint present_program;
    GLuint present_texture;
    GLuint overlay_texture;
    GLuint present_vao;
    GLint present_flip_uniform;
    int present_tex_width;
    int present_tex_height;
    int overlay_width;
    int overlay_height;

#define HOST_GL_FIELD(type, name) type name;
    HOST_GL_FUNCTIONS(HOST_GL_FIELD)
#undef HOST_GL_FIELD

    GLuint color_texture;
    GLuint framebuffer;

    /* Contexts created for guest threads other than the creating one. */
    EGLContext *thread_contexts;
    EGLSurface *thread_surfaces;
    unsigned int thread_context_count;
} HostGles;

/* Returns the GLES implementation, or NULL when the null backend is active. */
HostGles *host_gpu_get_gles(HostGpuDevice device);

#endif
