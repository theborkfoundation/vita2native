/*
 * SceGxm on GLES 3.x.
 *
 * The guest renders into a GLES framebuffer owned by SceGxmRenderTarget. GXP
 * shaders are compiled through Vita3K's recompiler (GXP -> SPIR-V) and
 * SPIRV-Cross (SPIR-V -> GLSL ES 3.10). Default uniforms are fed through the
 * same storage-buffer bindings Vita3K's SPIR-V uses (0 = vertex, 1 = fragment),
 * render info through uniform buffers 2/3, and textures through sampler
 * bindings equal to the GXM texture unit.
 *
 * At sceGxmEndScene the color target is read back into the guest's
 * SceGxmColorSurface, which is what sceDisplaySetFrameBuf presents.
 */
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gxm/util.h"
#include "host_gfx_gl.h"
#include "log.h"
#include "module.h"
#include "modules/SceSysmem.h"
#include "spirv_cross_glsl_es.h"
#include "vita3k_shader_recompiler_iface_c.h"

extern int vita3k_gxm_get_attribute_location(const SceGxmProgram *program, uint16_t reg_index);

#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif

#define GXM_MAX_LINKED_PROGRAMS 128
#define GXM_UNIFORM_ARENA_SIZE  (64 * 1024)
#define GXM_NOTIFICATIONS       512

struct GXMRenderVertUniformBlock {
    float viewport_flip[4];
    float viewport_flag;
    float screen_width;
    float screen_height;
    float z_offset;
    float z_scale;
};

struct GXMRenderFragUniformBlock {
    float back_disabled;
    float front_disabled;
    float writing_mask;
    float use_raw_image;
    float res_multiplier;
};

typedef struct {
    GLuint vbo;
    const void *data;
    uint32_t size;
} GxmStream;

typedef struct {
    GLuint tex;
    const void *data;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} GxmTextureCache;

typedef struct SceGxmContext {
    SceGxmContextParams params;
    bool in_scene;

    const SceGxmRenderTarget *render_target;
    const SceGxmColorSurfaceInner *color_surface;
    const SceGxmDepthStencilSurface *ds_surface;
    SceGxmSyncObject *fragment_sync;

    const SceGxmVertexProgram *vertex_program;
    const SceGxmFragmentProgram *fragment_program;

    GxmStream streams[SCE_GXM_MAX_VERTEX_STREAMS];
    SceGxmTextureInner fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS];

    void *vertex_uniforms;
    void *fragment_uniforms;
    uint32_t vertex_uniform_size;
    uint32_t fragment_uniform_size;

    uint8_t *vertex_uniform_arena;
    uint8_t *fragment_uniform_arena;
    uint32_t vertex_uniform_head;
    uint32_t fragment_uniform_head;

    SceGxmDepthFunc depth_func;
    SceGxmDepthWriteMode depth_write;
    SceGxmStencilFunc front_stencil_func;
    SceGxmStencilOp front_stencil_fail, front_stencil_depth_fail, front_stencil_pass;
    uint8_t front_stencil_compare_mask, front_stencil_write_mask;
    SceGxmStencilFunc back_stencil_func;
    SceGxmStencilOp back_stencil_fail, back_stencil_depth_fail, back_stencil_pass;
    uint8_t back_stencil_compare_mask, back_stencil_write_mask;
    unsigned int front_stencil_ref, back_stencil_ref;

    GLuint ssbo[2];
    GLuint ubo[2];
    GLuint vbo;
    GLuint ibo;
    GxmTextureCache textures[SCE_GXM_MAX_TEXTURE_UNITS];
} SceGxmContext;

typedef struct SceGxmSyncObject {
    int unused;
} SceGxmSyncObject;

typedef struct SceGxmRegisteredProgram {
    const SceGxmProgram *programHeader;
} SceGxmRegisteredProgram;

typedef struct SceGxmShaderPatcher {
    SceGxmShaderPatcherParams params;
    SceGxmShaderPatcherId *registered_programs;
    uint32_t registered_count;
} SceGxmShaderPatcher;

typedef struct SceGxmVertexProgram {
    SceGxmShaderPatcherId programId;
    SceGxmVertexAttribute *attributes;
    unsigned int attributeCount;
    SceGxmVertexStream *streams;
    unsigned int streamCount;
    GLuint shader;
} SceGxmVertexProgram;

typedef struct SceGxmFragmentProgram {
    SceGxmShaderPatcherId programId;
    SceGxmOutputRegisterFormat outputFormat;
    SceGxmMultisampleMode multisampleMode;
    SceGxmBlendInfo blendInfo;
    GLuint shader;
} SceGxmFragmentProgram;

typedef struct SceGxmRenderTarget {
    SceGxmRenderTargetParams params;
    GLuint fbo;
    GLuint color;
    GLuint depth;
} SceGxmRenderTarget;

typedef struct {
    const SceGxmVertexProgram *vs;
    const SceGxmFragmentProgram *fs;
    GLuint program;
} GxmLinkedProgram;

static bool g_gxm_initialized;
static HostGpuDevice g_device;
static HostGles *g_gl;
static uint32_t *g_notification_region;
static GxmLinkedProgram g_linked[GXM_MAX_LINKED_PROGRAMS];
static unsigned int g_linked_count;
static SceGxmDisplayQueueCallback *g_display_callback;
static uint32_t g_display_callback_data_size;

