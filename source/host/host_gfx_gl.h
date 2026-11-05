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
    X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                                                   \
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
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)

typedef struct HostGles {
    void *egl_lib;
    void *gles_lib;

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
    PFNEGLDESTROYSURFACEPROC eglDestroySurface;
    PFNEGLMAKECURRENTPROC eglMakeCurrent;
    PFNEGLRELEASETHREADPROC eglReleaseThread;

#define HOST_GL_FIELD(type, name) type name;
    HOST_GL_FUNCTIONS(HOST_GL_FIELD)
#undef HOST_GL_FIELD

    GLuint color_texture;
    GLuint framebuffer;
} HostGles;

/* Returns the GLES implementation, or NULL when the null backend is active. */
HostGles *host_gpu_get_gles(HostGpuDevice device);

#endif