static GLenum gxm_primitive_to_gl(SceGxmPrimitiveType type)
{
    switch (type) {
    case SCE_GXM_PRIMITIVE_POINTS: return GL_POINTS;
    case SCE_GXM_PRIMITIVE_LINES: return GL_LINES;
    case SCE_GXM_PRIMITIVE_TRIANGLES: return GL_TRIANGLES;
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

static GLenum gxm_depth_func_to_gl(SceGxmDepthFunc func)
{
    switch (func) {
    case SCE_GXM_DEPTH_FUNC_NEVER: return GL_NEVER;
    case SCE_GXM_DEPTH_FUNC_LESS: return GL_LESS;
    case SCE_GXM_DEPTH_FUNC_EQUAL: return GL_EQUAL;
    case SCE_GXM_DEPTH_FUNC_LESS_EQUAL: return GL_LEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER: return GL_GREATER;
    case SCE_GXM_DEPTH_FUNC_NOT_EQUAL: return GL_NOTEQUAL;
    case SCE_GXM_DEPTH_FUNC_GREATER_EQUAL: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static GLenum gxm_stencil_func_to_gl(SceGxmStencilFunc func)
{
    switch (func) {
    case SCE_GXM_STENCIL_FUNC_NEVER: return GL_NEVER;
    case SCE_GXM_STENCIL_FUNC_LESS: return GL_LESS;
    case SCE_GXM_STENCIL_FUNC_EQUAL: return GL_EQUAL;
    case SCE_GXM_STENCIL_FUNC_LESS_EQUAL: return GL_LEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER: return GL_GREATER;
    case SCE_GXM_STENCIL_FUNC_NOT_EQUAL: return GL_NOTEQUAL;
    case SCE_GXM_STENCIL_FUNC_GREATER_EQUAL: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static GLenum gxm_stencil_op_to_gl(SceGxmStencilOp op)
{
    switch (op) {
    case SCE_GXM_STENCIL_OP_ZERO: return GL_ZERO;
    case SCE_GXM_STENCIL_OP_REPLACE: return GL_REPLACE;
    case SCE_GXM_STENCIL_OP_INVERT: return GL_INVERT;
    case SCE_GXM_STENCIL_OP_INCR: return GL_INCR;
    case SCE_GXM_STENCIL_OP_DECR: return GL_DECR;
    case SCE_GXM_STENCIL_OP_INCR_WRAP: return GL_INCR_WRAP;
    case SCE_GXM_STENCIL_OP_DECR_WRAP: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

static GLenum gxm_blend_factor_to_gl(SceGxmBlendFactor factor)
{
    switch (factor) {
    case SCE_GXM_BLEND_FACTOR_ZERO: return GL_ZERO;
    case SCE_GXM_BLEND_FACTOR_ONE: return GL_ONE;
    case SCE_GXM_BLEND_FACTOR_SRC_COLOR: return GL_SRC_COLOR;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return GL_ONE_MINUS_SRC_COLOR;
    case SCE_GXM_BLEND_FACTOR_SRC_ALPHA: return GL_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case SCE_GXM_BLEND_FACTOR_DST_COLOR: return GL_DST_COLOR;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return GL_ONE_MINUS_DST_COLOR;
    case SCE_GXM_BLEND_FACTOR_DST_ALPHA: return GL_DST_ALPHA;
    case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    default: return GL_ONE;
    }
}

static GLenum gxm_blend_func_to_gl_equation(SceGxmBlendFunc func)
{
    switch (func) {
    case SCE_GXM_BLEND_FUNC_ADD: return GL_FUNC_ADD;
    case SCE_GXM_BLEND_FUNC_SUBTRACT: return GL_FUNC_SUBTRACT;
    case SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case SCE_GXM_BLEND_FUNC_MIN: return GL_MIN;
    case SCE_GXM_BLEND_FUNC_MAX: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum gxm_attribute_type(SceGxmAttributeFormat format)
{
    switch (format) {
    case SCE_GXM_ATTRIBUTE_FORMAT_U8:
    case SCE_GXM_ATTRIBUTE_FORMAT_U8N: return GL_UNSIGNED_BYTE;
    case SCE_GXM_ATTRIBUTE_FORMAT_S8:
    case SCE_GXM_ATTRIBUTE_FORMAT_S8N: return GL_BYTE;
    case SCE_GXM_ATTRIBUTE_FORMAT_U16:
    case SCE_GXM_ATTRIBUTE_FORMAT_U16N: return GL_UNSIGNED_SHORT;
    case SCE_GXM_ATTRIBUTE_FORMAT_S16:
    case SCE_GXM_ATTRIBUTE_FORMAT_S16N: return GL_SHORT;
    case SCE_GXM_ATTRIBUTE_FORMAT_F16: return GL_HALF_FLOAT;
    default: return GL_FLOAT;
    }
}

static GLboolean gxm_attribute_normalized(SceGxmAttributeFormat format)
{
    return format == SCE_GXM_ATTRIBUTE_FORMAT_U8N || format == SCE_GXM_ATTRIBUTE_FORMAT_S8N ||
           format == SCE_GXM_ATTRIBUTE_FORMAT_U16N || format == SCE_GXM_ATTRIBUTE_FORMAT_S16N;
}

static void gxm_compile_shader(GLuint shader, const char *source, const char *name)
{
    GLint ok = 0;
    char log[1024];

    g_gl->glShaderSource(shader, 1, &source, NULL);
    g_gl->glCompileShader(shader);
    g_gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g_gl->glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        LOG("SceGxm: %s shader compilation failed:\n%s", name, log);
    }
}

static GLuint gxm_translate_shader(const SceGxmProgram *program, GLenum stage, const char *name,
                                   const SceGxmVertexAttribute *attributes, unsigned int count)
{
    uint32_t *spirv = NULL;
    uint32_t num_instr = 0;
    char *glsl = NULL;
    GLuint shader;

    if (!convert_gxp_to_spirv_c(&spirv, &num_instr, program, name, false, false, false, false,
                                attributes, count, false, false, NULL)) {
        LOG("SceGxm: GXP -> SPIR-V failed for %s", name);
        return 0;
    }

    if (spirv_to_glsl_es(spirv, num_instr, &glsl) != 0) {
        free(spirv);
        LOG("SceGxm: SPIR-V -> GLSL ES failed for %s", name);
        return 0;
    }

    free(spirv);

    shader = g_gl->glCreateShader(stage);
    gxm_compile_shader(shader, glsl, name);
    free(glsl);

    return shader;
}

static GLuint gxm_get_linked_program(const SceGxmVertexProgram *vs, const SceGxmFragmentProgram *fs)
{
    GLint ok = 0;
    char log[1024];

    for (unsigned int i = 0; i < g_linked_count; i++) {
        if (g_linked[i].vs == vs && g_linked[i].fs == fs)
            return g_linked[i].program;
    }

    if (g_linked_count == GXM_MAX_LINKED_PROGRAMS)
        return 0;

    GLuint program = g_gl->glCreateProgram();
    if (vs)
        g_gl->glAttachShader(program, vs->shader);
    if (fs)
        g_gl->glAttachShader(program, fs->shader);
    g_gl->glLinkProgram(program);
    g_gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_gl->glGetProgramInfoLog(program, sizeof(log), NULL, log);
        LOG("SceGxm: program link failed:\n%s", log);
        g_gl->glDeleteProgram(program);
        return 0;
    }

    g_linked[g_linked_count].vs = vs;
    g_linked[g_linked_count].fs = fs;
    g_linked[g_linked_count].program = program;
    g_linked_count++;

    return program;
}

static void gxm_upload_fragment_textures(SceGxmContext *context)
{
    for (unsigned int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        SceGxmTextureInner *texture = &context->fragment_textures[i];
        GxmTextureCache *cache = &context->textures[i];
        const void *data;
        uint32_t width, height, fmt;

        if (!texture->data_addr) {
            g_gl->glActiveTexture(GL_TEXTURE0 + i);
            g_gl->glBindTexture(GL_TEXTURE_2D, 0);
            continue;
        }

        data = gxm_texture_get_data(texture);
        width = (uint32_t)gxm_texture_get_width(texture);
        height = (uint32_t)gxm_texture_get_height(texture);
        fmt = gxm_texture_get_format(texture);
        if (!data || !width || !height)
            continue;

        if (!cache->tex) {
            g_gl->glGenTextures(1, &cache->tex);
        }

        g_gl->glActiveTexture(GL_TEXTURE0 + i);
        g_gl->glBindTexture(GL_TEXTURE_2D, cache->tex);

        /*
         * Only 32-bit linear formats are decoded for now. Swizzled layouts and
         * paletted formats need the Vita tiling decoder; until that lands they
         * are uploaded as if they were linear.
         */
        g_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, data);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                              texture->min_filter == SCE_GXM_TEXTURE_FILTER_LINEAR ? GL_LINEAR
                                                                                    : GL_NEAREST);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                              texture->mag_filter == SCE_GXM_TEXTURE_FILTER_LINEAR ? GL_LINEAR
                                                                                    : GL_NEAREST);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                              texture->uaddr_mode == SCE_GXM_TEXTURE_ADDR_CLAMP ? GL_CLAMP_TO_EDGE
                                                                                : GL_REPEAT);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                              texture->vaddr_mode == SCE_GXM_TEXTURE_ADDR_CLAMP ? GL_CLAMP_TO_EDGE
                                                                                : GL_REPEAT);

        cache->data = data;
        cache->width = width;
        cache->height = height;
        cache->format = fmt;
    }
}

static void gxm_flush_state(SceGxmContext *context, GLuint program)
{
    const SceGxmVertexProgram *vp = context->vertex_program;
    const SceGxmFragmentProgram *fp = context->fragment_program;

    g_gl->glUseProgram(program);

    /* Render-info uniform buffers (same layout the deko3d backend used). */
    {
        struct GXMRenderVertUniformBlock vert = {
            {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, 960.0f, 544.0f, 0.0f, 1.0f,
        };
        struct GXMRenderFragUniformBlock frag = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        GLint loc;

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[0]);
        g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(vert), &vert, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 2, context->ubo[0]);

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[1]);
        g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(frag), &frag, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 3, context->ubo[1]);

        loc = g_gl->glGetUniformLocation(program, "renderVertInfo");
        if (loc >= 0)
            g_gl->glUniform1i(loc, 2);
        loc = g_gl->glGetUniformLocation(program, "renderFragInfo");
        if (loc >= 0)
            g_gl->glUniform1i(loc, 3);
    }

    /* Default uniforms as storage buffers. */
    if (vp && context->vertex_uniforms) {
        g_gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, context->ssbo[0]);
        g_gl->glBufferData(GL_SHADER_STORAGE_BUFFER, context->vertex_uniform_size,
                           context->vertex_uniforms, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, context->ssbo[0]);
    }
    if (fp && context->fragment_uniforms) {
        g_gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, context->ssbo[1]);
        g_gl->glBufferData(GL_SHADER_STORAGE_BUFFER, context->fragment_uniform_size,
                           context->fragment_uniforms, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, context->ssbo[1]);
    }

    /* Depth and stencil. */
    if (context->ds_surface) {
        g_gl->glEnable(GL_DEPTH_TEST);
        g_gl->glDepthFunc(gxm_depth_func_to_gl(context->depth_func));
        g_gl->glDepthMask(context->depth_write == SCE_GXM_DEPTH_WRITE_ENABLED ? GL_TRUE : GL_FALSE);
        g_gl->glEnable(GL_STENCIL_TEST);
        g_gl->glStencilFuncSeparate(GL_FRONT, gxm_stencil_func_to_gl(context->front_stencil_func),
                                    (GLint)context->front_stencil_ref,
                                    context->front_stencil_compare_mask);
        g_gl->glStencilOpSeparate(GL_FRONT, gxm_stencil_op_to_gl(context->front_stencil_fail),
                                  gxm_stencil_op_to_gl(context->front_stencil_depth_fail),
                                  gxm_stencil_op_to_gl(context->front_stencil_pass));
        g_gl->glStencilMaskSeparate(GL_FRONT, context->front_stencil_write_mask);
        g_gl->glStencilFuncSeparate(GL_BACK, gxm_stencil_func_to_gl(context->back_stencil_func),
                                    (GLint)context->back_stencil_ref,
                                    context->back_stencil_compare_mask);
        g_gl->glStencilOpSeparate(GL_BACK, gxm_stencil_op_to_gl(context->back_stencil_fail),
                                  gxm_stencil_op_to_gl(context->back_stencil_depth_fail),
                                  gxm_stencil_op_to_gl(context->back_stencil_pass));
        g_gl->glStencilMaskSeparate(GL_BACK, context->back_stencil_write_mask);
    } else {
        g_gl->glDisable(GL_DEPTH_TEST);
        g_gl->glDisable(GL_STENCIL_TEST);
    }

    /* Blend and color mask from the fragment program. */
    if (fp) {
        const SceGxmBlendInfo *b = &fp->blendInfo;
        if (b->colorFunc != SCE_GXM_BLEND_FUNC_NONE || b->alphaFunc != SCE_GXM_BLEND_FUNC_NONE) {
            g_gl->glEnable(GL_BLEND);
            g_gl->glBlendEquationSeparate(gxm_blend_func_to_gl_equation((SceGxmBlendFunc)b->colorFunc),
                                          gxm_blend_func_to_gl_equation((SceGxmBlendFunc)b->alphaFunc));
            g_gl->glBlendFuncSeparate(gxm_blend_factor_to_gl((SceGxmBlendFactor)b->colorSrc),
                                      gxm_blend_factor_to_gl((SceGxmBlendFactor)b->colorDst),
                                      gxm_blend_factor_to_gl((SceGxmBlendFactor)b->alphaSrc),
                                      gxm_blend_factor_to_gl((SceGxmBlendFactor)b->alphaDst));
        } else {
            g_gl->glDisable(GL_BLEND);
        }
        g_gl->glColorMask((b->colorMask & SCE_GXM_COLOR_MASK_R) ? GL_TRUE : GL_FALSE,
                          (b->colorMask & SCE_GXM_COLOR_MASK_G) ? GL_TRUE : GL_FALSE,
                          (b->colorMask & SCE_GXM_COLOR_MASK_B) ? GL_TRUE : GL_FALSE,
                          (b->colorMask & SCE_GXM_COLOR_MASK_A) ? GL_TRUE : GL_FALSE);
    }

    gxm_upload_fragment_textures(context);

    /* Vertex attributes: stream data is copied into a GL buffer every draw. */
    if (vp) {
        for (unsigned int i = 0; i < vp->attributeCount; i++) {
            const SceGxmVertexAttribute *a = &vp->attributes[i];
            const SceGxmVertexStream *s;
            int location;
            GxmStream *stream;
            GLenum type;

            if (a->streamIndex >= vp->streamCount)
                continue;

            location = vita3k_gxm_get_attribute_location(vp->programId->programHeader, a->regIndex);
            if (location < 0)
                continue;

            s = &vp->streams[a->streamIndex];
            stream = &context->streams[a->streamIndex];
            if (!stream->data)
                continue;

            if (!stream->vbo)
                g_gl->glGenBuffers(1, &stream->vbo);
            g_gl->glBindBuffer(GL_ARRAY_BUFFER, stream->vbo);
            g_gl->glBufferData(GL_ARRAY_BUFFER, stream->size, stream->data, GL_STREAM_DRAW);

            type = gxm_attribute_type((SceGxmAttributeFormat)a->format);
            g_gl->glEnableVertexAttribArray((GLuint)location);
            g_gl->glVertexAttribPointer((GLuint)location, a->componentCount, type,
                                        gxm_attribute_normalized((SceGxmAttributeFormat)a->format),
                                        s->stride, (const void *)(uintptr_t)a->offset);
        }
    }
}

EXPORT(SceGxm, 0xB0F1E4EC, int, sceGxmInitialize, const SceGxmInitializeParams *params)
{
    if (g_gxm_initialized)
        return SCE_GXM_ERROR_ALREADY_INITIALIZED;

    if (!g_device) {
        LOG("SceGxm: no GPU device");
        return SCE_GXM_ERROR_UNINITIALIZED;
    }

    g_gl = host_gpu_get_gles(g_device);
    if (!g_gl) {
        LOG("SceGxm: GLES backend unavailable, GXM calls will be ignored");
        g_gxm_initialized = true;
        return 0;
    }

    g_notification_region = calloc(GXM_NOTIFICATIONS, sizeof(uint32_t));
    if (!g_notification_region)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;

    g_display_callback = params ? params->displayQueueCallback : NULL;
    g_display_callback_data_size = params ? params->displayQueueCallbackDataSize : 0;
    g_linked_count = 0;
    g_gxm_initialized = true;

    return 0;
}

EXPORT(SceGxm, 0xB627DE66, int, sceGxmTerminate)
{
    if (!g_gxm_initialized)
        return SCE_GXM_ERROR_UNINITIALIZED;

    if (g_gl) {
        for (unsigned int i = 0; i < g_linked_count; i++)
            g_gl->glDeleteProgram(g_linked[i].program);
    }
    g_linked_count = 0;
    free(g_notification_region);
    g_notification_region = NULL;
    g_gxm_initialized = false;
    return 0;
}

EXPORT(SceGxm, 0x8BDE825A, volatile unsigned int *, sceGxmGetNotificationRegion, void)
{
    return g_notification_region;
}

EXPORT(SceGxm, 0x6A6013E1, int, sceGxmSyncObjectCreate, SceGxmSyncObject **syncObject)
{
    SceGxmSyncObject *sync = calloc(1, sizeof(*sync));
    if (!sync)
        return SCE_KERNEL_ERROR_NO_MEMORY;
    *syncObject = sync;
    return 0;
}

EXPORT(SceGxm, 0x889AE88C, int, sceGxmSyncObjectDestroy, SceGxmSyncObject *syncObject)
{
    free(syncObject);
    return 0;
}

EXPORT(SceGxm, 0x9F448E79, int, sceGxmNotificationWait, const SceGxmNotification *notification)
{
    (void)notification;
    return 0;
}

EXPORT(SceGxm, 0xE84CE5B4, int, sceGxmCreateContext, const SceGxmContextParams *params,
       SceGxmContext **context)
{
    SceGxmContext *ctx;

    if (!params || !context || params->hostMemSize < sizeof(SceGxmContext))
        return SCE_GXM_ERROR_INVALID_VALUE;

    ctx = params->hostMem;
    memset(ctx, 0, sizeof(*ctx));
    ctx->params = *params;

    ctx->vertex_uniform_arena = malloc(GXM_UNIFORM_ARENA_SIZE);
    ctx->fragment_uniform_arena = malloc(GXM_UNIFORM_ARENA_SIZE);
    if (!ctx->vertex_uniform_arena || !ctx->fragment_uniform_arena)
        goto oom;

    if (g_gl && g_gl->glGenBuffers) {
        g_gl->glGenBuffers(2, ctx->ssbo);
        g_gl->glGenBuffers(2, ctx->ubo);
    }

    ctx->depth_func = SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    ctx->depth_write = SCE_GXM_DEPTH_WRITE_ENABLED;
    ctx->front_stencil_func = SCE_GXM_STENCIL_FUNC_ALWAYS;
    ctx->back_stencil_func = SCE_GXM_STENCIL_FUNC_ALWAYS;
    ctx->front_stencil_compare_mask = 0xFF;
    ctx->front_stencil_write_mask = 0xFF;
    ctx->back_stencil_compare_mask = 0xFF;
    ctx->back_stencil_write_mask = 0xFF;

    *context = ctx;
    return 0;

oom:
    free(ctx->vertex_uniform_arena);
    free(ctx->fragment_uniform_arena);
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceGxm, 0xEDDC5FB2, int, sceGxmDestroyContext, SceGxmContext *context)
{
    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;

    if (g_gl) {
        for (unsigned int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++)
            if (context->textures[i].tex)
                g_gl->glDeleteTextures(1, &context->textures[i].tex);
        for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++)
            if (context->streams[i].vbo)
                g_gl->glDeleteBuffers(1, &context->streams[i].vbo);
        if (context->ssbo[0] || context->ssbo[1])
            g_gl->glDeleteBuffers(2, context->ssbo);
        if (context->ubo[0] || context->ubo[1])
            g_gl->glDeleteBuffers(2, context->ubo);
    }

    free(context->vertex_uniform_arena);
    free(context->fragment_uniform_arena);
    return 0;
}

EXPORT(SceGxm, 0x0733D8AE, void, sceGxmFinish, SceGxmContext *context)
{
    (void)context;
    if (g_gl)
        g_gl->glFinish();
}

EXPORT(SceGxm, 0x05032658, int, sceGxmShaderPatcherCreate, const SceGxmShaderPatcherParams *params,
       SceGxmShaderPatcher **shaderPatcher)
{
    SceGxmShaderPatcher *patcher = calloc(1, sizeof(*patcher));
    if (!patcher)
        return SCE_KERNEL_ERROR_NO_MEMORY;
    if (params)
        patcher->params = *params;
    *shaderPatcher = patcher;
    return 0;
}

EXPORT(SceGxm, 0xEAA5B100, int, sceGxmShaderPatcherDestroy, SceGxmShaderPatcher *shaderPatcher)
{
    free(shaderPatcher->registered_programs);
    free(shaderPatcher);
    return 0;
}

EXPORT(SceGxm, 0x2B528462, int, sceGxmShaderPatcherRegisterProgram,
       SceGxmShaderPatcher *shaderPatcher, const SceGxmProgram *programHeader,
       SceGxmShaderPatcherId *programId)
{
    SceGxmRegisteredProgram *registered = calloc(1, sizeof(*registered));

    if (!registered)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    registered->programHeader = programHeader;
    shaderPatcher->registered_programs =
        realloc(shaderPatcher->registered_programs,
                (shaderPatcher->registered_count + 1) * sizeof(SceGxmShaderPatcherId));
    shaderPatcher->registered_programs[shaderPatcher->registered_count++] = registered;
    *programId = registered;
    return 0;
}

EXPORT(SceGxm, 0xF103AF8A, int, sceGxmShaderPatcherUnregisterProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmShaderPatcherId programId)
{
    (void)shaderPatcher;
    free(programId);
    return 0;
}

EXPORT(SceGxm, 0xB7BBA6D5, int, sceGxmShaderPatcherCreateVertexProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmShaderPatcherId programId,
       const SceGxmVertexAttribute *attributes, unsigned int attributeCount,
       const SceGxmVertexStream *streams, unsigned int streamCount,
       SceGxmVertexProgram **vertexProgram)
{
    SceGxmVertexProgram *vp = calloc(1, sizeof(*vp));

    if (!vp)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    vp->programId = programId;
    vp->attributeCount = attributeCount;
    vp->attributes = calloc(attributeCount, sizeof(*attributes));
    memcpy(vp->attributes, attributes, attributeCount * sizeof(*attributes));
    vp->streamCount = streamCount;
    vp->streams = calloc(streamCount, sizeof(*streams));
    memcpy(vp->streams, streams, streamCount * sizeof(*streams));

    if (g_gl)
        vp->shader = gxm_translate_shader(programId->programHeader, GL_VERTEX_SHADER, "vert",
                                          attributes, attributeCount);
    if (g_gl && !vp->shader) {
        free(vp->attributes);
        free(vp->streams);
        free(vp);
        return SCE_GXM_ERROR_INVALID_VALUE;
    }

    *vertexProgram = vp;
    return 0;
}

EXPORT(SceGxm, 0xAC1FF2DA, int, sceGxmShaderPatcherReleaseVertexProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmVertexProgram *vertexProgram)
{
    (void)shaderPatcher;
    if (g_gl && vertexProgram->shader)
        g_gl->glDeleteShader(vertexProgram->shader);
    free(vertexProgram->attributes);
    free(vertexProgram->streams);
    free(vertexProgram);
    return 0;
}

EXPORT(SceGxm, 0x4ED2E49D, int, sceGxmShaderPatcherCreateFragmentProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmShaderPatcherId programId,
       SceGxmOutputRegisterFormat outputFormat, SceGxmMultisampleMode multisampleMode,
       const SceGxmBlendInfo *blendInfo, const SceGxmProgram *vertexProgram,
       SceGxmFragmentProgram **fragmentProgram)
{
    SceGxmFragmentProgram *fp = calloc(1, sizeof(*fp));

    (void)shaderPatcher;
    (void)vertexProgram;

    if (!fp)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    fp->programId = programId;
    fp->outputFormat = outputFormat;
    fp->multisampleMode = multisampleMode;
    if (blendInfo) {
        fp->blendInfo = *blendInfo;
    } else {
        fp->blendInfo = (SceGxmBlendInfo){
            SCE_GXM_COLOR_MASK_ALL, SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
            SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
            SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
        };
    }

    if (g_gl)
        fp->shader = gxm_translate_shader(programId->programHeader, GL_FRAGMENT_SHADER, "frag",
                                          NULL, 0);
    if (g_gl && !fp->shader) {
        free(fp);
        return SCE_GXM_ERROR_INVALID_VALUE;
    }

    *fragmentProgram = fp;
    return 0;
}

EXPORT(SceGxm, 0xBE2743D1, int, sceGxmShaderPatcherReleaseFragmentProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmFragmentProgram *fragmentProgram)
{
    (void)shaderPatcher;
    if (g_gl && fragmentProgram->shader)
        g_gl->glDeleteShader(fragmentProgram->shader);
    free(fragmentProgram);
    return 0;
}

EXPORT(SceGxm, 0xA949A803, const SceGxmProgram *, sceGxmShaderPatcherGetProgramFromId,
       SceGxmShaderPatcherId programId)
{
    return programId ? programId->programHeader : NULL;
}

EXPORT(SceGxm, 0xB291C959, int, sceGxmGetRenderTargetMemSize,
       const SceGxmRenderTargetParams *params, unsigned int *driverMemSize)
{
    (void)params;
    *driverMemSize = sizeof(SceGxmRenderTarget);
    return 0;
}

EXPORT(SceGxm, 0x207AF96B, int, sceGxmCreateRenderTarget, const SceGxmRenderTargetParams *params,
       SceGxmRenderTarget **renderTarget)
{
    SceGxmRenderTarget *rt;

    if (!g_gxm_initialized)
        return SCE_GXM_ERROR_UNINITIALIZED;
    if (!params)
        return SCE_GXM_ERROR_INVALID_POINTER;

    rt = calloc(1, sizeof(*rt));
    if (!rt)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    rt->params = *params;
    if (g_gl) {
        g_gl->glGenTextures(1, &rt->color);
        g_gl->glBindTexture(GL_TEXTURE_2D, rt->color);
        g_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, params->width, params->height, 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, NULL);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        g_gl->glGenRenderbuffers(1, &rt->depth);
        g_gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->depth);
        g_gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, params->width,
                                    params->height);

        g_gl->glGenFramebuffers(1, &rt->fbo);
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->color,
                                     0);
        g_gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                        GL_RENDERBUFFER, rt->depth);
        if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG("SceGxm: render target framebuffer incomplete");
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    *renderTarget = rt;
    return 0;
}

EXPORT(SceGxm, 0x0B94C50A, int, sceGxmDestroyRenderTarget, SceGxmRenderTarget *renderTarget)
{
    if (g_gl) {
        if (renderTarget->fbo)
            g_gl->glDeleteFramebuffers(1, &renderTarget->fbo);
        if (renderTarget->color)
            g_gl->glDeleteTextures(1, &renderTarget->color);
        if (renderTarget->depth)
            g_gl->glDeleteRenderbuffers(1, &renderTarget->depth);
    }
    free(renderTarget);
    return 0;
}

EXPORT(SceGxm, 0xED0F6E25, int, sceGxmColorSurfaceInit, SceGxmColorSurface *surface,
       SceGxmColorFormat colorFormat, SceGxmColorSurfaceType surfaceType,
       SceGxmColorSurfaceScaleMode scaleMode, SceGxmOutputRegisterSize outputRegisterSize,
       unsigned int width, unsigned int height, unsigned int strideInPixels, void *data)
{
    SceGxmColorSurfaceInner *inner = (SceGxmColorSurfaceInner *)surface;

    if (!surface)
        return SCE_GXM_ERROR_INVALID_POINTER;

    memset(inner, 0, sizeof(*inner));
    inner->downscale = scaleMode == SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE;
    inner->width = width;
    inner->height = height;
    inner->strideInPixels = strideInPixels;
    inner->data = data;
    inner->colorFormat = colorFormat;
    inner->surfaceType = surfaceType;
    inner->outputRegisterSize = outputRegisterSize;
    return 0;
}

EXPORT(SceGxm, 0xCA9D41D1, int, sceGxmDepthStencilSurfaceInit, SceGxmDepthStencilSurface *surface,
       SceGxmDepthStencilFormat depthStencilFormat, SceGxmDepthStencilSurfaceType surfaceType,
       unsigned int strideInSamples, void *depthData, void *stencilData)
{
    if (!surface)
        return SCE_GXM_ERROR_INVALID_POINTER;

    memset(surface, 0, sizeof(*surface));
    surface->zlsControl =
        SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED | SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED |
        (((strideInSamples >> 5) - 1) << SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_STRIDE_OFFSET) |
        (surfaceType &
         (SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_MASK << SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_OFFSET)) |
        (depthStencilFormat &
         (SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORMAT_MASK << SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORMAT_OFFSET));
    surface->depthData = depthData;
    surface->stencilData = stencilData;
    surface->backgroundDepth = 1.0f;
    surface->backgroundControl = SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT;
    return 0;
}

EXPORT(SceGxm, 0x14BD831F, void, sceGxmSetFrontDepthFunc, SceGxmContext *context,
       SceGxmDepthFunc depthFunc)
{
    context->depth_func = depthFunc;
}

EXPORT(SceGxm, 0xF32CBF34, void, sceGxmSetFrontDepthWriteEnable, SceGxmContext *context,
       SceGxmDepthWriteMode enable)
{
    context->depth_write = enable;
}

EXPORT(SceGxm, 0x8FA6FE44, void, sceGxmSetFrontStencilRef, SceGxmContext *context,
       unsigned int sref)
{
    context->front_stencil_ref = sref;
}

EXPORT(SceGxm, 0xB042A4D2, void, sceGxmSetBackDepthFunc, SceGxmContext *context,
       SceGxmDepthFunc depthFunc)
{
    (void)context;
    (void)depthFunc;
}

EXPORT(SceGxm, 0xC18B706B, void, sceGxmSetBackDepthWriteEnable, SceGxmContext *context,
       SceGxmDepthWriteMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0x866A0517, void, sceGxmSetBackStencilRef, SceGxmContext *context, unsigned int sref)
{
    context->back_stencil_ref = sref;
}

EXPORT(SceGxm, 0xB8645A9A, void, sceGxmSetFrontStencilFunc, SceGxmContext *context,
       SceGxmStencilFunc func, SceGxmStencilOp stencilFail, SceGxmStencilOp depthFail,
       SceGxmStencilOp depthPass, unsigned char compareMask, unsigned char writeMask)
{
    context->front_stencil_func = func;
    context->front_stencil_fail = stencilFail;
    context->front_stencil_depth_fail = depthFail;
    context->front_stencil_pass = depthPass;
    context->front_stencil_compare_mask = compareMask;
    context->front_stencil_write_mask = writeMask;
}

EXPORT(SceGxm, 0x1A68C8D2, void, sceGxmSetBackStencilFunc, SceGxmContext *context,
       SceGxmStencilFunc func, SceGxmStencilOp stencilFail, SceGxmStencilOp depthFail,
       SceGxmStencilOp depthPass, unsigned char compareMask, unsigned char writeMask)
{
    context->back_stencil_func = func;
    context->back_stencil_fail = stencilFail;
    context->back_stencil_depth_fail = depthFail;
    context->back_stencil_pass = depthPass;
    context->back_stencil_compare_mask = compareMask;
    context->back_stencil_write_mask = writeMask;
}

EXPORT(SceGxm, 0x8734FF4E, int, sceGxmBeginScene, SceGxmContext *context, unsigned int flags,
       const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
       SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
       const SceGxmColorSurface *colorSurface, const SceGxmDepthStencilSurface *depthStencil)
{
    (void)flags;
    (void)validRegion;
    (void)vertexSyncObject;

    if (context->in_scene)
        return SCE_GXM_ERROR_WITHIN_SCENE;
    if (!renderTarget)
        return SCE_GXM_ERROR_INVALID_POINTER;

    context->render_target = renderTarget;
    context->color_surface = (const SceGxmColorSurfaceInner *)colorSurface;
    context->ds_surface = depthStencil;
    context->fragment_sync = fragmentSyncObject;
    context->vertex_uniform_head = 0;
    context->fragment_uniform_head = 0;
    context->vertex_uniforms = NULL;
    context->fragment_uniforms = NULL;
    context->in_scene = true;

    if (g_gl) {
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, renderTarget->fbo);
        g_gl->glViewport(0, 0, renderTarget->params.width, renderTarget->params.height);
        g_gl->glDisable(GL_SCISSOR_TEST);
        g_gl->glClearDepthf(1.0f);
        g_gl->glClearStencil(0);
        g_gl->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }

    return 0;
}

EXPORT(SceGxm, 0xFE300E2F, int, sceGxmEndScene, SceGxmContext *context,
       const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification)
{
    const SceGxmColorSurfaceInner *surface;
    uint32_t width, height, stride;
    uint8_t *rgba;

    (void)vertexNotification;
    (void)fragmentNotification;

    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;

    context->in_scene = false;

    if (!g_gl || !context->render_target)
        return 0;

    g_gl->glFinish();

    surface = context->color_surface;
    if (!surface || !surface->data)
        return 0;

    width = surface->width < context->render_target->params.width ? surface->width
                                                                 : context->render_target->params.width;
    height = surface->height < context->render_target->params.height
                 ? surface->height
                 : context->render_target->params.height;
    stride = surface->strideInPixels;

    rgba = malloc((size_t)width * height * 4);
    if (!rgba)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;

    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, context->render_target->fbo);
    g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    /* GL's origin is bottom-left; Vita surfaces are top-left. */
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = rgba + (size_t)(height - 1 - y) * width * 4;
        uint8_t *dst = (uint8_t *)surface->data + (size_t)y * stride * 4;

        if (surface->colorFormat == SCE_GXM_COLOR_FORMAT_A8B8G8R8) {
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        } else {
            memcpy(dst, src, (size_t)width * 4);
        }
    }

    free(rgba);
    return 0;
}

EXPORT(SceGxm, 0xEC5C26B5, int, sceGxmDisplayQueueAddEntry, SceGxmSyncObject *oldBuffer,
       SceGxmSyncObject *newBuffer, const void *callbackData)
{
    (void)oldBuffer;
    (void)newBuffer;
    if (g_display_callback && callbackData)
        g_display_callback((void *)callbackData);
    return 0;
}

EXPORT(SceGxm, 0xB98C5B0D, int, sceGxmDisplayQueueFinish, void)
{
    return 0;
}

EXPORT(SceGxm, 0x31FF8ABD, void, sceGxmSetVertexProgram, SceGxmContext *context,
       const SceGxmVertexProgram *vertexProgram)
{
    context->vertex_program = vertexProgram;
}

EXPORT(SceGxm, 0xAD2F48D9, void, sceGxmSetFragmentProgram, SceGxmContext *context,
       const SceGxmFragmentProgram *fragmentProgram)
{
    context->fragment_program = fragmentProgram;
}

EXPORT(SceGxm, 0x29C34DF5, int, sceGxmSetFragmentTexture, SceGxmContext *context,
       unsigned int textureIndex, const SceGxmTexture *texture)
{
    if (textureIndex >= SCE_GXM_MAX_TEXTURE_UNITS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    context->fragment_textures[textureIndex] = *(const SceGxmTextureInner *)texture;
    return 0;
}

EXPORT(SceGxm, 0x895DF2E9, int, sceGxmSetVertexStream, SceGxmContext *context,
       unsigned int streamIndex, const void *streamData)
{
    VitaMemBlockInfo *block;

    if (streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
        return SCE_GXM_ERROR_INVALID_VALUE;

    block = SceSysmem_get_vita_memblock_info_for_addr(streamData);
    if (!block)
        return SCE_GXM_ERROR_INVALID_VALUE;

    context->streams[streamIndex].data = streamData;
    context->streams[streamIndex].size =
        (uint32_t)(block->size - ((uintptr_t)streamData - (uintptr_t)block->base));
    return 0;
}

EXPORT(SceGxm, 0x8FA3F9C3, unsigned int, sceGxmProgramGetDefaultUniformBufferSize,
       const SceGxmProgram *program)
{
    return program->default_uniform_buffer_count * sizeof(float);
}

EXPORT(SceGxm, 0xC697CAE5, int, sceGxmSetVertexDefaultUniformBuffer, SceGxmContext *context,
       const void *uniformBuffer)
{
    context->vertex_uniforms = (void *)uniformBuffer;
    context->vertex_uniform_size =
        context->vertex_program
            ? context->vertex_program->programId->programHeader->default_uniform_buffer_count *
                  sizeof(float)
            : 0;
    return 0;
}

EXPORT(SceGxm, 0x97118913, int, sceGxmReserveVertexDefaultUniformBuffer, SceGxmContext *context,
       void **uniformBuffer)
{
    uint32_t size;

    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!context->vertex_program)
        return SCE_GXM_ERROR_NULL_PROGRAM;

    size = context->vertex_program->programId->programHeader->default_uniform_buffer_count *
           sizeof(float);
    if (!size) {
        *uniformBuffer = NULL;
        return 0;
    }
    if (context->vertex_uniform_head + size > GXM_UNIFORM_ARENA_SIZE)
        context->vertex_uniform_head = 0;

    context->vertex_uniforms = context->vertex_uniform_arena + context->vertex_uniform_head;
    context->vertex_uniform_size = size;
    context->vertex_uniform_head += size;
    *uniformBuffer = context->vertex_uniforms;
    return 0;
}

EXPORT(SceGxm, 0xA824EB24, int, sceGxmSetFragmentDefaultUniformBuffer, SceGxmContext *context,
       const void *uniformBuffer)
{
    context->fragment_uniforms = (void *)uniformBuffer;
    context->fragment_uniform_size =
        context->fragment_program
            ? context->fragment_program->programId->programHeader->default_uniform_buffer_count *
                  sizeof(float)
            : 0;
    return 0;
}

EXPORT(SceGxm, 0x7B1FABB6, int, sceGxmReserveFragmentDefaultUniformBuffer, SceGxmContext *context,
       void **uniformBuffer)
{
    uint32_t size;

    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!context->fragment_program)
        return SCE_GXM_ERROR_NULL_PROGRAM;

    size = context->fragment_program->programId->programHeader->default_uniform_buffer_count *
           sizeof(float);
    if (!size) {
        *uniformBuffer = NULL;
        return 0;
    }
    if (context->fragment_uniform_head + size > GXM_UNIFORM_ARENA_SIZE)
        context->fragment_uniform_head = 0;

    context->fragment_uniforms = context->fragment_uniform_arena + context->fragment_uniform_head;
    context->fragment_uniform_size = size;
    context->fragment_uniform_head += size;
    *uniformBuffer = context->fragment_uniforms;
    return 0;
}

EXPORT(SceGxm, 0x65DD0C84, int, sceGxmSetUniformDataF, void *uniformBuffer,
       const SceGxmProgramParameter *parameter, unsigned int componentOffset,
       unsigned int componentCount, const float *sourceData)
{
    const bool is_float = (parameter->type == SCE_GXM_PARAMETER_TYPE_F16) ||
                          (parameter->type == SCE_GXM_PARAMETER_TYPE_F32);
    uint32_t alignment, scalar_size, vector_size, vector_index, scalar_index;
    uint32_t vector_mod_align, vector_padding_bytes, offset, scalars_to_copy;

    if (!uniformBuffer || !sourceData)
        return SCE_GXM_ERROR_INVALID_POINTER;

    alignment = (is_float && parameter->array_size > 1 && parameter->component_count > 1) ? 8 : 4;
    scalar_size = gxm_parameter_type_size(parameter->type);
    vector_size = scalar_size * parameter->component_count;
    vector_index = componentOffset / parameter->component_count;
    scalar_index = componentOffset % parameter->component_count;
    vector_mod_align = vector_size % alignment;
    vector_padding_bytes = vector_mod_align ? alignment - vector_mod_align : 0;
    offset = parameter->resource_index * sizeof(uint32_t) +
             vector_index * (vector_size + vector_padding_bytes) + scalar_index * scalar_size;

    scalars_to_copy = MIN2(parameter->component_count - scalar_index, componentCount);
    while (componentCount > 0) {
        memcpy((char *)uniformBuffer + offset, sourceData, scalars_to_copy * scalar_size);
        offset += scalars_to_copy * scalar_size + vector_padding_bytes;
        sourceData += scalars_to_copy;
        componentCount -= scalars_to_copy;
        scalars_to_copy = MIN2(parameter->component_count, componentCount);
    }

    return 0;
}

EXPORT(SceGxm, 0x4811AECB, int, sceGxmTextureInitLinear, SceGxmTexture *texture, const void *data,
       SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    if (!texture)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (width > 4096 || height > 4096 || mipCount > 13)
        return SCE_GXM_ERROR_INVALID_VALUE;

    memset(inner, 0, sizeof(*inner));
    inner->mip_count = (mipCount ? mipCount : 1) - 1;
    inner->format0 = (texFormat & 0x80000000) >> 31;
    inner->lod_bias = 31;
    inner->uaddr_mode = inner->vaddr_mode = SCE_GXM_TEXTURE_ADDR_CLAMP;
    inner->height = height - 1;
    inner->width = width - 1;
    inner->base_format = (texFormat & 0x1F000000) >> 24;
    inner->type = SCE_GXM_TEXTURE_LINEAR >> 29;
    inner->data_addr = (uint32_t)(uintptr_t)data >> 2;
    inner->swizzle_format = (texFormat & 0x7000) >> 12;
    inner->normalize_mode = 1;
    inner->min_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    inner->mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    return 0;
}

#define GXM_TEX_GETTER(name, expr)                                                                 \
    EXPORT(SceGxm, name, uint32_t, sce##name, const SceGxmTexture *texture)                        \
    {                                                                                              \
        return (uint32_t)(expr);                                                                   \
    }

GXM_TEX_GETTER(0x5420A086, gxm_texture_get_height((const SceGxmTextureInner *)texture))
GXM_TEX_GETTER(0x126A3EB3, gxm_texture_get_width((const SceGxmTextureInner *)texture))
GXM_TEX_GETTER(0xB0BD52F3, gxm_texture_get_type((const SceGxmTextureInner *)texture) ==
                                   SCE_GXM_TEXTURE_LINEAR_STRIDED
                               ? gxm_texture_get_stride_in_bytes((const SceGxmTextureInner *)texture)
                               : 0)

EXPORT(SceGxm, 0x5341BD46, void *, sceGxmTextureGetData, const SceGxmTexture *texture)
{
    return gxm_texture_get_data((const SceGxmTextureInner *)texture);
}

EXPORT(SceGxm, 0xE868D2B3, SceGxmTextureFormat, sceGxmTextureGetFormat,
       const SceGxmTexture *texture)
{
    return (SceGxmTextureFormat)gxm_texture_get_format((const SceGxmTextureInner *)texture);
}

EXPORT(SceGxm, 0xF23FCE81, SceGxmTextureGammaMode, sceGxmTextureGetGammaMode,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->gamma_mode << 27;
}

EXPORT(SceGxm, 0x2DE55DA5, uint32_t, sceGxmTextureGetLodBias, const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->lod_bias;
}

EXPORT(SceGxm, 0xBE524A2C, uint32_t, sceGxmTextureGetLodMin, const SceGxmTexture *texture)
{
    const SceGxmTextureInner *inner = (const SceGxmTextureInner *)texture;
    return inner->lod_min0 | (inner->lod_min1 << 2);
}

EXPORT(SceGxm, 0xAE7FBB51, SceGxmTextureFilter, sceGxmTextureGetMagFilter,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->mag_filter;
}

EXPORT(SceGxm, 0x920666C6, SceGxmTextureFilter, sceGxmTextureGetMinFilter,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->min_filter;
}

EXPORT(SceGxm, 0xCE94CA15, SceGxmTextureMipFilter, sceGxmTextureGetMipFilter,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->mip_filter ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED
                                                             : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED;
}

EXPORT(SceGxm, 0xF7B7B1E4, uint32_t, sceGxmTextureGetMipmapCount, const SceGxmTexture *texture)
{
    return (((const SceGxmTextureInner *)texture)->mip_count + 1) & 0xf;
}

EXPORT(SceGxm, 0x4CC42929, uint32_t, sceGxmTextureGetMipmapCountUnsafe,
       const SceGxmTexture *texture)
{
    return (((const SceGxmTextureInner *)texture)->mip_count + 1) & 0xf;
}

EXPORT(SceGxm, 0x512BB86C, int, sceGxmTextureGetNormalizeMode, const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->normalize_mode << 31;
}

EXPORT(SceGxm, 0x0D189C30, void *, sceGxmTextureGetPalette, const SceGxmTexture *texture)
{
    SceGxmTextureBaseFormat base =
        gxm_texture_get_base_format(gxm_texture_get_format((const SceGxmTextureInner *)texture));
    return gxm_base_format_is_paletted_format(base) ? (void *)(uintptr_t)(texture->palette_addr << 6)
                                                    : NULL;
}

EXPORT(SceGxm, 0xF65D4917, SceGxmTextureType, sceGxmTextureGetType, const SceGxmTexture *texture)
{
    return (SceGxmTextureType)gxm_texture_get_type((const SceGxmTextureInner *)texture);
}

EXPORT(SceGxm, 0x2AE22788, SceGxmTextureAddrMode, sceGxmTextureGetUAddrMode,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->uaddr_mode;
}

EXPORT(SceGxm, 0xC037DA83, int, sceGxmTextureGetUAddrModeSafe, const SceGxmTexture *texture)
{
    return gxm_texture_get_type((const SceGxmTextureInner *)texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED
               ? SCE_GXM_TEXTURE_ADDR_CLAMP
               : ((const SceGxmTextureInner *)texture)->uaddr_mode;
}

EXPORT(SceGxm, 0x46136CA9, SceGxmTextureAddrMode, sceGxmTextureGetVAddrMode,
       const SceGxmTexture *texture)
{
    return ((const SceGxmTextureInner *)texture)->vaddr_mode;
}

EXPORT(SceGxm, 0xD2F0D9C1, int, sceGxmTextureGetVAddrModeSafe, const SceGxmTexture *texture)
{
    return gxm_texture_get_type((const SceGxmTextureInner *)texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED
               ? SCE_GXM_TEXTURE_ADDR_CLAMP
               : ((const SceGxmTextureInner *)texture)->vaddr_mode;
}

EXPORT(SceGxm, 0x277794C4, const SceGxmProgramParameter *, sceGxmProgramFindParameterByName,
       const SceGxmProgram *program, const char *name)
{
    const uint8_t *parameter_bytes;
    const SceGxmProgramParameter *parameters;

    if (!program || !name)
        return NULL;

    parameters = (const SceGxmProgramParameter *)((const char *)&program->parameters_offset +
                                                  program->parameters_offset);

    for (uint32_t i = 0; i < program->parameter_count; i++) {
        parameter_bytes = (const uint8_t *)&parameters[i];
        if (strcmp((const char *)(parameter_bytes + parameters[i].name_offset), name) == 0)
            return (const SceGxmProgramParameter *)parameter_bytes;
    }

    return NULL;
}

#define GXM_PARAM_GETTER(name, type, expr)                                                         \
    EXPORT(SceGxm, name, type, sce##name, const SceGxmProgramParameter *parameter)                 \
    {                                                                                              \
        return (type)(expr);                                                                       \
    }

GXM_PARAM_GETTER(0xDBA8D061, uint32_t, parameter->array_size)
GXM_PARAM_GETTER(0x1997DC17, SceGxmParameterCategory, parameter->category)
GXM_PARAM_GETTER(0xBD2998D1, uint32_t, parameter->component_count)
GXM_PARAM_GETTER(0xBB58267D, uint32_t, parameter->container_index)
GXM_PARAM_GETTER(0x5C79D59A, uint32_t, parameter->resource_index)
GXM_PARAM_GETTER(0x7B9023C3, SceGxmParameterType, parameter->type)
GXM_PARAM_GETTER(0xE6D9C4CE, SceGxmParameterSemantic,
                 parameter->category == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE
                     ? parameter->semantic
                     : SCE_GXM_PARAMETER_SEMANTIC_NONE)
GXM_PARAM_GETTER(0xB85CC13E, uint32_t, parameter->semantic_index & 0xf)

EXPORT(SceGxm, 0x6AF88A5D, const char *, sceGxmProgramParameterGetName,
       const SceGxmProgramParameter *parameter)
{
    return parameter ? (const char *)((uintptr_t)parameter + parameter->name_offset) : NULL;
}

EXPORT(SceGxm, 0x6E61DDF5, uint32_t, sceGxmProgramParameterGetIndex, const SceGxmProgram *program,
       const SceGxmProgramParameter *parameter)
{
    uint32_t parameter_offset = program->parameters_offset;
    if (parameter_offset > 0)
        parameter_offset += (uintptr_t)&program->parameters_offset;
    return (uint32_t)((uintptr_t)parameter - parameter_offset) >> 4;
}

EXPORT(SceGxm, 0xBC059AFC, int, sceGxmDraw, SceGxmContext *context, SceGxmPrimitiveType primType,
       SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount)
{
    GLuint program;
    GLenum gl_index_type;
    size_t index_size;
    uint32_t index_offset = 0;

    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!context->vertex_program || !context->fragment_program)
        return SCE_GXM_ERROR_NULL_PROGRAM;
    if (!g_gl)
        return 0;

    program = gxm_get_linked_program(context->vertex_program, context->fragment_program);
    if (!program)
        return SCE_GXM_ERROR_INVALID_VALUE;

    gxm_flush_state(context, program);

    if (indexType == SCE_GXM_INDEX_FORMAT_U32) {
        gl_index_type = GL_UNSIGNED_INT;
        index_size = 4;
    } else {
        gl_index_type = GL_UNSIGNED_SHORT;
        index_size = 2;
    }

    if (indexData) {
        VitaMemBlockInfo *block = SceSysmem_get_vita_memblock_info_for_addr(indexData);
        if (block)
            index_offset = (uint32_t)((uintptr_t)indexData - (uintptr_t)block->base);
        (void)index_offset;

        if (!context->ibo)
            g_gl->glGenBuffers(1, &context->ibo);
        g_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, context->ibo);
        g_gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * index_size), indexData,
                           GL_STREAM_DRAW);
    }

    g_gl->glDrawElements(gxm_primitive_to_gl(primType), (GLsizei)indexCount, gl_index_type, 0);

    return 0;
}

EXPORT(SceGxm, 0xC61E34FC, int, sceGxmMapMemory, void *base, SceSize size,
       SceGxmMemoryAttribFlags attr)
{
    (void)base;
    (void)size;
    (void)attr;
    return 0;
}

EXPORT(SceGxm, 0x828C68E8, int, sceGxmUnmapMemory, void *base)
{
    (void)base;
    return 0;
}

EXPORT(SceGxm, 0xFA437510, int, sceGxmMapVertexUsseMemory, void *base, SceSize size,
       unsigned int *offset)
{
    (void)base;
    (void)size;
    if (offset)
        *offset = 0;
    return 0;
}

EXPORT(SceGxm, 0x099134F5, int, sceGxmUnmapVertexUsseMemory, void *base)
{
    (void)base;
    return 0;
}

EXPORT(SceGxm, 0x008402C6, int, sceGxmMapFragmentUsseMemory, void *base, SceSize size,
       unsigned int *offset)
{
    (void)base;
    (void)size;
    if (offset)
        *offset = 0;
    return 0;
}

EXPORT(SceGxm, 0x80CCEDBB, int, sceGxmUnmapFragmentUsseMemor, void *base)
{
    (void)base;
    return 0;
}

DECLARE_LIBRARY(SceGxm, 0xf76b66bd);

int SceGxm_init(HostGpuDevice device)
{
    g_device = device;
    return 0;
}

int SceGxm_finish(void)
{
    if (g_gl)
        g_gl->glFinish();
    return 0;
}
