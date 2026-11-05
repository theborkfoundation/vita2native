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
#include <math.h>
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gxm/texture.h"
#include "gxm/util.h"
#include "host.h"
#include "host_gfx_gl.h"
#include "log.h"
#include "module.h"
#include "spirv_cross_glsl_es.h"
#include "util.h"
#include "vita3k_shader_recompiler_iface_c.h"

extern int vita3k_gxm_get_attribute_location(const SceGxmProgram *program, uint16_t reg_index);

#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif

#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

#define GXM_MAX_LINKED_PROGRAMS 128
#define GXM_MAX_UNIFORM_BUFFER_LAYOUT 16
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
    GLenum target;
    const void *data;
    const void *palette;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t mip_count;
    uint32_t hash;
    bool valid;
} GxmTextureCache;

typedef struct GxmCommand GxmCommand;

static void gxm_command_free(GxmCommand *command);

typedef struct SceGxmContext {
    SceGxmContextParams params;
    bool in_scene;

    const SceGxmRenderTarget *render_target;
    const SceGxmColorSurfaceInner *color_surface;
    const SceGxmDepthStencilSurface *ds_surface;
    const SceGxmDepthStencilSurface *ds_store_surface;
    SceGxmSyncObject *fragment_sync;

    const SceGxmVertexProgram *vertex_program;
    const SceGxmFragmentProgram *fragment_program;

    GxmStream streams[SCE_GXM_MAX_VERTEX_STREAMS];
    SceGxmTextureInner fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    SceGxmTextureInner vertex_textures[SCE_GXM_MAX_TEXTURE_UNITS];

    void *vertex_uniforms;
    void *fragment_uniforms;
    uint32_t vertex_uniform_size;
    uint32_t fragment_uniform_size;
    const void *vertex_uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];
    const void *fragment_uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];

    uint8_t *vertex_uniform_arena;
    uint8_t *fragment_uniform_arena;
    uint32_t vertex_uniform_head;
    uint32_t fragment_uniform_head;

    uint8_t *vertex_ssbo_data;
    uint8_t *fragment_ssbo_data;
    uint32_t vertex_ssbo_size;
    uint32_t fragment_ssbo_size;

    float viewport_x_offset, viewport_x_scale;
    float viewport_y_offset, viewport_y_scale;
    float viewport_z_offset, viewport_z_scale;
    SceGxmViewportMode viewport_enable;
    SceGxmRegionClipMode region_clip_mode;
    unsigned int region_clip_xmin, region_clip_ymin;
    unsigned int region_clip_xmax, region_clip_ymax;
    SceGxmCullMode cull_mode;
    SceGxmPolygonMode front_polygon_mode;
    SceGxmPolygonMode back_polygon_mode;
    unsigned int front_point_line_width;
    unsigned int back_point_line_width;
    int front_depth_bias_factor, front_depth_bias_units;
    int back_depth_bias_factor, back_depth_bias_units;
    float w_clamp_value;
    SceGxmWClampMode w_clamp_enable;
    SceGxmTwoSidedMode two_sided;
    SceGxmFragmentProgramMode front_fragment_program_enable;
    SceGxmFragmentProgramMode back_fragment_program_enable;

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
    GLuint wire_ibo;
    GxmTextureCache textures[2][SCE_GXM_MAX_TEXTURE_UNITS];

    const SceGxmPrecomputedVertexState *precomputed_vertex;
    const SceGxmPrecomputedFragmentState *precomputed_fragment;

    bool deferred;
    bool command_list_active;
    GxmCommand *cmd_head;
    GxmCommand *cmd_tail;
    void *vdm_buffer;
    uint32_t vdm_buffer_size;
    void *vertex_buffer;
    uint32_t vertex_buffer_size;
    void *fragment_buffer;
    uint32_t fragment_buffer_size;
} SceGxmContext;

typedef struct SceGxmSyncObject {
    int unused;
} SceGxmSyncObject;

typedef struct SceGxmRegisteredProgram {
    const SceGxmProgram *programHeader;
    Vita3kUniformBufferLayout uniform_buffers[GXM_MAX_UNIFORM_BUFFER_LAYOUT];
    uint32_t uniform_buffer_count;
    uint32_t uniform_buffer_size; /* total SSBO container size */
    int refcount;
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
    int refcount;
} SceGxmVertexProgram;

typedef struct SceGxmFragmentProgram {
    SceGxmShaderPatcherId programId;
    SceGxmOutputRegisterFormat outputFormat;
    SceGxmMultisampleMode multisampleMode;
    SceGxmBlendInfo blendInfo;
    GLuint shader;
    bool mask_update;
    int refcount;
} SceGxmFragmentProgram;

typedef struct SceGxmRenderTarget {
    SceGxmRenderTargetParams params;
    GLuint fbo;
    GLuint color;
    GLuint depth;
    GLuint depth_rb;
    bool depth_is_texture;
} SceGxmRenderTarget;

typedef struct {
    const SceGxmVertexProgram *vs;
    const SceGxmFragmentProgram *fs;
    GLuint program;
} GxmLinkedProgram;

/*
 * Deferred contexts record draw snapshots instead of issuing GL commands. A
 * snapshot stores everything sceGxmDraw would read, so command lists can be
 * executed later on an immediate context that is inside a scene.
 */
typedef struct {
    const SceGxmVertexProgram *vertex_program;
    const SceGxmFragmentProgram *fragment_program;
    const void *streams[SCE_GXM_MAX_VERTEX_STREAMS];
    SceGxmTextureInner fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    SceGxmTextureInner vertex_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    const void *vertex_uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];
    const void *fragment_uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];
    void *vertex_uniforms;
    uint32_t vertex_uniform_size;
    void *fragment_uniforms;
    uint32_t fragment_uniform_size;

    float viewport_x_offset, viewport_x_scale;
    float viewport_y_offset, viewport_y_scale;
    float viewport_z_offset, viewport_z_scale;
    SceGxmViewportMode viewport_enable;
    SceGxmRegionClipMode region_clip_mode;
    unsigned int region_clip_xmin, region_clip_ymin, region_clip_xmax, region_clip_ymax;
    SceGxmCullMode cull_mode;
    SceGxmPolygonMode front_polygon_mode;
    SceGxmPolygonMode back_polygon_mode;
    unsigned int front_point_line_width;
    int front_depth_bias_factor, front_depth_bias_units;
    SceGxmTwoSidedMode two_sided;
    SceGxmFragmentProgramMode front_fragment_program_enable;
    SceGxmFragmentProgramMode back_fragment_program_enable;

    SceGxmDepthFunc depth_func;
    SceGxmDepthWriteMode depth_write;
    SceGxmStencilFunc front_stencil_func;
    SceGxmStencilOp front_stencil_fail, front_stencil_depth_fail, front_stencil_pass;
    uint8_t front_stencil_compare_mask, front_stencil_write_mask;
    SceGxmStencilFunc back_stencil_func;
    SceGxmStencilOp back_stencil_fail, back_stencil_depth_fail, back_stencil_pass;
    uint8_t back_stencil_compare_mask, back_stencil_write_mask;
    unsigned int front_stencil_ref, back_stencil_ref;

    SceGxmPrimitiveType prim_type;
    SceGxmIndexFormat index_type;
    const void *index_data;
    unsigned int index_count;
    unsigned int instances;
} GxmDrawCommand;

struct GxmCommand {
    uint32_t type;
    GxmDrawCommand draw;
    struct GxmCommand *next;
};

#define GXM_COMMAND_DRAW 1
#define GXM_MAX_COMMAND_LISTS 64
#define GXM_COMMAND_LIST_MAGIC 0x4C53584Du /* "LXSL" */

static bool g_gxm_initialized;
static HostGpuDevice g_device;
static HostGles *g_gl;
static uint32_t *g_notification_region;
static GxmLinkedProgram g_linked[GXM_MAX_LINKED_PROGRAMS];
static unsigned int g_linked_count;
static SceGxmDisplayQueueCallback *g_display_callback;
static uint32_t g_display_callback_data_size;

typedef struct {
    SceGxmCommandList *guest;
    GxmCommand *head;
    GxmCommand *tail;
    uint32_t index;
} GxmCommandListEntry;

static GxmCommandListEntry g_command_lists[GXM_MAX_COMMAND_LISTS];

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
    if (vs && vs->shader)
        g_gl->glAttachShader(program, vs->shader);
    if (fs && fs->shader)
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

static uint32_t gxm_hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;

    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }

    return hash;
}

static uint32_t gxm_texture_hash(const SceGxmTextureInner *texture, const void *data)
{
    SceGxmTextureFormat format = gxm_texture_get_format(texture);
    SceGxmTextureType type = (SceGxmTextureType)gxm_texture_get_type(texture);
    uint32_t base = (uint32_t)(format & SCE_GXM_TEXTURE_BASE_FORMAT_MASK);
    uint32_t hash = 2166136261u;
    uint32_t faces = (type == SCE_GXM_TEXTURE_CUBE || type == SCE_GXM_TEXTURE_CUBE_ARBITRARY) ? 6
                                                                                            : 1;

    hash = gxm_hash_bytes(hash, &format, sizeof(format));
    hash = gxm_hash_bytes(hash, &type, sizeof(type));

    for (uint32_t face = 0; face < faces; face++) {
        GxmTextureLevel level;

        if (gxm_texture_get_level(texture, face, 0, &level) != 0)
            return 0;
        hash = gxm_hash_bytes(hash, (const uint8_t *)data + level.offset, level.size);
    }

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_P4 || base == SCE_GXM_TEXTURE_BASE_FORMAT_P8) {
        const uint32_t *palette = (const uint32_t *)(uintptr_t)(texture->palette_addr << 6);
        uint32_t entries = base == SCE_GXM_TEXTURE_BASE_FORMAT_P4 ? 16 : 256;

        if (!palette)
            return 0;
        hash = gxm_hash_bytes(hash, palette, entries * sizeof(uint32_t));
    }

    return hash;
}

static GLenum gxm_wrap_mode(unsigned int mode)
{
    switch (mode) {
    case SCE_GXM_TEXTURE_ADDR_REPEAT:
    case SCE_GXM_TEXTURE_ADDR_REPEAT_IGNORE_BORDER:
        return GL_REPEAT;
    case SCE_GXM_TEXTURE_ADDR_MIRROR:
        return GL_MIRRORED_REPEAT;
    default:
        return GL_CLAMP_TO_EDGE;
    }
}

static GLenum gxm_min_filter(const SceGxmTextureInner *texture, uint32_t mip_count)
{
    bool mipmapped = mip_count > 1;

    switch (texture->min_filter) {
    case SCE_GXM_TEXTURE_FILTER_LINEAR:
        return GL_LINEAR;
    case SCE_GXM_TEXTURE_FILTER_MIPMAP_LINEAR:
        if (!mipmapped)
            return GL_LINEAR;
        return texture->mip_filter ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
    case SCE_GXM_TEXTURE_FILTER_MIPMAP_POINT:
        return mipmapped ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    case SCE_GXM_TEXTURE_FILTER_POINT:
    default:
        return GL_NEAREST;
    }
}

static void gxm_texture_params(const SceGxmTextureInner *texture, GLenum target, uint32_t mip_count)
{
    g_gl->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)gxm_min_filter(texture, mip_count));
    g_gl->glTexParameteri(target, GL_TEXTURE_MAG_FILTER,
                          texture->mag_filter == SCE_GXM_TEXTURE_FILTER_LINEAR ? GL_LINEAR
                                                                                : GL_NEAREST);
    g_gl->glTexParameteri(target, GL_TEXTURE_WRAP_S, (GLint)gxm_wrap_mode(texture->uaddr_mode));
    g_gl->glTexParameteri(target, GL_TEXTURE_WRAP_T, (GLint)gxm_wrap_mode(texture->vaddr_mode));
    if (target == GL_TEXTURE_CUBE_MAP)
        g_gl->glTexParameteri(target, GL_TEXTURE_WRAP_R, (GLint)gxm_wrap_mode(texture->vaddr_mode));

    g_gl->glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, mip_count > 0 ? (GLint)mip_count - 1 : 0);
    g_gl->glTexParameterf(target, GL_TEXTURE_MIN_LOD,
                          (float)(texture->lod_min0 | (texture->lod_min1 << 2)));
}

static bool gxm_upload_texture(GxmTextureCache *cache, const SceGxmTextureInner *texture,
                               unsigned int unit)
{
    SceGxmTextureType type = (SceGxmTextureType)gxm_texture_get_type(texture);
    bool cube = type == SCE_GXM_TEXTURE_CUBE || type == SCE_GXM_TEXTURE_CUBE_ARBITRARY;
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    const void *data = gxm_texture_get_data(texture);
    uint32_t width = (uint32_t)gxm_texture_get_width(texture);
    uint32_t height = (uint32_t)gxm_texture_get_height(texture);
    uint32_t mip_count = gxm_texture_true_mip_count(texture);
    uint32_t hash;
    uint8_t *rgba;

    g_gl->glActiveTexture(GL_TEXTURE0 + unit);

    if (!data || !width || !height || width > 4096 || height > 4096) {
        g_gl->glBindTexture(GL_TEXTURE_2D, 0);
        g_gl->glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        cache->valid = false;
        return false;
    }

    hash = gxm_texture_hash(texture, data);

    if (cache->tex && cache->target != target) {
        g_gl->glDeleteTextures(1, &cache->tex);
        cache->tex = 0;
        cache->valid = false;
    }

    if (!cache->tex) {
        g_gl->glGenTextures(1, &cache->tex);
        cache->target = target;
        cache->valid = false;
    }

    g_gl->glBindTexture(target, cache->tex);

    if (cache->valid && cache->hash == hash && cache->data == data &&
        cache->format == (uint32_t)gxm_texture_get_format(texture) &&
        cache->mip_count == mip_count && cache->target == target) {
        gxm_texture_params(texture, target, mip_count);
        return true;
    }

    rgba = malloc((size_t)width * height * 4);
    if (!rgba)
        return false;

    for (uint32_t face = 0; face < (cube ? 6u : 1u); face++) {
        for (uint32_t level = 0; level < mip_count; level++) {
            GxmTextureLevel info;
            GLenum upload_target = target;

            if (gxm_texture_get_level(texture, face, level, &info) != 0)
                continue;
            if (gxm_texture_decode_level_at(rgba, texture, data, face, level) != 0) {
                static uint32_t warned_formats[8];
                uint32_t format = (uint32_t)gxm_texture_get_format(texture);
                bool warned = false;

                for (uint32_t i = 0; i < ARRAY_SIZE(warned_formats); i++) {
                    if (warned_formats[i] == format)
                        warned = true;
                }
                if (!warned) {
                    for (uint32_t i = 0; i < ARRAY_SIZE(warned_formats); i++) {
                        if (!warned_formats[i]) {
                            warned_formats[i] = format;
                            break;
                        }
                    }
                    LOG("SceGxm: unsupported texture format 0x%08x, sampling black", format);
                }
                free(rgba);
                g_gl->glBindTexture(target, 0);
                cache->valid = false;
                return false;
            }

            if (cube)
                upload_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;

            g_gl->glTexImage2D(upload_target, (GLint)level, GL_RGBA8, (GLsizei)info.width,
                               (GLsizei)info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }

    free(rgba);

    gxm_texture_params(texture, target, mip_count);

    cache->data = data;
    cache->palette = (const void *)(uintptr_t)(texture->palette_addr << 6);
    cache->width = width;
    cache->height = height;
    cache->format = (uint32_t)gxm_texture_get_format(texture);
    cache->mip_count = mip_count;
    cache->hash = hash;
    cache->valid = true;

    return true;
}

static void gxm_upload_textures(SceGxmContext *context)
{
    for (unsigned int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        gxm_upload_texture(&context->textures[0][i], &context->fragment_textures[i], i);
        gxm_upload_texture(&context->textures[1][i], &context->vertex_textures[i],
                           SCE_GXM_MAX_TEXTURE_UNITS + i);
    }
}

static void gxm_upload_uniforms(SceGxmContext *context, const SceGxmRegisteredProgram *program,
                                bool vertex)
{
    const void *default_buffer = vertex ? context->vertex_uniforms : context->fragment_uniforms;
    const void *const *buffers =
        vertex ? context->vertex_uniform_buffers : context->fragment_uniform_buffers;
    uint8_t **data = vertex ? &context->vertex_ssbo_data : &context->fragment_ssbo_data;
    uint32_t *capacity = vertex ? &context->vertex_ssbo_size : &context->fragment_ssbo_size;
    uint32_t total = program ? program->uniform_buffer_size : 0;

    if (!program || !total)
        return;

    if (*capacity != total) {
        free(*data);
        *data = malloc(total);
        *capacity = *data ? total : 0;
    }
    if (!*data)
        return;

    memset(*data, 0, total);

    for (uint32_t i = 0; i < program->uniform_buffer_count; i++) {
        const Vita3kUniformBufferLayout *layout = &program->uniform_buffers[i];
        const void *source = NULL;
        size_t size;

        if (layout->guest_index == 14)
            source = default_buffer;
        else if (layout->guest_index < SCE_GXM_MAX_UNIFORM_BUFFERS)
            source = buffers[layout->guest_index];

        if (!source)
            continue;

        size = layout->guest_size < layout->member_size ? layout->guest_size
                                                        : layout->member_size;
        memcpy(*data + layout->offset, source, size);
    }

    g_gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, context->ssbo[vertex ? 0 : 1]);
    g_gl->glBufferData(GL_SHADER_STORAGE_BUFFER, total, *data, GL_STREAM_DRAW);
    g_gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, vertex ? 0 : 1,
                           context->ssbo[vertex ? 0 : 1]);
}

static void gxm_apply_viewport(SceGxmContext *context)
{
    uint32_t width = context->render_target ? context->render_target->params.width : 960;
    uint32_t height = context->render_target ? context->render_target->params.height : 544;

    if (context->viewport_enable == SCE_GXM_VIEWPORT_ENABLED) {
        float ymin = context->viewport_y_offset + context->viewport_y_scale;
        float ymax = context->viewport_y_offset - context->viewport_y_scale - 1.0f;
        int x = (int)(context->viewport_x_offset - fabsf(context->viewport_x_scale));
        int y = (int)(ymin < ymax ? ymin : ymax);
        int w = (int)fabsf(2.0f * context->viewport_x_scale);
        int h = (int)fabsf(2.0f * context->viewport_y_scale);

        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
        g_gl->glViewport(x, y, w, h);
    } else {
        g_gl->glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    }
}

static void gxm_apply_region_clip(SceGxmContext *context)
{
    uint32_t height = context->render_target ? context->render_target->params.height : 544;
    int x, y, w, h;

    switch (context->region_clip_mode) {
    case SCE_GXM_REGION_CLIP_ALL:
        g_gl->glEnable(GL_SCISSOR_TEST);
        g_gl->glScissor(0, 0, 0, 0);
        return;
    case SCE_GXM_REGION_CLIP_OUTSIDE:
        break;
    case SCE_GXM_REGION_CLIP_INSIDE:
        /* GLES cannot invert a scissor rectangle; approximate as disabled. */
    case SCE_GXM_REGION_CLIP_NONE:
    default:
        g_gl->glDisable(GL_SCISSOR_TEST);
        return;
    }

    x = (int)context->region_clip_xmin;
    y = (int)(height - context->region_clip_ymax - 1);
    w = (int)(context->region_clip_xmax - context->region_clip_xmin + 1);
    h = (int)(context->region_clip_ymax - context->region_clip_ymin + 1);

    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;

    g_gl->glEnable(GL_SCISSOR_TEST);
    g_gl->glScissor(x, y, w, h);
}

static void gxm_apply_cull(SceGxmContext *context)
{
    switch (context->cull_mode) {
    case SCE_GXM_CULL_CW:
        g_gl->glEnable(GL_CULL_FACE);
        g_gl->glCullFace(GL_BACK);
        break;
    case SCE_GXM_CULL_CCW:
        g_gl->glEnable(GL_CULL_FACE);
        g_gl->glCullFace(GL_FRONT);
        break;
    case SCE_GXM_CULL_NONE:
    default:
        g_gl->glDisable(GL_CULL_FACE);
        break;
    }
}

static void gxm_gl_check(const char *where)
{
    GLenum error = g_gl->glGetError();

    if (error != GL_NO_ERROR)
        LOG("SceGxm: GL error 0x%x at %s", error, where);
}

typedef struct {
    uint32_t depth_bits; /* 0 = no depth, 16/24/32 = storage width */
    bool has_stencil;
    bool packed; /* depth and stencil share one 32-bit word (S8D24) */
} GxmDsLayout;

static GxmDsLayout gxm_ds_layout(SceGxmDepthStencilFormat format);

static void gxm_flush_state(SceGxmContext *context, GLuint program, uint32_t max_index)
{
    const SceGxmVertexProgram *vp = context->vertex_program;
    const SceGxmFragmentProgram *fp = context->fragment_program;
    uint32_t surface_width = context->color_surface && context->color_surface->width
                                 ? context->color_surface->width
                                 : 960;
    uint32_t surface_height = context->color_surface && context->color_surface->height
                                  ? context->color_surface->height
                                  : 544;
    bool both_side_disabled;

    g_gl->glUseProgram(program);

    /* Render-info uniform buffers. */
    {
        struct GXMRenderVertUniformBlock vert = {
            {1.0f, 1.0f, 1.0f, 1.0f},
            context->viewport_enable == SCE_GXM_VIEWPORT_ENABLED ? 1.0f : 0.0f,
            (float)surface_width,
            (float)surface_height,
            context->viewport_enable == SCE_GXM_VIEWPORT_ENABLED ? context->viewport_z_offset
                                                                 : 0.0f,
            context->viewport_enable == SCE_GXM_VIEWPORT_ENABLED ? context->viewport_z_scale
                                                                 : 1.0f,
        };
        both_side_disabled =
            context->front_fragment_program_enable == SCE_GXM_FRAGMENT_PROGRAM_DISABLED &&
            (context->back_fragment_program_enable == SCE_GXM_FRAGMENT_PROGRAM_DISABLED ||
             context->two_sided == SCE_GXM_TWO_SIDED_DISABLED);
        struct GXMRenderFragUniformBlock frag = {
            both_side_disabled
                ? 0.0f
                : (context->front_fragment_program_enable == SCE_GXM_FRAGMENT_PROGRAM_DISABLED
                       ? 1.0f
                       : 0.0f),
            both_side_disabled
                ? 0.0f
                : (context->two_sided == SCE_GXM_TWO_SIDED_DISABLED
                       ? (context->front_fragment_program_enable ==
                                  SCE_GXM_FRAGMENT_PROGRAM_DISABLED
                              ? 1.0f
                              : 0.0f)
                       : (context->back_fragment_program_enable ==
                                  SCE_GXM_FRAGMENT_PROGRAM_DISABLED
                              ? 1.0f
                              : 0.0f)),
            0.0f,
            0.0f,
            1.0f,
        };
        GLuint block;

        if (both_side_disabled)
            g_gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[0]);
        g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(vert), &vert, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 2, context->ubo[0]);

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[1]);
        g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(frag), &frag, GL_STREAM_DRAW);
        g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 3, context->ubo[1]);

        block = g_gl->glGetUniformBlockIndex(program, "renderVertInfo");
        if (block != GL_INVALID_INDEX)
            g_gl->glUniformBlockBinding(program, block, 2);
        block = g_gl->glGetUniformBlockIndex(program, "renderFragInfo");
        if (block != GL_INVALID_INDEX)
            g_gl->glUniformBlockBinding(program, block, 3);
    }

    /* Default uniforms and named uniform buffers share one SSBO per stage. */
    if (vp)
        gxm_upload_uniforms(context, vp->programId, true);
    if (fp)
        gxm_upload_uniforms(context, fp->programId, false);

    gxm_apply_viewport(context);
    gxm_apply_region_clip(context);
    gxm_apply_cull(context);

    if (context->front_depth_bias_factor || context->front_depth_bias_units) {
        g_gl->glEnable(GL_POLYGON_OFFSET_FILL);
        g_gl->glPolygonOffset((GLfloat)context->front_depth_bias_factor,
                              (GLfloat)context->front_depth_bias_units);
    } else {
        g_gl->glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (context->front_point_line_width > 1)
        g_gl->glLineWidth((GLfloat)context->front_point_line_width);
    else
        g_gl->glLineWidth(1.0f);

    /* Depth and stencil. */
    if (context->ds_surface) {
        GxmDsLayout layout = gxm_ds_layout(gxm_ds_surface_get_format(context->ds_surface));

        if (layout.depth_bits) {
            g_gl->glEnable(GL_DEPTH_TEST);
            g_gl->glDepthFunc(gxm_depth_func_to_gl(context->depth_func));
            g_gl->glDepthMask(context->depth_write == SCE_GXM_DEPTH_WRITE_ENABLED ? GL_TRUE
                                                                                  : GL_FALSE);
        } else {
            g_gl->glDisable(GL_DEPTH_TEST);
            g_gl->glDepthMask(GL_FALSE);
        }

        if (layout.has_stencil) {
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
            g_gl->glDisable(GL_STENCIL_TEST);
        }
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
        if (both_side_disabled)
            g_gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        else
            g_gl->glColorMask((b->colorMask & SCE_GXM_COLOR_MASK_R) ? GL_TRUE : GL_FALSE,
                              (b->colorMask & SCE_GXM_COLOR_MASK_G) ? GL_TRUE : GL_FALSE,
                              (b->colorMask & SCE_GXM_COLOR_MASK_B) ? GL_TRUE : GL_FALSE,
                              (b->colorMask & SCE_GXM_COLOR_MASK_A) ? GL_TRUE : GL_FALSE);
    }

    gxm_upload_textures(context);

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
            s = &vp->streams[a->streamIndex];
            stream = &context->streams[a->streamIndex];
            if (location < 0)
                continue;

            if (!stream->data || !s->stride)
                continue;

            stream->size = (max_index + 1) * s->stride;
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
    if (!notification || !notification->address)
        return SCE_GXM_ERROR_INVALID_POINTER;

    while (*notification->address != notification->value)
        host_sleep_ns(1000000);

    return 0;
}

static void gxm_context_init_defaults(SceGxmContext *ctx, bool deferred)
{
    ctx->deferred = deferred;
    ctx->vertex_uniform_arena = malloc(GXM_UNIFORM_ARENA_SIZE);
    ctx->fragment_uniform_arena = malloc(GXM_UNIFORM_ARENA_SIZE);

    if (!deferred && g_gl && g_gl->glGenBuffers) {
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

    /* Defaults matching a 960x544 full-surface viewport. Applications normally
     * call sceGxmSetDefaultRegionClipAndViewport right after BeginScene. */
    ctx->viewport_enable = SCE_GXM_VIEWPORT_ENABLED;
    ctx->viewport_x_offset = 480.0f;
    ctx->viewport_x_scale = 480.0f;
    ctx->viewport_y_offset = 272.0f;
    ctx->viewport_y_scale = -272.0f;
    ctx->viewport_z_offset = 0.5f;
    ctx->viewport_z_scale = 0.5f;
    ctx->region_clip_mode = SCE_GXM_REGION_CLIP_NONE;
    ctx->region_clip_xmin = 0;
    ctx->region_clip_ymin = 0;
    ctx->region_clip_xmax = 959;
    ctx->region_clip_ymax = 543;
    ctx->cull_mode = SCE_GXM_CULL_NONE;
    ctx->front_polygon_mode = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    ctx->back_polygon_mode = SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    ctx->front_point_line_width = 1;
    ctx->back_point_line_width = 1;
    ctx->w_clamp_value = 1.0f;
    ctx->w_clamp_enable = SCE_GXM_WCLAMP_MODE_DISABLED;
    ctx->two_sided = SCE_GXM_TWO_SIDED_DISABLED;
    ctx->front_fragment_program_enable = SCE_GXM_FRAGMENT_PROGRAM_ENABLED;
    ctx->back_fragment_program_enable = SCE_GXM_FRAGMENT_PROGRAM_ENABLED;
}

EXPORT(SceGxm, 0xE84CE5B4, int, sceGxmCreateContext, const SceGxmContextParams *params,
       SceGxmContext **context)
{
    SceGxmContext *ctx;

    if (!params || !context || !params->hostMem || params->hostMemSize < sizeof(SceGxmContext))
        return SCE_GXM_ERROR_INVALID_VALUE;

    ctx = params->hostMem;
    memset(ctx, 0, sizeof(*ctx));
    ctx->params = *params;
    gxm_context_init_defaults(ctx, false);

    if (!ctx->vertex_uniform_arena || !ctx->fragment_uniform_arena)
        goto oom;

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

    if (g_gl && !context->deferred) {
        for (unsigned int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
            if (context->textures[0][i].tex)
                g_gl->glDeleteTextures(1, &context->textures[0][i].tex);
            if (context->textures[1][i].tex)
                g_gl->glDeleteTextures(1, &context->textures[1][i].tex);
        }
        for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++)
            if (context->streams[i].vbo)
                g_gl->glDeleteBuffers(1, &context->streams[i].vbo);
        if (context->ssbo[0] || context->ssbo[1])
            g_gl->glDeleteBuffers(2, context->ssbo);
        if (context->ubo[0] || context->ubo[1])
            g_gl->glDeleteBuffers(2, context->ubo);
        if (context->ibo)
            g_gl->glDeleteBuffers(1, &context->ibo);
        if (context->wire_ibo)
            g_gl->glDeleteBuffers(1, &context->wire_ibo);
    }

    free(context->vertex_ssbo_data);
    free(context->fragment_ssbo_data);
    free(context->vertex_uniform_arena);
    free(context->fragment_uniform_arena);
    gxm_command_free(context->cmd_head);
    context->cmd_head = NULL;
    context->cmd_tail = NULL;
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
    for (uint32_t i = 0; i < shaderPatcher->registered_count; i++)
        free(shaderPatcher->registered_programs[i]);
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
    registered->refcount = 1;
    registered->uniform_buffer_size =
        vita3k_gxm_get_uniform_buffer_layout(programHeader, registered->uniform_buffers,
                                             ARRAY_SIZE(registered->uniform_buffers));
    while (registered->uniform_buffer_count < ARRAY_SIZE(registered->uniform_buffers) &&
           registered->uniform_buffers[registered->uniform_buffer_count].member_size != 0)
        registered->uniform_buffer_count++;
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
    for (uint32_t i = 0; i < shaderPatcher->registered_count; i++) {
        if (shaderPatcher->registered_programs[i] == programId) {
            free(programId);
            for (uint32_t j = i; j + 1 < shaderPatcher->registered_count; j++)
                shaderPatcher->registered_programs[j] = shaderPatcher->registered_programs[j + 1];
            shaderPatcher->registered_count--;
            return 0;
        }
    }

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
    vp->refcount = 1;
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
    if (!vertexProgram)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (--vertexProgram->refcount > 0)
        return 0;
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
    fp->refcount = 1;
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
    if (!fragmentProgram)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (--fragmentProgram->refcount > 0)
        return 0;
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

        g_gl->glGenFramebuffers(1, &rt->fbo);
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->color,
                                     0);

        /*
         * A depth/stencil *texture* lets sceGxmBeginScene upload guest depth
         * data (glTexSubImage2D); a renderbuffer would be write-only. Some
         * GLES 3.0 drivers do not allow packed depth/stencil textures, so
         * fall back to a renderbuffer when the framebuffer is incomplete.
         */
        while (g_gl->glGetError() != GL_NO_ERROR) {
        }
        g_gl->glGenTextures(1, &rt->depth);
        g_gl->glBindTexture(GL_TEXTURE_2D, rt->depth);
        g_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, params->width, params->height, 0,
                           GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                     rt->depth, 0);
        if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
            g_gl->glGetError() == GL_NO_ERROR) {
            rt->depth_is_texture = true;
        } else {
            g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                         0, 0);
            g_gl->glDeleteTextures(1, &rt->depth);
            rt->depth = 0;

            g_gl->glGenRenderbuffers(1, &rt->depth_rb);
            g_gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->depth_rb);
            g_gl->glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, params->width,
                                        params->height);
            g_gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                            GL_RENDERBUFFER, rt->depth_rb);
            if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                LOG("SceGxm: render target framebuffer incomplete");
        }
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
            g_gl->glDeleteTextures(1, &renderTarget->depth);
        if (renderTarget->depth_rb)
            g_gl->glDeleteRenderbuffers(1, &renderTarget->depth_rb);
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
    inner->clipXMin = 0;
    inner->clipYMin = 0;
    inner->clipXMax = (uint16_t)(width ? width - 1 : 0);
    inner->clipYMax = (uint16_t)(height ? height - 1 : 0);
    return 0;
}

EXPORT(SceGxm, 0x613639FA, int, sceGxmColorSurfaceInitDisabled, SceGxmColorSurface *surface)
{
    SceGxmColorSurfaceInner *inner = (SceGxmColorSurfaceInner *)surface;

    if (!surface)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memset(inner, 0, sizeof(*inner));
    inner->disabled = 1;
    return 0;
}

EXPORT(SceGxm, 0x0E0EBB57, SceBool, sceGxmColorSurfaceIsEnabled,
       const SceGxmColorSurface *surface)
{
    return surface && !((const SceGxmColorSurfaceInner *)surface)->disabled;
}

EXPORT(SceGxm, 0x07DFEE4B, void, sceGxmColorSurfaceGetClip, const SceGxmColorSurface *surface,
       unsigned int *xMin, unsigned int *yMin, unsigned int *xMax, unsigned int *yMax)
{
    const SceGxmColorSurfaceInner *inner = (const SceGxmColorSurfaceInner *)surface;

    if (!inner)
        return;
    if (xMin)
        *xMin = inner->clipXMin;
    if (yMin)
        *yMin = inner->clipYMin;
    if (xMax)
        *xMax = inner->clipXMax;
    if (yMax)
        *yMax = inner->clipYMax;
}

EXPORT(SceGxm, 0x86456F7B, void, sceGxmColorSurfaceSetClip, SceGxmColorSurface *surface,
       unsigned int xMin, unsigned int yMin, unsigned int xMax, unsigned int yMax)
{
    SceGxmColorSurfaceInner *inner = (SceGxmColorSurfaceInner *)surface;

    if (!inner)
        return;
    inner->clipXMin = (uint16_t)xMin;
    inner->clipYMin = (uint16_t)yMin;
    inner->clipXMax = (uint16_t)xMax;
    inner->clipYMax = (uint16_t)yMax;
}

EXPORT(SceGxm, 0x6E3FA74D, SceGxmColorSurfaceScaleMode, sceGxmColorSurfaceGetScaleMode,
       const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->downscale
               ? SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE
               : SCE_GXM_COLOR_SURFACE_SCALE_NONE;
}

EXPORT(SceGxm, 0x6B96EDF7, void, sceGxmColorSurfaceSetScaleMode, SceGxmColorSurface *surface,
       SceGxmColorSurfaceScaleMode scaleMode)
{
    ((SceGxmColorSurfaceInner *)surface)->downscale =
        scaleMode == SCE_GXM_COLOR_SURFACE_SCALE_MSAA_DOWNSCALE;
}

EXPORT(SceGxm, 0x2DB6026C, void *, sceGxmColorSurfaceGetData, const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->data;
}

EXPORT(SceGxm, 0x537CA400, int, sceGxmColorSurfaceSetData, SceGxmColorSurface *surface, void *data)
{
    ((SceGxmColorSurfaceInner *)surface)->data = data;
    return 0;
}

EXPORT(SceGxm, 0xF3C1C6C6, SceGxmColorFormat, sceGxmColorSurfaceGetFormat,
       const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->colorFormat;
}

EXPORT(SceGxm, 0x5F9A3A16, int, sceGxmColorSurfaceSetFormat, SceGxmColorSurface *surface,
       SceGxmColorFormat format)
{
    ((SceGxmColorSurfaceInner *)surface)->colorFormat = format;
    return 0;
}

EXPORT(SceGxm, 0x52FDE962, SceGxmColorSurfaceType, sceGxmColorSurfaceGetType,
       const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->surfaceType;
}

EXPORT(SceGxm, 0xF33D9980, unsigned int, sceGxmColorSurfaceGetStrideInPixels,
       const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->strideInPixels;
}

EXPORT(SceGxm, 0xEE0B4DF0, SceGxmColorSurfaceGammaMode, sceGxmColorSurfaceGetGammaMode,
       const SceGxmColorSurface *surface)
{
    return (SceGxmColorSurfaceGammaMode)(
        ((const SceGxmColorSurfaceInner *)surface)->gamma_mode << 12);
}

EXPORT(SceGxm, 0xF5C89643, int, sceGxmColorSurfaceSetGammaMode, SceGxmColorSurface *surface,
       SceGxmColorSurfaceGammaMode gammaMode)
{
    ((SceGxmColorSurfaceInner *)surface)->gamma_mode = (gammaMode >> 12) & 3;
    return 0;
}

EXPORT(SceGxm, 0x200A96E1, SceGxmColorSurfaceDitherMode, sceGxmColorSurfaceGetDitherMode,
       const SceGxmColorSurface *surface)
{
    return ((const SceGxmColorSurfaceInner *)surface)->dither_mode
               ? SCE_GXM_COLOR_SURFACE_DITHER_ENABLED
               : SCE_GXM_COLOR_SURFACE_DITHER_DISABLED;
}

EXPORT(SceGxm, 0x45027BAB, int, sceGxmColorSurfaceSetDitherMode, SceGxmColorSurface *surface,
       SceGxmColorSurfaceDitherMode ditherMode)
{
    ((SceGxmColorSurfaceInner *)surface)->dither_mode =
        ditherMode == SCE_GXM_COLOR_SURFACE_DITHER_ENABLED ? 1 : 0;
    return 0;
}

EXPORT(SceGxm, 0xCA9D41D1, int, sceGxmDepthStencilSurfaceInit, SceGxmDepthStencilSurface *surface,
       SceGxmDepthStencilFormat depthStencilFormat, SceGxmDepthStencilSurfaceType surfaceType,
       unsigned int strideInSamples, void *depthData, void *stencilData)
{
    uint32_t type_format;

    if (!surface)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!strideInSamples || (strideInSamples & 31u))
        return SCE_GXM_ERROR_INVALID_VALUE;

    type_format = ((uint32_t)surfaceType >> 12) & SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_MASK;
    type_format |= SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_ALWAYS_SET_BIT;
    type_format |= ((uint32_t)depthStencilFormat >> 12) &
                   SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORMAT_MASK;

    memset(surface, 0, sizeof(*surface));
    surface->zlsControl =
        SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_ALWAYS_BIT |
        ((((strideInSamples >> 5) - 1) & SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_STRIDE_MASK)
         << SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_STRIDE_OFFSET) |
        (type_format << SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_FORMAT_OFFSET);
    surface->depthData = depthData;
    surface->stencilData = stencilData;
    surface->backgroundDepth = 1.0f;
    surface->backgroundControl =
        SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT | SCE_GXM_DEPTH_STENCIL_BG_CTRL_UNK_BIT;
    return 0;
}

EXPORT(SceGxm, 0xA41DB0D6, int, sceGxmDepthStencilSurfaceInitDisabled,
       SceGxmDepthStencilSurface *surface)
{
    if (!surface)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memset(surface, 0, sizeof(*surface));
    surface->backgroundDepth = 1.0f;
    surface->backgroundControl =
        SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT | SCE_GXM_DEPTH_STENCIL_BG_CTRL_UNK_BIT;
    return 0;
}

EXPORT(SceGxm, 0x082200E1, SceBool, sceGxmDepthStencilSurfaceIsEnabled,
       const SceGxmDepthStencilSurface *surface)
{
    return surface && !gxm_ds_surface_is_disabled(surface);
}

EXPORT(SceGxm, 0x269B56BE, float, sceGxmDepthStencilSurfaceGetBackgroundDepth,
       const SceGxmDepthStencilSurface *surface)
{
    return surface->backgroundDepth;
}

EXPORT(SceGxm, 0x32F280F0, void, sceGxmDepthStencilSurfaceSetBackgroundDepth,
       SceGxmDepthStencilSurface *surface, float depth)
{
    surface->backgroundDepth = depth;
}

EXPORT(SceGxm, 0xAAFC062B, unsigned char, sceGxmDepthStencilSurfaceGetBackgroundStencil,
       const SceGxmDepthStencilSurface *surface)
{
    return (unsigned char)(surface->backgroundControl & 0xff);
}

EXPORT(SceGxm, 0xF5D3F3E8, void, sceGxmDepthStencilSurfaceSetBackgroundStencil,
       SceGxmDepthStencilSurface *surface, unsigned char stencil)
{
    surface->backgroundControl =
        (surface->backgroundControl & ~0xffu) | (unsigned int)stencil;
}

EXPORT(SceGxm, 0x85D4DEFE, SceBool, sceGxmDepthStencilSurfaceGetBackgroundMask,
       const SceGxmDepthStencilSurface *surface)
{
    return (surface->backgroundControl & SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT) ? 1 : 0;
}

EXPORT(SceGxm, 0xF28A688A, void, sceGxmDepthStencilSurfaceSetBackgroundMask,
       SceGxmDepthStencilSurface *surface, SceBool mask)
{
    if (mask)
        surface->backgroundControl |= SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT;
    else
        surface->backgroundControl &= ~SCE_GXM_DEPTH_STENCIL_BG_CTRL_MASK_BIT;
}

EXPORT(SceGxm, 0x0C44ACD7, void, sceGxmDepthStencilSurfaceSetForceLoadMode,
       SceGxmDepthStencilSurface *surface, SceGxmDepthStencilForceLoadMode forceLoad)
{
    surface->zlsControl = (surface->zlsControl & ~0x2u) |
                          ((unsigned int)forceLoad & 0x2u);
}

EXPORT(SceGxm, 0x2F5CC20C, SceGxmDepthStencilForceLoadMode,
       sceGxmDepthStencilSurfaceGetForceLoadMode, const SceGxmDepthStencilSurface *surface)
{
    return (SceGxmDepthStencilForceLoadMode)(surface->zlsControl & 0x2u);
}

EXPORT(SceGxm, 0x12AAA7AF, void, sceGxmDepthStencilSurfaceSetForceStoreMode,
       SceGxmDepthStencilSurface *surface, SceGxmDepthStencilForceStoreMode forceStore)
{
    surface->zlsControl = (surface->zlsControl & ~0x4u) |
                          ((unsigned int)forceStore & 0x4u);
}

EXPORT(SceGxm, 0x544AA05A, SceGxmDepthStencilForceStoreMode,
       sceGxmDepthStencilSurfaceGetForceStoreMode, const SceGxmDepthStencilSurface *surface)
{
    return (SceGxmDepthStencilForceStoreMode)(surface->zlsControl & 0x4u);
}

EXPORT(SceGxm, 0x8504038D, SceGxmDepthStencilFormat, sceGxmDepthStencilSurfaceGetFormat,
       const SceGxmDepthStencilSurface *surface)
{
    return gxm_ds_surface_get_format(surface);
}

EXPORT(SceGxm, 0x11628789, unsigned int, sceGxmDepthStencilSurfaceGetStrideInSamples,
       const SceGxmDepthStencilSurface *surface)
{
    return gxm_ds_surface_get_stride_in_samples(surface);
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

EXPORT(SceGxm, 0x3EB3380B, void, sceGxmSetViewport, SceGxmContext *context, float xOffset,
       float xScale, float yOffset, float yScale, float zOffset, float zScale)
{
    context->viewport_x_offset = xOffset;
    context->viewport_x_scale = xScale;
    context->viewport_y_offset = yOffset;
    context->viewport_y_scale = yScale;
    context->viewport_z_offset = zOffset;
    context->viewport_z_scale = zScale;
    context->viewport_enable = SCE_GXM_VIEWPORT_ENABLED;
}

EXPORT(SceGxm, 0x814F61EB, void, sceGxmSetViewportEnable, SceGxmContext *context,
       SceGxmViewportMode enable)
{
    context->viewport_enable = enable;
}

EXPORT(SceGxm, 0x70C86868, void, sceGxmSetRegionClip, SceGxmContext *context,
       SceGxmRegionClipMode mode, unsigned int xMin, unsigned int yMin, unsigned int xMax,
       unsigned int yMax)
{
    context->region_clip_mode = mode;
    context->region_clip_xmin = xMin;
    context->region_clip_ymin = yMin;
    context->region_clip_xmax = xMax;
    context->region_clip_ymax = yMax;
}

EXPORT(SceGxm, 0x60CF708E, void, sceGxmSetDefaultRegionClipAndViewport, SceGxmContext *context,
       unsigned int xMax, unsigned int yMax)
{
    context->viewport_x_offset = 0.5f * (1.0f + (float)xMax);
    context->viewport_y_offset = 0.5f * (1.0f + (float)yMax);
    context->viewport_z_offset = 0.5f;
    context->viewport_x_scale = 0.5f * (1.0f + (float)xMax);
    context->viewport_y_scale = -0.5f * (1.0f + (float)yMax);
    context->viewport_z_scale = 0.5f;
    context->viewport_enable = SCE_GXM_VIEWPORT_ENABLED;

    context->region_clip_mode = SCE_GXM_REGION_CLIP_OUTSIDE;
    context->region_clip_xmin = 0;
    context->region_clip_ymin = 0;
    context->region_clip_xmax = xMax;
    context->region_clip_ymax = yMax;
}

EXPORT(SceGxm, 0xE1CA72AE, void, sceGxmSetCullMode, SceGxmContext *context,
       SceGxmCullMode mode)
{
    context->cull_mode = mode;
}

EXPORT(SceGxm, 0xFD93209D, void, sceGxmSetFrontPolygonMode, SceGxmContext *context,
       SceGxmPolygonMode mode)
{
    context->front_polygon_mode = mode;
}

EXPORT(SceGxm, 0xF66EC6FE, void, sceGxmSetBackPolygonMode, SceGxmContext *context,
       SceGxmPolygonMode mode)
{
    context->back_polygon_mode = mode;
}

EXPORT(SceGxm, 0x0DE9AEB7, void, sceGxmSetTwoSidedEnable, SceGxmContext *context,
       SceGxmTwoSidedMode enable)
{
    context->two_sided = enable;
}

EXPORT(SceGxm, 0x06752183, void, sceGxmSetFrontPointLineWidth, SceGxmContext *context,
       unsigned int width)
{
    context->front_point_line_width = width;
}

EXPORT(SceGxm, 0x8DCB0EDB, void, sceGxmSetBackPointLineWidth, SceGxmContext *context,
       unsigned int width)
{
    context->back_point_line_width = width;
}

EXPORT(SceGxm, 0xAAA97F81, void, sceGxmSetFrontDepthBias, SceGxmContext *context, int factor,
       int units)
{
    context->front_depth_bias_factor = factor;
    context->front_depth_bias_units = units;
}

EXPORT(SceGxm, 0x17B3BF86, void, sceGxmSetBackDepthBias, SceGxmContext *context, int factor,
       int units)
{
    context->back_depth_bias_factor = factor;
    context->back_depth_bias_units = units;
}

EXPORT(SceGxm, 0xD096336E, void, sceGxmSetWClampValue, SceGxmContext *context, float clampValue)
{
    context->w_clamp_value = clampValue;
}

EXPORT(SceGxm, 0x1BF8B853, void, sceGxmSetWClampEnable, SceGxmContext *context,
       SceGxmWClampMode enable)
{
    context->w_clamp_enable = enable;
}

EXPORT(SceGxm, 0x575958A8, void, sceGxmSetFrontFragmentProgramEnable, SceGxmContext *context,
       SceGxmFragmentProgramMode enable)
{
    context->front_fragment_program_enable = enable;
}

EXPORT(SceGxm, 0xE26B4834, void, sceGxmSetBackFragmentProgramEnable, SceGxmContext *context,
       SceGxmFragmentProgramMode enable)
{
    context->back_fragment_program_enable = enable;
}

EXPORT(SceGxm, 0x5765DE9F, void, sceGxmSetFrontLineFillLastPixelEnable, SceGxmContext *context,
       SceGxmLineFillLastPixelMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0xC88EB702, void, sceGxmSetBackLineFillLastPixelEnable, SceGxmContext *context,
       SceGxmLineFillLastPixelMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0xEED86975, void, sceGxmSetWBufferEnable, SceGxmContext *context,
       SceGxmWBufferMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0x7767EC49, int, sceGxmSetVisibilityBuffer, SceGxmContext *context,
       void *bufferBase, unsigned int stridePerCore)
{
    (void)stridePerCore;
    if (!context || !bufferBase)
        return SCE_GXM_ERROR_INVALID_POINTER;
    /* The GLES backend cannot use the Vita visibility buffer; mark all visible. */
    memset(bufferBase, 0xff, 4 * stridePerCore);
    return 0;
}

EXPORT(SceGxm, 0x30459117, void, sceGxmSetFrontVisibilityTestEnable, SceGxmContext *context,
       SceGxmVisibilityTestMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0x17CF46B9, void, sceGxmSetBackVisibilityTestEnable, SceGxmContext *context,
       SceGxmVisibilityTestMode enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0x12625C34, void, sceGxmSetFrontVisibilityTestIndex, SceGxmContext *context,
       unsigned int index)
{
    (void)context;
    (void)index;
}

EXPORT(SceGxm, 0xAE7886FE, void, sceGxmSetBackVisibilityTestIndex, SceGxmContext *context,
       unsigned int index)
{
    (void)context;
    (void)index;
}

EXPORT(SceGxm, 0xD0E3CD9A, void, sceGxmSetFrontVisibilityTestOp, SceGxmContext *context,
       SceGxmVisibilityTestOp op)
{
    (void)context;
    (void)op;
}

EXPORT(SceGxm, 0xC83F0AB3, void, sceGxmSetBackVisibilityTestOp, SceGxmContext *context,
       SceGxmVisibilityTestOp op)
{
    (void)context;
    (void)op;
}

EXPORT(SceGxm, 0xB32917F0, int, sceGxmSetYuvProfile, SceGxmContext *context,
       unsigned int index, SceGxmYuvProfile profile)
{
    (void)context;
    (void)index;
    (void)profile;
    return 0;
}

EXPORT(SceGxm, 0x8C6A24C9, void, sceGxmSetValidationEnable, SceGxmContext *context,
       SceBool enable)
{
    (void)context;
    (void)enable;
}

EXPORT(SceGxm, 0x3276C475, int, sceGxmPushUserMarker, SceGxmContext *context, const char *tag)
{
    (void)context;
    LOG("SceGxm: marker %s", tag ? tag : "");
    return 0;
}

EXPORT(SceGxm, 0x4FA073A6, int, sceGxmPopUserMarker, SceGxmContext *context)
{
    (void)context;
    return 0;
}

EXPORT(SceGxm, 0xC7A8CB77, int, sceGxmSetUserMarker, SceGxmContext *context, const char *tag)
{
    (void)context;
    LOG("SceGxm: marker %s", tag ? tag : "");
    return 0;
}

EXPORT(SceGxm, 0x3D25FCE9, int, sceGxmPadHeartbeat, const SceGxmColorSurface *displaySurface,
       SceGxmSyncObject *displaySyncObject)
{
    (void)displaySurface;
    (void)displaySyncObject;
    return 0;
}

EXPORT(SceGxm, 0x47E06984, int, sceGxmPadTriggerGpuPaTrace, void)
{
    return 0;
}

EXPORT(SceGxm, 0x2B5C0444, int, sceGxmMidSceneFlush, SceGxmContext *context, unsigned int flags,
       SceGxmSyncObject *vertexSyncObject, const SceGxmNotification *vertexNotification)
{
    (void)flags;
    (void)vertexSyncObject;
    (void)vertexNotification;
    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (g_gl)
        g_gl->glFinish();
    return 0;
}

EXPORT(SceGxm, 0x9EB4380F, int, sceGxmSetVertexTexture, SceGxmContext *context,
       unsigned int textureIndex, const SceGxmTexture *texture)
{
    if (textureIndex >= SCE_GXM_MAX_TEXTURE_UNITS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    context->vertex_textures[textureIndex] = *(const SceGxmTextureInner *)texture;
    return 0;
}

EXPORT(SceGxm, 0xC68015E4, int, sceGxmSetVertexUniformBuffer, SceGxmContext *context,
       unsigned int bufferIndex, const void *bufferData)
{
    if (!context || !bufferData)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (bufferIndex >= SCE_GXM_MAX_UNIFORM_BUFFERS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    context->vertex_uniform_buffers[bufferIndex] = bufferData;
    return 0;
}

EXPORT(SceGxm, 0xEA0FC310, int, sceGxmSetFragmentUniformBuffer, SceGxmContext *context,
       unsigned int bufferIndex, const void *bufferData)
{
    if (!context || !bufferData)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (bufferIndex >= SCE_GXM_MAX_UNIFORM_BUFFERS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    context->fragment_uniform_buffers[bufferIndex] = bufferData;
    return 0;
}

EXPORT(SceGxm, 0x91B4F7F4, int, sceGxmSetAuxiliarySurface, SceGxmContext *context,
       unsigned int surfaceIndex, const SceGxmAuxiliarySurface *surface)
{
    (void)context;
    (void)surfaceIndex;
    (void)surface;
    return 0;
}

EXPORT(SceGxm, 0xD0EDAB4C, int, sceGxmRenderTargetGetHostMem, const SceGxmRenderTarget *renderTarget,
       void **hostMem)
{
    if (!renderTarget || !hostMem)
        return SCE_GXM_ERROR_INVALID_POINTER;
    *hostMem = (void *)renderTarget;
    return 0;
}

EXPORT(SceGxm, 0x49553737, int, sceGxmRenderTargetGetDriverMemBlock,
       const SceGxmRenderTarget *renderTarget, SceUID *driverMemBlock)
{
    (void)renderTarget;
    if (driverMemBlock)
        *driverMemBlock = 0;
    return 0;
}

static GxmDsLayout gxm_ds_layout(SceGxmDepthStencilFormat format)
{
    GxmDsLayout layout = { 0, false, false };

    switch (format) {
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M:
        layout.depth_bits = 32;
        break;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_D16:
        layout.depth_bits = 16;
        break;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8:
    case SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8:
        layout.depth_bits = 32;
        layout.has_stencil = true;
        break;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24:
        layout.depth_bits = 24;
        layout.has_stencil = true;
        layout.packed = true;
        break;
    case SCE_GXM_DEPTH_STENCIL_FORMAT_S8:
        layout.has_stencil = true;
        break;
    default:
        break;
    }

    return layout;
}

static uint32_t gxm_depth_to_u24(float value)
{
    if (!(value > 0.0f))
        return 0;
    if (value >= 1.0f)
        return 0xFFFFFFu;
    return (uint32_t)(value * 16777215.0f + 0.5f);
}

/*
 * Uploads the guest depth/stencil surface into the render target's GL depth
 * texture. Only called when the surface asks for a force load. Tiled surfaces
 * are currently interpreted as linear (the tile order is undocumented and
 * emulators ignore the backing store entirely).
 */
static bool gxm_ds_load_surface(SceGxmRenderTarget *rt,
                                const SceGxmDepthStencilSurface *surface)
{
    GxmDsLayout layout;
    SceGxmDepthStencilFormat format;
    uint32_t stride, width, height, bg_stencil;
    const uint8_t *depth_data;
    const uint8_t *stencil_data;
    uint32_t *packed;
    bool loaded = false;

    if (!surface || gxm_ds_surface_is_disabled(surface))
        return false;
    if (!(surface->zlsControl & SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORCE_LOAD_BIT))
        return false;
    if (!surface->depthData && !surface->stencilData)
        return false;
    if (!rt->depth_is_texture) {
        LOG("SceGxm: force load requested without a depth texture render target");
        return false;
    }

    format = gxm_ds_surface_get_format(surface);
    layout = gxm_ds_layout(format);
    if (!layout.depth_bits && !layout.has_stencil)
        return false;

    width = (uint32_t)rt->params.width;
    height = (uint32_t)rt->params.height;
    stride = gxm_ds_surface_get_stride_in_samples(surface);
    if (stride < width)
        stride = width;
    bg_stencil = surface->backgroundControl & SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK;
    depth_data = layout.packed ? (const uint8_t *)(surface->depthData ? surface->depthData
                                                                     : surface->stencilData)
                               : (const uint8_t *)surface->depthData;
    stencil_data = (const uint8_t *)surface->stencilData;

    packed = malloc((size_t)width * height * sizeof(uint32_t));
    if (!packed)
        return false;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t d24 = 0xFFFFFFu;
            uint32_t s8 = bg_stencil;

            if (layout.packed) {
                uint32_t value = ((const uint32_t *)depth_data)[(size_t)y * stride + x];
                d24 = value >> 8;
                s8 = value & 0xFFu;
            } else {
                if (layout.depth_bits == 32 && depth_data) {
                    float value = ((const float *)depth_data)[(size_t)y * stride + x];
                    d24 = gxm_depth_to_u24(value);
                } else if (layout.depth_bits == 16 && depth_data) {
                    uint32_t value = ((const uint16_t *)depth_data)[(size_t)y * stride + x];
                    d24 = (uint32_t)(((uint64_t)value * 0xFFFFFFu + 0x7FFFu) / 0xFFFFu);
                }
                if (layout.has_stencil && stencil_data)
                    s8 = stencil_data[(size_t)y * stride + x];
            }

            packed[(size_t)y * width + x] = (d24 << 8) | s8;
        }
    }

    while (g_gl->glGetError() != GL_NO_ERROR) {
    }
    g_gl->glBindTexture(GL_TEXTURE_2D, rt->depth);
    g_gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_STENCIL,
                          GL_UNSIGNED_INT_24_8, packed);
    loaded = g_gl->glGetError() == GL_NO_ERROR;
    if (!loaded)
        LOG("SceGxm: depth/stencil upload failed");
    g_gl->glBindTexture(GL_TEXTURE_2D, 0);

    free(packed);
    return loaded;
}

/*
 * Reads the render target's depth/stencil back into the guest surface when the
 * surface asks for a force store. The GL origin is bottom-left, like the color
 * readback in sceGxmEndScene, so rows are flipped.
 */
static void gxm_ds_store_surface(SceGxmRenderTarget *rt,
                                 const SceGxmDepthStencilSurface *surface)
{
    GxmDsLayout layout;
    SceGxmDepthStencilFormat format;
    uint32_t stride, width, height;
    uint8_t *depth_data;
    uint8_t *stencil_data;
    uint32_t *packed_values = NULL;
    float *depth_values = NULL;
    bool have_stencil = false;
    bool have_depth = false;

    if (!surface || gxm_ds_surface_is_disabled(surface))
        return;
    if (!(surface->zlsControl & SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORCE_STORE_BIT))
        return;
    if (!surface->depthData && !surface->stencilData)
        return;
    if (!rt->depth_is_texture && !rt->depth_rb)
        return;

    format = gxm_ds_surface_get_format(surface);
    layout = gxm_ds_layout(format);
    if (!layout.depth_bits && !layout.has_stencil)
        return;

    width = (uint32_t)rt->params.width;
    height = (uint32_t)rt->params.height;
    stride = gxm_ds_surface_get_stride_in_samples(surface);
    if (stride < width)
        stride = width;
    depth_data = (uint8_t *)(layout.packed ? (surface->depthData ? surface->depthData
                                                                : surface->stencilData)
                                           : surface->depthData);
    stencil_data = (uint8_t *)surface->stencilData;

    packed_values = malloc((size_t)width * height * sizeof(uint32_t));
    if (!packed_values)
        return;

    while (g_gl->glGetError() != GL_NO_ERROR) {
    }

    /*
     * GLES 3.0 reads packed depth/stencil directly. Some drivers refuse it;
     * in that case retry with depth only and keep the background stencil.
     */
    g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
                       packed_values);
    have_stencil = g_gl->glGetError() == GL_NO_ERROR;
    have_depth = have_stencil;

    if (!have_depth) {
        depth_values = malloc((size_t)width * height * sizeof(float));
        if (depth_values) {
            while (g_gl->glGetError() != GL_NO_ERROR) {
            }
            g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT, GL_FLOAT,
                               depth_values);
            have_depth = g_gl->glGetError() == GL_NO_ERROR;
        }
    }

    if (!have_depth && !have_stencil) {
        free(packed_values);
        free(depth_values);
        return;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = height - 1 - y;

        for (uint32_t x = 0; x < width; x++) {
            size_t src = (size_t)src_y * width + x;
            size_t dst = (size_t)y * stride + x;
            float depth = 1.0f;
            uint32_t d24;
            uint8_t s8 = (uint8_t)(surface->backgroundControl &
                                   SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK);

            if (have_stencil) {
                d24 = packed_values[src] >> 8;
                s8 = (uint8_t)(packed_values[src] & 0xFFu);
                depth = (float)d24 / 16777215.0f;
            } else {
                depth = depth_values[src];
                d24 = gxm_depth_to_u24(depth);
            }

            if (layout.packed) {
                ((uint32_t *)depth_data)[dst] = (d24 << 8) | s8;
            } else {
                if (depth_data && have_depth) {
                    if (layout.depth_bits == 32)
                        ((float *)depth_data)[dst] = depth;
                    else if (layout.depth_bits == 16)
                        ((uint16_t *)depth_data)[dst] =
                            (uint16_t)(depth * 65535.0f + 0.5f);
                }
                if (stencil_data && have_stencil && layout.has_stencil)
                    stencil_data[dst] = s8;
            }
        }
    }

    free(packed_values);
    free(depth_values);
}

EXPORT(SceGxm, 0x8734FF4E, int, sceGxmBeginScene, SceGxmContext *context, unsigned int flags,
       const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
       SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
       const SceGxmColorSurface *colorSurface, const SceGxmDepthStencilSurface *depthStencil)
{
    (void)flags;
    (void)validRegion;
    (void)vertexSyncObject;

    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (context->in_scene)
        return SCE_GXM_ERROR_WITHIN_SCENE;
    if (!renderTarget)
        return SCE_GXM_ERROR_INVALID_POINTER;

    context->render_target = renderTarget;
    context->color_surface =
        (colorSurface && !((const SceGxmColorSurfaceInner *)colorSurface)->disabled)
            ? (const SceGxmColorSurfaceInner *)colorSurface
            : NULL;
    context->ds_surface = (depthStencil && !gxm_ds_surface_is_disabled(depthStencil)) ? depthStencil
                                                                                        : NULL;
    context->ds_store_surface = context->ds_surface;
    context->fragment_sync = fragmentSyncObject;
    context->vertex_uniform_head = 0;
    context->fragment_uniform_head = 0;
    context->vertex_uniforms = NULL;
    context->fragment_uniforms = NULL;
    context->precomputed_vertex = NULL;
    context->precomputed_fragment = NULL;
    context->in_scene = true;

    if (g_gl) {
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, renderTarget->fbo);
        g_gl->glViewport(0, 0, renderTarget->params.width, renderTarget->params.height);
        g_gl->glDisable(GL_SCISSOR_TEST);

        if (!gxm_ds_load_surface((SceGxmRenderTarget *)renderTarget, context->ds_surface)) {
            float bg_depth = 1.0f;
            unsigned int bg_stencil = 0;

            if (context->ds_surface) {
                bg_depth = context->ds_surface->backgroundDepth;
                bg_stencil = context->ds_surface->backgroundControl &
                             SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK;
            }

            /* glClear honours the write masks. */
            g_gl->glDepthMask(GL_TRUE);
            g_gl->glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
            g_gl->glClearDepthf(bg_depth);
            g_gl->glClearStencil((GLint)bg_stencil);
            g_gl->glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
    }

    return 0;
}

static uint32_t gxm_color_format_bytes(SceGxmColorFormat format)
{
    uint32_t base = format & SCE_GXM_COLOR_BASE_FORMAT_MASK;

    switch (base) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
        return 4;
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
        return 3;
    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5:
    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5:
    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4:
    case SCE_GXM_COLOR_BASE_FORMAT_U8U3U3U2:
        return 2;
    case SCE_GXM_COLOR_BASE_FORMAT_U8:
    case SCE_GXM_COLOR_BASE_FORMAT_S8:
        return 1;
    default:
        return 4;
    }
}

static void gxm_write_color_pixel(uint8_t *dst, const uint8_t rgba[4], SceGxmColorFormat format)
{
    uint32_t base = format & SCE_GXM_COLOR_BASE_FORMAT_MASK;
    uint32_t swizzle = (format >> 20) & 3;
    uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];

    switch (base) {
    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8:
        switch (swizzle) {
        case 1: /* ARGB: word A R G B -> bytes B G R A */
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = a;
            break;
        case 2: /* RGBA: word R G B A -> bytes A B G R */
            dst[0] = a;
            dst[1] = b;
            dst[2] = g;
            dst[3] = r;
            break;
        case 3: /* BGRA: word B G R A -> bytes A R G B */
            dst[0] = a;
            dst[1] = r;
            dst[2] = g;
            dst[3] = b;
            break;
        case 0: /* ABGR (A8B8G8R8): bytes R G B A */
        default:
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = a;
            break;
        }
        break;

    case SCE_GXM_COLOR_BASE_FORMAT_U8U8U8:
        if (swizzle & 1) { /* RGB */
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
        } else { /* BGR */
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
        }
        break;

    case SCE_GXM_COLOR_BASE_FORMAT_U5U6U5: {
        uint16_t value = swizzle & 1 ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                                     : (uint16_t)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5: {
        uint16_t value = (uint16_t)(((a >> 7) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) |
                                    (r >> 3));
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4: {
        uint16_t value = (uint16_t)(((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) |
                                    (r >> 4));
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U8U3U3U2: {
        uint16_t value = (uint16_t)(((uint16_t)a << 8) | (((r >> 5) & 7) << 5) |
                                    (((g >> 5) & 7) << 2) | ((b >> 6) & 3));
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U8:
    default:
        dst[0] = r;
        break;
    }
}

EXPORT(SceGxm, 0xFE300E2F, int, sceGxmEndScene, SceGxmContext *context,
       const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification)
{
    const SceGxmColorSurfaceInner *surface;
    uint32_t width, height, stride;
    uint8_t *rgba;

    (void)vertexNotification;
    (void)fragmentNotification;

    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;

    context->in_scene = false;

    if (!g_gl || !context->render_target)
        return 0;

    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, context->render_target->fbo);
    g_gl->glFinish();

    surface = context->color_surface;
    if (surface && surface->data && !surface->disabled) {
        width = surface->width < context->render_target->params.width
                    ? surface->width
                    : context->render_target->params.width;
        height = surface->height < context->render_target->params.height
                     ? surface->height
                     : context->render_target->params.height;
        stride = surface->strideInPixels ? surface->strideInPixels : width;

        rgba = malloc((size_t)width * height * 4);
        if (!rgba)
            return SCE_GXM_ERROR_OUT_OF_MEMORY;

        g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        g_gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

        /* GL's origin is bottom-left; Vita surfaces are top-left. */
        {
            uint32_t bytes_per_pixel = gxm_color_format_bytes(surface->colorFormat);

            for (uint32_t y = 0; y < height; y++) {
                const uint8_t *src = rgba + (size_t)(height - 1 - y) * width * 4;
                uint8_t *dst = (uint8_t *)surface->data + (size_t)y * stride * bytes_per_pixel;

                for (uint32_t x = 0; x < width; x++)
                    gxm_write_color_pixel(dst + (size_t)x * bytes_per_pixel, src + (size_t)x * 4,
                                          surface->colorFormat);
            }
        }

        free(rgba);
    }

    if (context->ds_store_surface)
        gxm_ds_store_surface((SceGxmRenderTarget *)context->render_target,
                             context->ds_store_surface);

    if (fragmentNotification && fragmentNotification->address) {
        __sync_synchronize();
        *fragmentNotification->address = fragmentNotification->value;
        __sync_synchronize();
    }
    if (vertexNotification && vertexNotification->address) {
        __sync_synchronize();
        *vertexNotification->address = vertexNotification->value;
        __sync_synchronize();
    }

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
    if (streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
        return SCE_GXM_ERROR_INVALID_VALUE;

    context->streams[streamIndex].data = streamData;
    context->streams[streamIndex].size = 0;
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

    if (context->deferred) {
        if (!context->command_list_active)
            return SCE_GXM_ERROR_NOT_WITHIN_COMMAND_LIST;
    } else if (!context->in_scene) {
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    }
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

    if (context->deferred) {
        if (!context->command_list_active)
            return SCE_GXM_ERROR_NOT_WITHIN_COMMAND_LIST;
    } else if (!context->in_scene) {
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    }
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

static uint32_t gxm_floor_log2(uint32_t value)
{
    uint32_t bit = 0;

    while (value >>= 1)
        bit++;
    return bit;
}

static int gxm_texture_init(SceGxmTexture *texture, const void *data, SceGxmTextureFormat format,
                            unsigned int width, unsigned int height, unsigned int mip_count,
                            SceGxmTextureType type)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;
    bool swizzled = type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE;

    if (!texture)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!width || !height || width > 4096 || height > 4096 || mip_count > 13)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (swizzled && ((width & (width - 1)) != 0 || (height & (height - 1)) != 0))
        return SCE_GXM_ERROR_INVALID_VALUE;

    memset(inner, 0, sizeof(*inner));
    inner->mip_count = mip_count ? (mip_count - 1 > 15 ? 15 : mip_count - 1) : 15;
    inner->format0 = (format & 0x80000000u) >> 31;
    inner->lod_bias = 31;
    inner->uaddr_mode = inner->vaddr_mode = SCE_GXM_TEXTURE_ADDR_CLAMP;

    if (swizzled) {
        inner->width_base2 = gxm_floor_log2(width);
        inner->height_base2 = gxm_floor_log2(height);
    } else {
        inner->width = width - 1;
        inner->height = height - 1;
    }

    inner->base_format = (format & 0x1f000000u) >> 24;
    inner->type = (uint32_t)type >> 29;
    inner->data_addr = (uint32_t)(uintptr_t)data >> 2;
    inner->swizzle_format = (format & 0x7000u) >> 12;
    inner->normalize_mode = 1;
    inner->min_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    inner->mag_filter = SCE_GXM_TEXTURE_FILTER_POINT;
    inner->mip_filter = 0;

    return 0;
}

EXPORT(SceGxm, 0x4811AECB, int, sceGxmTextureInitLinear, SceGxmTexture *texture, const void *data,
       SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount,
                            SCE_GXM_TEXTURE_LINEAR);
}

EXPORT(SceGxm, 0xD572D547, int, sceGxmTextureInitSwizzled, SceGxmTexture *texture, const void *data,
       SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount,
                            SCE_GXM_TEXTURE_SWIZZLED);
}

EXPORT(SceGxm, 0x5DBFBA2C, int, sceGxmTextureInitSwizzledArbitrary, SceGxmTexture *texture,
       const void *data, SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount,
                            SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY);
}

EXPORT(SceGxm, 0x11DC8DC9, int, sceGxmTextureInitCube, SceGxmTexture *texture, const void *data,
       SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount, SCE_GXM_TEXTURE_CUBE);
}

EXPORT(SceGxm, 0xE3DF5E3B, int, sceGxmTextureInitCubeArbitrary, SceGxmTexture *texture,
       const void *data, SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount,
                            SCE_GXM_TEXTURE_CUBE_ARBITRARY);
}

EXPORT(SceGxm, 0xE6F0DB27, int, sceGxmTextureInitTiled, SceGxmTexture *texture, const void *data,
       SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int mipCount)
{
    return gxm_texture_init(texture, data, texFormat, width, height, mipCount,
                            SCE_GXM_TEXTURE_TILED);
}

static void gxm_texture_set_stride_bits(SceGxmTextureInner *inner, unsigned int byteStride)
{
    uint32_t words = byteStride / 4;
    uint32_t value = words ? words - 1 : 0;

    inner->mip_filter = value & 1;
    inner->min_filter = (value >> 1) & 3;
    inner->mip_count = (value >> 3) & 0xf;
    inner->lod_bias = (value >> 7) & 0x3f;
}

EXPORT(SceGxm, 0x6679BEF0, int, sceGxmTextureInitLinearStrided, SceGxmTexture *texture,
       const void *data, SceGxmTextureFormat texFormat, unsigned int width, unsigned int height,
       unsigned int byteStride)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;
    uint32_t bits = gxm_texture_base_bits(texFormat);
    int result;

    if (!byteStride || (byteStride & 3) || byteStride / 4 > 0x2000)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (bits && byteStride * 8 < width * bits)
        return SCE_GXM_ERROR_INVALID_VALUE;

    result = gxm_texture_init(texture, data, texFormat, width, height, 1,
                              SCE_GXM_TEXTURE_LINEAR_STRIDED);
    if (result != 0)
        return result;

    gxm_texture_set_stride_bits(inner, byteStride);
    return 0;
}

EXPORT(SceGxm, 0x855814C4, int, sceGxmTextureSetData, SceGxmTexture *texture, const void *data)
{
    ((SceGxmTextureInner *)texture)->data_addr = (uint32_t)(uintptr_t)data >> 2;
    return 0;
}

EXPORT(SceGxm, 0xFC943596, int, sceGxmTextureSetFormat, SceGxmTexture *texture,
       SceGxmTextureFormat texFormat)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    inner->format0 = (texFormat & 0x80000000u) >> 31;
    inner->base_format = (texFormat & 0x1f000000u) >> 24;
    inner->swizzle_format = (texFormat & 0x7000u) >> 12;
    return 0;
}

EXPORT(SceGxm, 0x416764E3, int, sceGxmTextureSetMinFilter, SceGxmTexture *texture,
       SceGxmTextureFilter minFilter)
{
    ((SceGxmTextureInner *)texture)->min_filter = minFilter;
    return 0;
}

EXPORT(SceGxm, 0xFA695FD7, int, sceGxmTextureSetMagFilter, SceGxmTexture *texture,
       SceGxmTextureFilter magFilter)
{
    ((SceGxmTextureInner *)texture)->mag_filter = magFilter;
    return 0;
}

EXPORT(SceGxm, 0x1CA9FE0B, int, sceGxmTextureSetMipFilter, SceGxmTexture *texture,
       SceGxmTextureMipFilter mipFilter)
{
    ((SceGxmTextureInner *)texture)->mip_filter = mipFilter ? 1 : 0;
    return 0;
}

EXPORT(SceGxm, 0x4281763E, int, sceGxmTextureSetUAddrMode, SceGxmTexture *texture,
       SceGxmTextureAddrMode addrMode)
{
    ((SceGxmTextureInner *)texture)->uaddr_mode = addrMode;
    return 0;
}

EXPORT(SceGxm, 0x126CDAA3, int, sceGxmTextureSetVAddrMode, SceGxmTexture *texture,
       SceGxmTextureAddrMode addrMode)
{
    ((SceGxmTextureInner *)texture)->vaddr_mode = addrMode;
    return 0;
}

EXPORT(SceGxm, 0x8699ECF4, int, sceGxmTextureSetUAddrModeSafe, SceGxmTexture *texture,
       SceGxmTextureAddrMode addrMode)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    inner->uaddr_mode = gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_LINEAR_STRIDED
                            ? SCE_GXM_TEXTURE_ADDR_CLAMP
                            : addrMode;
    return 0;
}

EXPORT(SceGxm, 0xFA22F6CC, int, sceGxmTextureSetVAddrModeSafe, SceGxmTexture *texture,
       SceGxmTextureAddrMode addrMode)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    inner->vaddr_mode = gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_LINEAR_STRIDED
                            ? SCE_GXM_TEXTURE_ADDR_CLAMP
                            : addrMode;
    return 0;
}

EXPORT(SceGxm, 0xB65EE6F7, int, sceGxmTextureSetLodBias, SceGxmTexture *texture, unsigned int bias)
{
    ((SceGxmTextureInner *)texture)->lod_bias = bias & 0x3f;
    return 0;
}

EXPORT(SceGxm, 0xB79E43DD, int, sceGxmTextureSetLodMin, SceGxmTexture *texture, unsigned int min)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    inner->lod_min0 = min & 3;
    inner->lod_min1 = (min >> 2) & 3;
    return 0;
}

EXPORT(SceGxm, 0x58D0EB0A, int, sceGxmTextureSetStride, SceGxmTexture *texture,
       unsigned int byteStride)
{
    if (!byteStride || (byteStride & 3) || byteStride / 4 > 0x2000)
        return SCE_GXM_ERROR_INVALID_VALUE;
    gxm_texture_set_stride_bits((SceGxmTextureInner *)texture, byteStride);
    return 0;
}

EXPORT(SceGxm, 0x2EA178BE, int, sceGxmTextureSetWidth, SceGxmTexture *texture, unsigned int width)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    if (!width || width > 4096)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_SWIZZLED ||
        gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_CUBE)
        inner->width_base2 = gxm_floor_log2(width);
    else
        inner->width = width - 1;
    return 0;
}

EXPORT(SceGxm, 0xAEE7FDD1, int, sceGxmTextureSetHeight, SceGxmTexture *texture, unsigned int height)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    if (!height || height > 4096)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_SWIZZLED ||
        gxm_texture_get_type(inner) == SCE_GXM_TEXTURE_CUBE)
        inner->height_base2 = gxm_floor_log2(height);
    else
        inner->height = height - 1;
    return 0;
}

EXPORT(SceGxm, 0xD2DC4643, int, sceGxmTextureSetMipmapCount, SceGxmTexture *texture,
       unsigned int mipCount)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    if (mipCount > 13)
        return SCE_GXM_ERROR_INVALID_VALUE;
    inner->mip_count = mipCount ? (mipCount - 1 > 15 ? 15 : mipCount - 1) : 15;
    return 0;
}

EXPORT(SceGxm, 0xDD6AABFA, int, sceGxmTextureSetPalette, SceGxmTexture *texture,
       const void *paletteData)
{
    if ((uintptr_t)paletteData & (SCE_GXM_PALETTE_ALIGNMENT - 1))
        return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    ((SceGxmTextureInner *)texture)->palette_addr = (uint32_t)(uintptr_t)paletteData >> 6;
    return 0;
}

EXPORT(SceGxm, 0xA6D9F4DA, int, sceGxmTextureSetGammaMode, SceGxmTexture *texture,
       SceGxmTextureGammaMode gammaMode)
{
    SceGxmTextureInner *inner = (SceGxmTextureInner *)texture;

    if (gammaMode & ~0x60000000u)
        return SCE_GXM_ERROR_INVALID_VALUE;
    inner->gamma_mode = (gammaMode >> 27) & 3;
    return 0;
}

EXPORT(SceGxm, 0xCE8DDAD0, int, sceGxmTextureSetNormalizeMode, SceGxmTexture *texture,
       SceBool normalize)
{
    ((SceGxmTextureInner *)texture)->normalize_mode = normalize ? 1 : 0;
    return 0;
}

EXPORT(SceGxm, 0x5331BED3, int, sceGxmTextureValidate, const SceGxmTexture *texture)
{
    SceGxmTextureFormat format;
    uint32_t width, height;

    if (!texture)
        return SCE_GXM_ERROR_INVALID_POINTER;
    format = (SceGxmTextureFormat)gxm_texture_get_format((const SceGxmTextureInner *)texture);
    width = (uint32_t)gxm_texture_get_width((const SceGxmTextureInner *)texture);
    height = (uint32_t)gxm_texture_get_height((const SceGxmTextureInner *)texture);

    if (!gxm_texture_base_bits(format) || !width || !height || width > 4096 || height > 4096)
        return SCE_GXM_ERROR_INVALID_VALUE;
    return 0;
}

#define GXM_TEX_GETTER(nid, name, expr)                                                            \
    EXPORT(SceGxm, nid, uint32_t, sceGxmTexture##name, const SceGxmTexture *texture)               \
    {                                                                                              \
        return (uint32_t)(expr);                                                                   \
    }

GXM_TEX_GETTER(0x5420A086, GetHeight, gxm_texture_get_height((const SceGxmTextureInner *)texture))
GXM_TEX_GETTER(0x126A3EB3, GetWidth, gxm_texture_get_width((const SceGxmTextureInner *)texture))
GXM_TEX_GETTER(0xB0BD52F3, GetStride,
               gxm_texture_get_type((const SceGxmTextureInner *)texture) ==
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

#define GXM_PARAM_GETTER(nid, name, type, expr)                                                    \
    EXPORT(SceGxm, nid, type, sceGxmProgramParameter##name,                                        \
           const SceGxmProgramParameter *parameter)                                                \
    {                                                                                              \
        return (type)(expr);                                                                       \
    }

GXM_PARAM_GETTER(0xDBA8D061, GetArraySize, uint32_t, parameter->array_size)
GXM_PARAM_GETTER(0x1997DC17, GetCategory, SceGxmParameterCategory, parameter->category)
GXM_PARAM_GETTER(0xBD2998D1, GetComponentCount, uint32_t, parameter->component_count)
GXM_PARAM_GETTER(0xBB58267D, GetContainerIndex, uint32_t, parameter->container_index)
GXM_PARAM_GETTER(0x5C79D59A, GetResourceIndex, uint32_t, parameter->resource_index)
GXM_PARAM_GETTER(0x7B9023C3, GetType, SceGxmParameterType, parameter->type)
GXM_PARAM_GETTER(0xE6D9C4CE, GetSemantic, SceGxmParameterSemantic,
                 parameter->category == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE
                     ? parameter->semantic
                     : SCE_GXM_PARAMETER_SEMANTIC_NONE)
GXM_PARAM_GETTER(0xB85CC13E, GetSemanticIndex, uint32_t, parameter->semantic_index & 0xf)

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

static uint32_t gxm_read_index(const void *data, SceGxmIndexFormat format, unsigned int index)
{
    if (format == SCE_GXM_INDEX_FORMAT_U32)
        return ((const uint32_t *)data)[index];
    return ((const uint16_t *)data)[index];
}

static bool gxm_polygon_is_line(SceGxmPolygonMode mode)
{
    return mode == SCE_GXM_POLYGON_MODE_LINE || mode == SCE_GXM_POLYGON_MODE_TRIANGLE_LINE;
}

static bool gxm_polygon_is_point(SceGxmPolygonMode mode)
{
    return mode == SCE_GXM_POLYGON_MODE_POINT || mode == SCE_GXM_POLYGON_MODE_POINT_10UV ||
           mode == SCE_GXM_POLYGON_MODE_POINT_01UV ||
           mode == SCE_GXM_POLYGON_MODE_TRIANGLE_POINT;
}

static uint32_t *gxm_wireframe_indices(SceGxmPrimitiveType primitive, SceGxmIndexFormat format,
                                       const void *indexData, unsigned int indexCount,
                                       uint32_t *out_count)
{
    uint32_t *indices;
    uint32_t count = 0;

    if (primitive != SCE_GXM_PRIMITIVE_TRIANGLES &&
        primitive != SCE_GXM_PRIMITIVE_TRIANGLE_STRIP &&
        primitive != SCE_GXM_PRIMITIVE_TRIANGLE_FAN)
        return NULL;

    indices = malloc((size_t)indexCount * 3 * sizeof(uint32_t));
    if (!indices)
        return NULL;

    if (primitive == SCE_GXM_PRIMITIVE_TRIANGLES) {
        for (unsigned int i = 0; i + 2 < indexCount; i += 3) {
            uint32_t a = gxm_read_index(indexData, format, i);
            uint32_t b = gxm_read_index(indexData, format, i + 1);
            uint32_t c = gxm_read_index(indexData, format, i + 2);

            indices[count++] = a;
            indices[count++] = b;
            indices[count++] = b;
            indices[count++] = c;
            indices[count++] = c;
            indices[count++] = a;
        }
    } else if (primitive == SCE_GXM_PRIMITIVE_TRIANGLE_STRIP) {
        for (unsigned int i = 0; i + 2 < indexCount; i++) {
            uint32_t a = gxm_read_index(indexData, format, i);
            uint32_t b = gxm_read_index(indexData, format, i + 1);
            uint32_t c = gxm_read_index(indexData, format, i + 2);

            indices[count++] = a;
            indices[count++] = b;
            indices[count++] = b;
            indices[count++] = c;
            indices[count++] = c;
            indices[count++] = a;
        }
    } else {
        uint32_t first = gxm_read_index(indexData, format, 0);

        for (unsigned int i = 1; i + 1 < indexCount; i++) {
            uint32_t a = gxm_read_index(indexData, format, i);
            uint32_t b = gxm_read_index(indexData, format, i + 1);

            indices[count++] = first;
            indices[count++] = a;
            indices[count++] = a;
            indices[count++] = b;
            indices[count++] = b;
            indices[count++] = first;
        }
    }

    *out_count = count;
    return indices;
}

static uint32_t gxm_default_uniform_bytes(const SceGxmProgram *program, const void *buffer)
{
    if (!program || !buffer)
        return 0;
    return program->default_uniform_buffer_count * sizeof(float);
}

static void gxm_command_free(GxmCommand *command)
{
    while (command) {
        GxmCommand *next = command->next;

        free(command->draw.vertex_uniforms);
        free(command->draw.fragment_uniforms);
        free(command);
        command = next;
    }
}

static GxmCommandListEntry *gxm_command_list_entry(SceGxmCommandList *list, bool create)
{
    GxmCommandListEntry *free_slot = NULL;
    uint32_t free_index = 0;

    for (unsigned int i = 0; i < GXM_MAX_COMMAND_LISTS; i++) {
        if (g_command_lists[i].guest == list)
            return &g_command_lists[i];
        if (!g_command_lists[i].guest && !free_slot) {
            free_slot = &g_command_lists[i];
            free_index = i;
        }
    }

    if (!create || !free_slot)
        return NULL;

    free_slot->guest = list;
    free_slot->head = NULL;
    free_slot->tail = NULL;
    free_slot->index = free_index;
    return free_slot;
}

static GxmCommandListEntry *gxm_command_list_lookup(const SceGxmCommandList *list)
{
    uint32_t index;

    if (!list || list->words[0] != GXM_COMMAND_LIST_MAGIC)
        return NULL;

    index = list->words[1];
    if (index >= GXM_MAX_COMMAND_LISTS || !g_command_lists[index].guest)
        return NULL;

    return &g_command_lists[index];
}

/*
 * Takes a snapshot of everything a draw reads from the deferred context. The
 * command list is replayed later on an immediate context inside a scene.
 */
static int gxm_record_draw(SceGxmContext *context, SceGxmPrimitiveType primType,
                           SceGxmIndexFormat indexType, const void *indexData,
                           unsigned int indexCount, unsigned int instances)
{
    GxmCommand *command = calloc(1, sizeof(*command));
    GxmDrawCommand *draw;
    uint32_t vertex_size, fragment_size;

    if (!command)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;

    command->type = GXM_COMMAND_DRAW;
    draw = &command->draw;
    draw->vertex_program = context->vertex_program;
    draw->fragment_program = context->fragment_program;
    for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++)
        draw->streams[i] = context->streams[i].data;
    memcpy(draw->fragment_textures, context->fragment_textures, sizeof(draw->fragment_textures));
    memcpy(draw->vertex_textures, context->vertex_textures, sizeof(draw->vertex_textures));
    memcpy(draw->vertex_uniform_buffers, context->vertex_uniform_buffers,
           sizeof(draw->vertex_uniform_buffers));
    memcpy(draw->fragment_uniform_buffers, context->fragment_uniform_buffers,
           sizeof(draw->fragment_uniform_buffers));

    draw->viewport_x_offset = context->viewport_x_offset;
    draw->viewport_x_scale = context->viewport_x_scale;
    draw->viewport_y_offset = context->viewport_y_offset;
    draw->viewport_y_scale = context->viewport_y_scale;
    draw->viewport_z_offset = context->viewport_z_offset;
    draw->viewport_z_scale = context->viewport_z_scale;
    draw->viewport_enable = context->viewport_enable;
    draw->region_clip_mode = context->region_clip_mode;
    draw->region_clip_xmin = context->region_clip_xmin;
    draw->region_clip_ymin = context->region_clip_ymin;
    draw->region_clip_xmax = context->region_clip_xmax;
    draw->region_clip_ymax = context->region_clip_ymax;
    draw->cull_mode = context->cull_mode;
    draw->front_polygon_mode = context->front_polygon_mode;
    draw->back_polygon_mode = context->back_polygon_mode;
    draw->front_point_line_width = context->front_point_line_width;
    draw->front_depth_bias_factor = context->front_depth_bias_factor;
    draw->front_depth_bias_units = context->front_depth_bias_units;
    draw->two_sided = context->two_sided;
    draw->front_fragment_program_enable = context->front_fragment_program_enable;
    draw->back_fragment_program_enable = context->back_fragment_program_enable;
    draw->depth_func = context->depth_func;
    draw->depth_write = context->depth_write;
    draw->front_stencil_func = context->front_stencil_func;
    draw->front_stencil_fail = context->front_stencil_fail;
    draw->front_stencil_depth_fail = context->front_stencil_depth_fail;
    draw->front_stencil_pass = context->front_stencil_pass;
    draw->front_stencil_compare_mask = context->front_stencil_compare_mask;
    draw->front_stencil_write_mask = context->front_stencil_write_mask;
    draw->back_stencil_func = context->back_stencil_func;
    draw->back_stencil_fail = context->back_stencil_fail;
    draw->back_stencil_depth_fail = context->back_stencil_depth_fail;
    draw->back_stencil_pass = context->back_stencil_pass;
    draw->back_stencil_compare_mask = context->back_stencil_compare_mask;
    draw->back_stencil_write_mask = context->back_stencil_write_mask;
    draw->front_stencil_ref = context->front_stencil_ref;
    draw->back_stencil_ref = context->back_stencil_ref;
    draw->prim_type = primType;
    draw->index_type = indexType;
    draw->index_data = indexData;
    draw->index_count = indexCount;
    draw->instances = instances;

    vertex_size =
        gxm_default_uniform_bytes(context->vertex_program->programId->programHeader,
                                  context->vertex_uniforms);
    fragment_size =
        gxm_default_uniform_bytes(context->fragment_program->programId->programHeader,
                                  context->fragment_uniforms);

    if (vertex_size) {
        draw->vertex_uniforms = malloc(vertex_size);
        if (!draw->vertex_uniforms)
            goto oom;
        memcpy(draw->vertex_uniforms, context->vertex_uniforms, vertex_size);
        draw->vertex_uniform_size = vertex_size;
    }
    if (fragment_size) {
        draw->fragment_uniforms = malloc(fragment_size);
        if (!draw->fragment_uniforms)
            goto oom;
        memcpy(draw->fragment_uniforms, context->fragment_uniforms, fragment_size);
        draw->fragment_uniform_size = fragment_size;
    }

    if (context->cmd_tail)
        context->cmd_tail->next = command;
    else
        context->cmd_head = command;
    context->cmd_tail = command;

    return 0;

oom:
    gxm_command_free(command);
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
}

static void gxm_apply_draw_command(SceGxmContext *context, const GxmDrawCommand *draw)
{
    context->vertex_program = draw->vertex_program;
    context->fragment_program = draw->fragment_program;
    for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        context->streams[i].data = draw->streams[i];
        context->streams[i].size = 0;
    }
    memcpy(context->fragment_textures, draw->fragment_textures, sizeof(context->fragment_textures));
    memcpy(context->vertex_textures, draw->vertex_textures, sizeof(context->vertex_textures));
    memcpy(context->vertex_uniform_buffers, draw->vertex_uniform_buffers,
           sizeof(context->vertex_uniform_buffers));
    memcpy(context->fragment_uniform_buffers, draw->fragment_uniform_buffers,
           sizeof(context->fragment_uniform_buffers));
    context->vertex_uniforms = draw->vertex_uniforms;
    context->vertex_uniform_size = draw->vertex_uniform_size;
    context->fragment_uniforms = draw->fragment_uniforms;
    context->fragment_uniform_size = draw->fragment_uniform_size;

    context->viewport_x_offset = draw->viewport_x_offset;
    context->viewport_x_scale = draw->viewport_x_scale;
    context->viewport_y_offset = draw->viewport_y_offset;
    context->viewport_y_scale = draw->viewport_y_scale;
    context->viewport_z_offset = draw->viewport_z_offset;
    context->viewport_z_scale = draw->viewport_z_scale;
    context->viewport_enable = draw->viewport_enable;
    context->region_clip_mode = draw->region_clip_mode;
    context->region_clip_xmin = draw->region_clip_xmin;
    context->region_clip_ymin = draw->region_clip_ymin;
    context->region_clip_xmax = draw->region_clip_xmax;
    context->region_clip_ymax = draw->region_clip_ymax;
    context->cull_mode = draw->cull_mode;
    context->front_polygon_mode = draw->front_polygon_mode;
    context->back_polygon_mode = draw->back_polygon_mode;
    context->front_point_line_width = draw->front_point_line_width;
    context->front_depth_bias_factor = draw->front_depth_bias_factor;
    context->front_depth_bias_units = draw->front_depth_bias_units;
    context->two_sided = draw->two_sided;
    context->front_fragment_program_enable = draw->front_fragment_program_enable;
    context->back_fragment_program_enable = draw->back_fragment_program_enable;
    context->depth_func = draw->depth_func;
    context->depth_write = draw->depth_write;
    context->front_stencil_func = draw->front_stencil_func;
    context->front_stencil_fail = draw->front_stencil_fail;
    context->front_stencil_depth_fail = draw->front_stencil_depth_fail;
    context->front_stencil_pass = draw->front_stencil_pass;
    context->front_stencil_compare_mask = draw->front_stencil_compare_mask;
    context->front_stencil_write_mask = draw->front_stencil_write_mask;
    context->back_stencil_func = draw->back_stencil_func;
    context->back_stencil_fail = draw->back_stencil_fail;
    context->back_stencil_depth_fail = draw->back_stencil_depth_fail;
    context->back_stencil_pass = draw->back_stencil_pass;
    context->back_stencil_compare_mask = draw->back_stencil_compare_mask;
    context->back_stencil_write_mask = draw->back_stencil_write_mask;
    context->front_stencil_ref = draw->front_stencil_ref;
    context->back_stencil_ref = draw->back_stencil_ref;
}

static int gxm_draw_common(SceGxmContext *context, SceGxmPrimitiveType primType,
                           SceGxmIndexFormat indexType, const void *indexData,
                           unsigned int indexCount, unsigned int instances);

static int gxm_replay_commands(SceGxmContext *context, GxmCommand *command)
{
    for (; command; command = command->next) {
        int result;

        if (command->type != GXM_COMMAND_DRAW)
            continue;

        gxm_apply_draw_command(context, &command->draw);
        result = gxm_draw_common(context, command->draw.prim_type, command->draw.index_type,
                                 command->draw.index_data, command->draw.index_count,
                                 command->draw.instances);
        if (result != 0)
            return result;
    }

    return 0;
}

static int gxm_draw_common(SceGxmContext *context, SceGxmPrimitiveType primType,
                           SceGxmIndexFormat indexType, const void *indexData,
                           unsigned int indexCount, unsigned int instances)
{
    GLuint program;
    GLenum gl_index_type;
    GLenum gl_primitive;
    size_t index_size;
    uint32_t max_index = 0;
    SceGxmPolygonMode polygon_mode;

    if (context->deferred) {
        if (!context->command_list_active)
            return SCE_GXM_ERROR_NOT_WITHIN_COMMAND_LIST;
        if (!context->vertex_program || !context->fragment_program)
            return SCE_GXM_ERROR_NULL_PROGRAM;
        if (!indexData || !indexCount)
            return SCE_GXM_ERROR_INVALID_VALUE;
        if (!g_gl)
            return 0;
        return gxm_record_draw(context, primType, indexType, indexData, indexCount, instances);
    }

    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!context->vertex_program || !context->fragment_program)
        return SCE_GXM_ERROR_NULL_PROGRAM;
    if (!indexData || !indexCount)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (!g_gl)
        return 0;

    program = gxm_get_linked_program(context->vertex_program, context->fragment_program);
    if (!program)
        return SCE_GXM_ERROR_INVALID_VALUE;

    {
        for (unsigned int i = 0; i < indexCount; i++) {
            uint32_t index = gxm_read_index(indexData, indexType, i);
            if (index > max_index)
                max_index = index;
        }

        gxm_flush_state(context, program, max_index);
    }

    if (indexType == SCE_GXM_INDEX_FORMAT_U32) {
        gl_index_type = GL_UNSIGNED_INT;
        index_size = 4;
    } else {
        gl_index_type = GL_UNSIGNED_SHORT;
        index_size = 2;
    }

    if (!context->ibo)
        g_gl->glGenBuffers(1, &context->ibo);
    g_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, context->ibo);
    g_gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * index_size), indexData,
                       GL_STREAM_DRAW);

    gl_primitive = gxm_primitive_to_gl(primType);
    polygon_mode = context->front_polygon_mode;

    if (gxm_polygon_is_line(polygon_mode)) {
        uint32_t wire_count = 0;
        uint32_t *wire = gxm_wireframe_indices(primType, indexType, indexData, indexCount,
                                               &wire_count);

        if (wire) {
            if (!context->wire_ibo)
                g_gl->glGenBuffers(1, &context->wire_ibo);
            g_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, context->wire_ibo);
            g_gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(wire_count * 4), wire,
                               GL_STREAM_DRAW);
            free(wire);
            g_gl->glDrawElements(GL_LINES, (GLsizei)wire_count, GL_UNSIGNED_INT, 0);
            return 0;
        }
    }

    if (gxm_polygon_is_point(polygon_mode))
        gl_primitive = GL_POINTS;

    if (instances > 1 && gl_primitive != GL_POINTS)
        g_gl->glDrawElementsInstanced(gl_primitive, (GLsizei)indexCount, gl_index_type, 0,
                                      (GLsizei)instances);
    else
        g_gl->glDrawElements(gl_primitive, (GLsizei)indexCount, gl_index_type, 0);

    gxm_gl_check("draw");

    return 0;
}

EXPORT(SceGxm, 0xBC059AFC, int, sceGxmDraw, SceGxmContext *context, SceGxmPrimitiveType primType,
       SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount)
{
    return gxm_draw_common(context, primType, indexType, indexData, indexCount, 1);
}

EXPORT(SceGxm, 0x14C4E7D3, int, sceGxmDrawInstanced, SceGxmContext *context,
       SceGxmPrimitiveType primType, SceGxmIndexFormat indexType, const void *indexData,
       unsigned int indexCount, unsigned int indexWrap)
{
    if (!indexWrap)
        return SCE_GXM_ERROR_INVALID_VALUE;

    return gxm_draw_common(context, primType, indexType, indexData, indexWrap,
                           indexCount / indexWrap);
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

EXPORT(SceGxm, 0x80CCEDBB, int, sceGxmUnmapFragmentUsseMemory, void *base)
{
    (void)base;
    return 0;
}

EXPORT(SceGxm, 0xD5D5FCCD, unsigned int, sceGxmProgramGetParameterCount,
       const SceGxmProgram *program)
{
    return program ? program->parameter_count : 0;
}

static const SceGxmProgramParameter *gxm_program_parameter(const SceGxmProgram *program,
                                                           unsigned int index)
{
    const uint8_t *base;

    if (!program || index >= program->parameter_count)
        return NULL;

    base = (const uint8_t *)&program->parameters_offset + program->parameters_offset;
    return (const SceGxmProgramParameter *)(base + index * sizeof(SceGxmProgramParameter));
}

EXPORT(SceGxm, 0x06FF9151, const SceGxmProgramParameter *, sceGxmProgramGetParameter,
       const SceGxmProgram *program, unsigned int index)
{
    return gxm_program_parameter(program, index);
}

EXPORT(SceGxm, 0x633CAA54, const SceGxmProgramParameter *, sceGxmProgramFindParameterBySemantic,
       const SceGxmProgram *program, SceGxmParameterSemantic semantic, unsigned int index)
{
    if (!program)
        return NULL;

    for (unsigned int i = 0; i < program->parameter_count; i++) {
        const SceGxmProgramParameter *parameter = gxm_program_parameter(program, i);

        if (parameter->semantic == semantic && (parameter->semantic_index & 0xf) == index)
            return parameter;
    }

    return NULL;
}

EXPORT(SceGxm, 0xED8B6C69, int, sceGxmProgramCheck, const SceGxmProgram *program)
{
    if (!program)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (program->magic != 0x00505847u /* "GXP\0" */ || program->size == 0)
        return SCE_GXM_ERROR_INVALID_VALUE;
    return 0;
}

EXPORT(SceGxm, 0xBF5E2090, unsigned int, sceGxmProgramGetSize, const SceGxmProgram *program)
{
    return program ? program->size : 0;
}

EXPORT(SceGxm, 0x04BB3C59, SceGxmProgramType, sceGxmProgramGetType, const SceGxmProgram *program)
{
    return program && (program->program_flags & 1) ? SCE_GXM_FRAGMENT_PROGRAM
                                                   : SCE_GXM_VERTEX_PROGRAM;
}

EXPORT(SceGxm, 0x029B4F1C, SceBool, sceGxmProgramIsDiscardUsed, const SceGxmProgram *program)
{
    return program && (program->program_flags & (1u << 3)) ? 1 : 0;
}

EXPORT(SceGxm, 0x89613EF2, SceBool, sceGxmProgramIsDepthReplaceUsed, const SceGxmProgram *program)
{
    return program && (program->program_flags & (1u << 4)) ? 1 : 0;
}

EXPORT(SceGxm, 0xE63C53D8, SceBool, sceGxmProgramIsSpriteCoordUsed, const SceGxmProgram *program)
{
    return program && (program->program_flags & (1u << 5)) ? 1 : 0;
}

EXPORT(SceGxm, 0xA4297E57, SceBool, sceGxmProgramIsNativeColorUsed, const SceGxmProgram *program)
{
    return program && (program->program_flags & (1u << 6)) ? 1 : 0;
}

EXPORT(SceGxm, 0x104F23F4, SceBool, sceGxmProgramIsFragColorUsed, const SceGxmProgram *program)
{
    return program && (program->program_flags & (1u << 7)) ? 1 : 0;
}

EXPORT(SceGxm, 0xF7AA978B, SceBool, sceGxmProgramParameterIsSamplerCube,
       const SceGxmProgramParameter *parameter)
{
    return parameter && ((parameter->semantic_index >> 4) & 1) ? 1 : 0;
}

EXPORT(SceGxm, 0x871E5009, SceBool, sceGxmProgramParameterIsRegFormat,
       const SceGxmProgram *program, const SceGxmProgramParameter *parameter)
{
    const uint8_t *base;
    uint64_t untyped = 0;

    if (!program || !parameter || (program->program_flags & 1))
        return 0;
    if (parameter->category != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
        return 0;

    base = (const uint8_t *)&program->varyings_offset + program->varyings_offset;
    memcpy(&untyped, base + 8, sizeof(untyped));
    return (untyped & (1ull << parameter->resource_index)) ? 1 : 0;
}

EXPORT(SceGxm, 0xC6924709, unsigned int, sceGxmProgramGetFragmentProgramInputs,
       const SceGxmProgram *program)
{
    (void)program;
    return 0;
}

EXPORT(SceGxm, 0xFB01C7E5, unsigned int, sceGxmProgramGetVertexProgramOutputs,
       const SceGxmProgram *program)
{
    const uint8_t *base;
    uint32_t outputs = 0;

    if (!program || (program->program_flags & 1))
        return 0;
    base = (const uint8_t *)&program->varyings_offset + program->varyings_offset;
    memcpy(&outputs, base + 16, sizeof(outputs));
    return outputs;
}

EXPORT(SceGxm, 0xE0E3B3F8, const SceGxmProgram *, sceGxmFragmentProgramGetProgram,
       const SceGxmFragmentProgram *fragmentProgram)
{
    return fragmentProgram ? fragmentProgram->programId->programHeader : NULL;
}

EXPORT(SceGxm, 0xBC52320E, const SceGxmProgram *, sceGxmVertexProgramGetProgram,
       const SceGxmVertexProgram *vertexProgram)
{
    return vertexProgram ? vertexProgram->programId->programHeader : NULL;
}

/* ---------------------------------------------------- shader patcher extras */

EXPORT(SceGxm, 0xF9B8FCFD, int, sceGxmShaderPatcherSetUserData, SceGxmShaderPatcher *shaderPatcher,
       void *userData)
{
    shaderPatcher->params.userData = userData;
    return 0;
}

EXPORT(SceGxm, 0x96A7E6DD, void *, sceGxmShaderPatcherGetUserData, SceGxmShaderPatcher *shaderPatcher)
{
    return shaderPatcher->params.userData;
}

EXPORT(SceGxm, 0x9DBBC71C, unsigned int, sceGxmShaderPatcherGetHostMemAllocated,
       const SceGxmShaderPatcher *shaderPatcher)
{
    (void)shaderPatcher;
    return 0;
}

EXPORT(SceGxm, 0xC694D039, unsigned int, sceGxmShaderPatcherGetBufferMemAllocated,
       const SceGxmShaderPatcher *shaderPatcher)
{
    (void)shaderPatcher;
    return 0;
}

EXPORT(SceGxm, 0x7D2F83C1, unsigned int, sceGxmShaderPatcherGetVertexUsseMemAllocated,
       const SceGxmShaderPatcher *shaderPatcher)
{
    (void)shaderPatcher;
    return 0;
}

EXPORT(SceGxm, 0x3C9DDB4A, unsigned int, sceGxmShaderPatcherGetFragmentUsseMemAllocated,
       const SceGxmShaderPatcher *shaderPatcher)
{
    (void)shaderPatcher;
    return 0;
}

EXPORT(SceGxm, 0x630D4B2E, int, sceGxmShaderPatcherForceUnregisterProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmShaderPatcherId programId)
{
    return sceGxmShaderPatcherUnregisterProgram(shaderPatcher, programId);
}

EXPORT(SceGxm, 0x8E5FCC2A, int, sceGxmShaderPatcherSetAuxiliarySurface,
       SceGxmShaderPatcher *shaderPatcher, unsigned int auxSurfaceIndex,
       const SceGxmAuxiliarySurface *auxSurface)
{
    (void)shaderPatcher;
    (void)auxSurfaceIndex;
    (void)auxSurface;
    return 0;
}

EXPORT(SceGxm, 0x0FD1E589, int, sceGxmShaderPatcherAddRefVertexProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmVertexProgram *vertexProgram)
{
    (void)shaderPatcher;
    vertexProgram->refcount++;
    return 0;
}

EXPORT(SceGxm, 0x4CD2D19F, int, sceGxmShaderPatcherAddRefFragmentProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmFragmentProgram *fragmentProgram)
{
    (void)shaderPatcher;
    fragmentProgram->refcount++;
    return 0;
}

EXPORT(SceGxm, 0xA1A16FF6, int, sceGxmShaderPatcherGetVertexProgramRefCount,
       SceGxmShaderPatcher *shaderPatcher, SceGxmVertexProgram *vertexProgram,
       unsigned int *count)
{
    (void)shaderPatcher;
    if (!vertexProgram || !count)
        return SCE_GXM_ERROR_INVALID_POINTER;
    *count = (unsigned int)vertexProgram->refcount;
    return 0;
}

EXPORT(SceGxm, 0x2C5550F0, int, sceGxmShaderPatcherGetFragmentProgramRefCount,
       SceGxmShaderPatcher *shaderPatcher, SceGxmFragmentProgram *fragmentProgram,
       unsigned int *count)
{
    (void)shaderPatcher;
    if (!fragmentProgram || !count)
        return SCE_GXM_ERROR_INVALID_POINTER;
    *count = (unsigned int)fragmentProgram->refcount;
    return 0;
}

EXPORT(SceGxm, 0xA4433427, int, sceGxmShaderPatcherCreateMaskUpdateFragmentProgram,
       SceGxmShaderPatcher *shaderPatcher, SceGxmFragmentProgram **fragmentProgram)
{
    SceGxmFragmentProgram *fp = calloc(1, sizeof(*fp));

    (void)shaderPatcher;
    if (!fp)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    fp->refcount = 1;
    fp->mask_update = true;
    fp->blendInfo = (SceGxmBlendInfo){
        SCE_GXM_COLOR_MASK_NONE, SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
    };
    *fragmentProgram = fp;
    return 0;
}

/* ----------------------------------------------------------- transfer APIs */

static uint32_t gxm_transfer_bpp(SceGxmTransferFormat format)
{
    switch (format) {
    case SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR:
    case SCE_GXM_TRANSFER_FORMAT_U2U10U10U10_ABGR:
    case SCE_GXM_TRANSFER_FORMAT_RAW32:
        return 4;
    case SCE_GXM_TRANSFER_FORMAT_U8U8U8_BGR:
        return 3;
    case SCE_GXM_TRANSFER_FORMAT_U4U4U4U4_ABGR:
    case SCE_GXM_TRANSFER_FORMAT_U1U5U5U5_ABGR:
    case SCE_GXM_TRANSFER_FORMAT_U5U6U5_BGR:
    case SCE_GXM_TRANSFER_FORMAT_U8U8_GR:
    case SCE_GXM_TRANSFER_FORMAT_VYUY422:
    case SCE_GXM_TRANSFER_FORMAT_YVYU422:
    case SCE_GXM_TRANSFER_FORMAT_UYVY422:
    case SCE_GXM_TRANSFER_FORMAT_YUYV422:
    case SCE_GXM_TRANSFER_FORMAT_RAW16:
        return 2;
    case SCE_GXM_TRANSFER_FORMAT_U8_R:
        return 1;
    case SCE_GXM_TRANSFER_FORMAT_RAW64:
        return 8;
    case SCE_GXM_TRANSFER_FORMAT_RAW128:
        return 16;
    default:
        return 4;
    }
}

EXPORT(SceGxm, 0x69DDFF5E, int, sceGxmTransferFill, uint32_t color,
       SceGxmTransferFormat destFormat, void *destAddress, uint32_t destX, uint32_t destY,
       uint32_t destWidth, uint32_t destHeight, int32_t destStride, SceGxmSyncObject *syncObject,
       uint32_t syncFlags, const SceGxmNotification *notification)
{
    uint32_t bpp = gxm_transfer_bpp(destFormat);

    (void)syncObject;
    (void)syncFlags;
    (void)notification;

    if (!destAddress || !destWidth || !destHeight || destStride <= 0)
        return SCE_GXM_ERROR_INVALID_POINTER;

    for (uint32_t y = 0; y < destHeight; y++) {
        uint8_t *row = (uint8_t *)destAddress + (size_t)(destY + y) * (size_t)destStride * bpp +
                       (size_t)destX * bpp;

        for (uint32_t x = 0; x < destWidth; x++) {
            switch (bpp) {
            case 1:
                row[x] = (uint8_t)color;
                break;
            case 2:
                ((uint16_t *)row)[x] = (uint16_t)color;
                break;
            case 3:
                row[x * 3 + 0] = (uint8_t)color;
                row[x * 3 + 1] = (uint8_t)(color >> 8);
                row[x * 3 + 2] = (uint8_t)(color >> 16);
                break;
            default:
                ((uint32_t *)row)[x] = color;
                break;
            }
        }
    }

    return 0;
}

EXPORT(SceGxm, 0x62312BF8, int, sceGxmTransferCopy, uint32_t width, uint32_t height,
       uint32_t colorKeyValue, uint32_t colorKeyMask, SceGxmTransferColorKeyMode colorKeyMode,
       SceGxmTransferFormat srcFormat, SceGxmTransferType srcType, const void *srcAddress,
       uint32_t srcX, uint32_t srcY, int32_t srcStride, SceGxmTransferFormat destFormat,
       SceGxmTransferType destType, void *destAddress, uint32_t destX, uint32_t destY,
       int32_t destStride, SceGxmSyncObject *syncObject, uint32_t syncFlags,
       const SceGxmNotification *notification)
{
    uint32_t src_bpp = gxm_transfer_bpp(srcFormat);
    uint32_t dst_bpp = gxm_transfer_bpp(destFormat);

    (void)colorKeyValue;
    (void)colorKeyMask;
    (void)colorKeyMode;
    (void)srcType;
    (void)destType;
    (void)syncObject;
    (void)syncFlags;
    (void)notification;

    if (!srcAddress || !destAddress || !width || !height)
        return SCE_GXM_ERROR_INVALID_POINTER;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *src = (const uint8_t *)srcAddress +
                             (size_t)(srcY + y) * (size_t)srcStride * src_bpp +
                             (size_t)srcX * src_bpp;
        uint8_t *dst = (uint8_t *)destAddress +
                       (size_t)(destY + y) * (size_t)destStride * dst_bpp +
                       (size_t)destX * dst_bpp;

        if (src_bpp == dst_bpp)
            memcpy(dst, src, (size_t)width * src_bpp);
        else
            return SCE_GXM_ERROR_INVALID_VALUE;
    }

    return 0;
}

EXPORT(SceGxm, 0xD10F7EAD, int, sceGxmTransferDownscale, SceGxmTransferFormat srcFormat,
       const void *srcAddress, unsigned int srcX, unsigned int srcY, unsigned int srcWidth,
       unsigned int srcHeight, int srcStride, SceGxmTransferFormat destFormat,
       void *destAddress, unsigned int destX, unsigned int destY, int destStride,
       SceGxmSyncObject *syncObject, unsigned int syncFlags,
       const SceGxmNotification *notification)
{
    uint32_t src_bpp = gxm_transfer_bpp(srcFormat);
    uint32_t dst_bpp = gxm_transfer_bpp(destFormat);

    (void)syncObject;
    (void)syncFlags;
    (void)notification;

    if (!srcAddress || !destAddress || !srcWidth || !srcHeight || src_bpp != dst_bpp)
        return SCE_GXM_ERROR_INVALID_VALUE;

    for (unsigned int y = 0; y < srcHeight / 2; y++) {
        for (unsigned int x = 0; x < srcWidth / 2; x++) {
            const uint8_t *p00 = (const uint8_t *)srcAddress +
                                 (size_t)(srcY + y * 2) * (size_t)srcStride * src_bpp +
                                 (size_t)(srcX + x * 2) * src_bpp;
            const uint8_t *p01 = (const uint8_t *)srcAddress +
                                 (size_t)(srcY + y * 2) * (size_t)srcStride * src_bpp +
                                 (size_t)(srcX + x * 2 + 1) * src_bpp;
            const uint8_t *p10 = (const uint8_t *)srcAddress +
                                 (size_t)(srcY + y * 2 + 1) * (size_t)srcStride * src_bpp +
                                 (size_t)(srcX + x * 2) * src_bpp;
            const uint8_t *p11 = (const uint8_t *)srcAddress +
                                 (size_t)(srcY + y * 2 + 1) * (size_t)srcStride * src_bpp +
                                 (size_t)(srcX + x * 2 + 1) * src_bpp;
            uint8_t *dst = (uint8_t *)destAddress +
                           (size_t)(destY + y) * (size_t)destStride * dst_bpp +
                           (size_t)(destX + x) * dst_bpp;

            for (uint32_t c = 0; c < dst_bpp; c++)
                dst[c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4);
        }
    }

    return 0;
}

EXPORT(SceGxm, 0x45229C39, int, sceGxmTransferFinish, void)
{
    if (g_gl)
        g_gl->glFinish();
    return 0;
}

/* ------------------------------------------------------- deferred contexts */

EXPORT(SceGxm, 0x249D5B00, int, sceGxmCreateDeferredContext,
       const SceGxmDeferredContextParams *params, SceGxmContext **context)
{
    SceGxmContext *ctx;

    if (!params || !context || !params->hostMem)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (params->hostMemSize < sizeof(SceGxmContext))
        return SCE_GXM_ERROR_INVALID_VALUE;

    ctx = params->hostMem;
    memset(ctx, 0, sizeof(*ctx));
    gxm_context_init_defaults(ctx, true);
    if (!ctx->vertex_uniform_arena || !ctx->fragment_uniform_arena) {
        free(ctx->vertex_uniform_arena);
        free(ctx->fragment_uniform_arena);
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    }

    ctx->vdm_buffer = params->vdmRingBufferMem;
    ctx->vdm_buffer_size = params->vdmRingBufferMemSize;
    ctx->vertex_buffer = params->vertexRingBufferMem;
    ctx->vertex_buffer_size = params->vertexRingBufferMemSize;
    ctx->fragment_buffer = params->fragmentRingBufferMem;
    ctx->fragment_buffer_size = params->fragmentRingBufferMemSize;

    *context = ctx;
    return 0;
}

EXPORT(SceGxm, 0xD6A2FF2F, int, sceGxmDestroyDeferredContext, SceGxmContext *context)
{
    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;

    gxm_command_free(context->cmd_head);
    context->cmd_head = NULL;
    context->cmd_tail = NULL;
    free(context->vertex_uniform_arena);
    free(context->fragment_uniform_arena);
    context->vertex_uniform_arena = NULL;
    context->fragment_uniform_arena = NULL;
    context->deferred = false;
    return 0;
}

EXPORT(SceGxm, 0x944D3F83, int, sceGxmBeginCommandList, SceGxmContext *context)
{
    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (context->command_list_active)
        return SCE_GXM_ERROR_WITHIN_COMMAND_LIST;

    gxm_command_free(context->cmd_head);
    context->cmd_head = NULL;
    context->cmd_tail = NULL;
    context->vertex_uniform_head = 0;
    context->fragment_uniform_head = 0;
    context->command_list_active = true;
    return 0;
}

EXPORT(SceGxm, 0x36D85916, int, sceGxmEndCommandList, SceGxmContext *context,
       SceGxmCommandList *list)
{
    GxmCommandListEntry *entry;

    if (!context || !list)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (!context->command_list_active)
        return SCE_GXM_ERROR_NOT_WITHIN_COMMAND_LIST;

    entry = gxm_command_list_entry(list, true);
    if (!entry) {
        gxm_command_free(context->cmd_head);
        context->cmd_head = NULL;
        context->cmd_tail = NULL;
        context->command_list_active = false;
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    }

    gxm_command_free(entry->head);
    entry->head = context->cmd_head;
    entry->tail = context->cmd_tail;
    context->cmd_head = NULL;
    context->cmd_tail = NULL;
    context->command_list_active = false;

    memset(list, 0, sizeof(*list));
    list->words[0] = GXM_COMMAND_LIST_MAGIC;
    list->words[1] = entry->index;
    return 0;
}

EXPORT(SceGxm, 0xE8E139DD, int, sceGxmExecuteCommandList, SceGxmContext *context,
       SceGxmCommandList *list)
{
    GxmCommandListEntry *entry;

    if (!context || !list)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;

    entry = gxm_command_list_lookup(list);
    if (!entry)
        return SCE_GXM_ERROR_INVALID_VALUE;

    return gxm_replay_commands(context, entry->head);
}

EXPORT(SceGxm, 0xAD8C2EBA, int, sceGxmGetDeferredContextVdmBuffer,
       const SceGxmContext *context, void **buffer, unsigned int *size)
{
    if (!context || !buffer || !size)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;

    *buffer = context->vdm_buffer;
    *size = context->vdm_buffer_size;
    return 0;
}

EXPORT(SceGxm, 0x87632B9C, int, sceGxmGetDeferredContextVertexBuffer,
       const SceGxmContext *context, void **buffer)
{
    if (!context || !buffer)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;

    *buffer = context->vertex_buffer;
    return 0;
}

EXPORT(SceGxm, 0xFA081D05, int, sceGxmGetDeferredContextFragmentBuffer,
       const SceGxmContext *context, void **buffer)
{
    if (!context || !buffer)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;

    *buffer = context->fragment_buffer;
    return 0;
}

EXPORT(SceGxm, 0x0B9D13CE, void, sceGxmSetDeferredContextVdmBuffer, SceGxmContext *context,
       void *buffer, unsigned int size)
{
    if (!context || !context->deferred)
        return;
    context->vdm_buffer = buffer;
    context->vdm_buffer_size = size;
}

EXPORT(SceGxm, 0x27CAD127, void, sceGxmSetDeferredContextVertexBuffer, SceGxmContext *context,
       void *buffer)
{
    if (!context || !context->deferred)
        return;
    context->vertex_buffer = buffer;
}

EXPORT(SceGxm, 0xC6B3FCD0, void, sceGxmSetDeferredContextFragmentBuffer, SceGxmContext *context,
       void *buffer)
{
    if (!context || !context->deferred)
        return;
    context->fragment_buffer = buffer;
}

#define GXM_PRECOMPUTED_MAGIC 0x47504d31u /* "GPM1" */

typedef struct {
    uint32_t magic;
    uint32_t pointer;
} GxmPrecomputedHeader;

typedef struct {
    const SceGxmVertexProgram *program;
    void *default_uniform;
    const void *uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];
    SceGxmTexture textures[SCE_GXM_MAX_TEXTURE_UNITS];
} GxmPrecomputedVertex;

typedef struct {
    const SceGxmFragmentProgram *program;
    void *default_uniform;
    const void *uniform_buffers[SCE_GXM_MAX_UNIFORM_BUFFERS];
    SceGxmTexture textures[SCE_GXM_MAX_TEXTURE_UNITS];
    SceGxmAuxiliarySurface auxiliary[SCE_GXM_MAX_AUXILIARY_SURFACES];
} GxmPrecomputedFragment;

typedef struct {
    const SceGxmVertexProgram *program;
    const void *streams[SCE_GXM_MAX_VERTEX_STREAMS];
    SceGxmPrimitiveType primitive;
    SceGxmIndexFormat index_format;
    const void *index_data;
    unsigned int index_count;
    unsigned int index_wrap;
} GxmPrecomputedDrawState;

static void *gxm_precomputed_alloc(unsigned int *words, size_t size)
{
    GxmPrecomputedHeader *header = (GxmPrecomputedHeader *)words;
    void *state;

    if (header->magic == GXM_PRECOMPUTED_MAGIC && header->pointer)
        free((void *)(uintptr_t)header->pointer);

    state = calloc(1, size);
    if (!state)
        return NULL;

    header->magic = GXM_PRECOMPUTED_MAGIC;
    header->pointer = (uint32_t)(uintptr_t)state;
    return state;
}

static void *gxm_precomputed_get(const unsigned int *words)
{
    const GxmPrecomputedHeader *header = (const GxmPrecomputedHeader *)words;

    return header->magic == GXM_PRECOMPUTED_MAGIC ? (void *)(uintptr_t)header->pointer : NULL;
}

EXPORT(SceGxm, 0x9D83CA3B, unsigned int, sceGxmGetPrecomputedVertexStateSize,
       const SceGxmVertexProgram *vertexProgram)
{
    (void)vertexProgram;
    return sizeof(SceGxmPrecomputedVertexState);
}

EXPORT(SceGxm, 0x85DE8506, unsigned int, sceGxmGetPrecomputedFragmentStateSize,
       const SceGxmFragmentProgram *fragmentProgram)
{
    (void)fragmentProgram;
    return sizeof(SceGxmPrecomputedFragmentState);
}

EXPORT(SceGxm, 0x41BBD792, unsigned int, sceGxmGetPrecomputedDrawSize,
       const SceGxmVertexProgram *vertexProgram)
{
    (void)vertexProgram;
    return sizeof(SceGxmPrecomputedDraw);
}

EXPORT(SceGxm, 0xBE937F8D, int, sceGxmPrecomputedVertexStateInit,
       SceGxmPrecomputedVertexState *precomputedState, const SceGxmVertexProgram *vertexProgram,
       void *memBlock)
{
    GxmPrecomputedVertex *state;

    if (!precomputedState || !vertexProgram || !memBlock)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if ((uintptr_t)memBlock & 0xf)
        return SCE_GXM_ERROR_INVALID_ALIGNMENT;

    state = gxm_precomputed_alloc(precomputedState->data, sizeof(*state));
    if (!state)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    state->program = vertexProgram;
    return 0;
}

EXPORT(SceGxm, 0x34BF64E3, void, sceGxmPrecomputedVertexStateSetDefaultUniformBuffer,
       SceGxmPrecomputedVertexState *precomputedState, void *defaultBuffer)
{
    GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    if (state)
        state->default_uniform = defaultBuffer;
}

EXPORT(SceGxm, 0xBE5A68EF, void *, sceGxmPrecomputedVertexStateGetDefaultUniformBuffer,
       const SceGxmPrecomputedVertexState *precomputedState)
{
    const GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    return state ? state->default_uniform : NULL;
}

EXPORT(SceGxm, 0xC40C9127, int, sceGxmPrecomputedVertexStateSetAllTextures,
       SceGxmPrecomputedVertexState *precomputedState, const SceGxmTexture *textures)
{
    GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !textures)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->textures, textures, sizeof(state->textures));
    return 0;
}

EXPORT(SceGxm, 0x6A29EB06, int, sceGxmPrecomputedVertexStateSetTexture,
       SceGxmPrecomputedVertexState *precomputedState, unsigned int textureIndex,
       const SceGxmTexture *texture)
{
    GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !texture || textureIndex >= SCE_GXM_MAX_TEXTURE_UNITS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    state->textures[textureIndex] = *texture;
    return 0;
}

EXPORT(SceGxm, 0x0389861D, int, sceGxmPrecomputedVertexStateSetAllUniformBuffers,
       SceGxmPrecomputedVertexState *precomputedState, const void *const *bufferDataArray)
{
    GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !bufferDataArray)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->uniform_buffers, bufferDataArray, sizeof(state->uniform_buffers));
    return 0;
}

EXPORT(SceGxm, 0xDBF97ED6, int, sceGxmPrecomputedVertexStateSetUniformBuffer,
       SceGxmPrecomputedVertexState *precomputedState, unsigned int bufferIndex,
       const void *bufferData)
{
    GxmPrecomputedVertex *state = gxm_precomputed_get(precomputedState->data);
    if (!state || bufferIndex >= SCE_GXM_MAX_UNIFORM_BUFFERS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    state->uniform_buffers[bufferIndex] = bufferData;
    return 0;
}

EXPORT(SceGxm, 0xE297D7AF, int, sceGxmPrecomputedFragmentStateInit,
       SceGxmPrecomputedFragmentState *precomputedState,
       const SceGxmFragmentProgram *fragmentProgram, void *memBlock)
{
    GxmPrecomputedFragment *state;

    if (!precomputedState || !fragmentProgram || !memBlock)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if ((uintptr_t)memBlock & 0xf)
        return SCE_GXM_ERROR_INVALID_ALIGNMENT;

    state = gxm_precomputed_alloc(precomputedState->data, sizeof(*state));
    if (!state)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    state->program = fragmentProgram;
    return 0;
}

EXPORT(SceGxm, 0x91236858, void, sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer,
       SceGxmPrecomputedFragmentState *precomputedState, void *defaultBuffer)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (state)
        state->default_uniform = defaultBuffer;
}

EXPORT(SceGxm, 0xCECB584A, void *, sceGxmPrecomputedFragmentStateGetDefaultUniformBuffer,
       const SceGxmPrecomputedFragmentState *precomputedState)
{
    const GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    return state ? state->default_uniform : NULL;
}

EXPORT(SceGxm, 0xC383DE39, int, sceGxmPrecomputedFragmentStateSetAllTextures,
       SceGxmPrecomputedFragmentState *precomputedState, const SceGxmTexture *textureArray)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !textureArray)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->textures, textureArray, sizeof(state->textures));
    return 0;
}

EXPORT(SceGxm, 0x29118BF1, int, sceGxmPrecomputedFragmentStateSetTexture,
       SceGxmPrecomputedFragmentState *precomputedState, unsigned int textureIndex,
       const SceGxmTexture *texture)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !texture || textureIndex >= SCE_GXM_MAX_TEXTURE_UNITS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    state->textures[textureIndex] = *texture;
    return 0;
}

EXPORT(SceGxm, 0x5A783DC3, int, sceGxmPrecomputedFragmentStateSetAllUniformBuffers,
       SceGxmPrecomputedFragmentState *precomputedState, const void *const *bufferDataArray)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !bufferDataArray)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->uniform_buffers, bufferDataArray, sizeof(state->uniform_buffers));
    return 0;
}

EXPORT(SceGxm, 0xB452F1FB, int, sceGxmPrecomputedFragmentStateSetUniformBuffer,
       SceGxmPrecomputedFragmentState *precomputedState, unsigned int bufferIndex,
       const void *bufferData)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (!state || bufferIndex >= SCE_GXM_MAX_UNIFORM_BUFFERS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    state->uniform_buffers[bufferIndex] = bufferData;
    return 0;
}

EXPORT(SceGxm, 0x9D93B63A, int, sceGxmPrecomputedFragmentStateSetAllAuxiliarySurfaces,
       SceGxmPrecomputedFragmentState *precomputedState,
       const SceGxmAuxiliarySurface *auxSurfaceArray)
{
    GxmPrecomputedFragment *state = gxm_precomputed_get(precomputedState->data);
    if (!state || !auxSurfaceArray)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->auxiliary, auxSurfaceArray, sizeof(state->auxiliary));
    return 0;
}

EXPORT(SceGxm, 0xA197F096, int, sceGxmPrecomputedDrawInit,
       SceGxmPrecomputedDraw *precomputedDraw, const SceGxmVertexProgram *vertexProgram,
       void *memBlock)
{
    GxmPrecomputedDrawState *state;

    if (!precomputedDraw || !vertexProgram || !memBlock)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if ((uintptr_t)memBlock & 0xf)
        return SCE_GXM_ERROR_INVALID_ALIGNMENT;

    state = gxm_precomputed_alloc(precomputedDraw->data, sizeof(*state));
    if (!state)
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    state->program = vertexProgram;
    state->primitive = SCE_GXM_PRIMITIVE_TRIANGLES;
    state->index_format = SCE_GXM_INDEX_FORMAT_U16;
    return 0;
}

EXPORT(SceGxm, 0xB6C6F571, int, sceGxmPrecomputedDrawSetAllVertexStreams,
       SceGxmPrecomputedDraw *precomputedDraw, const void *const *streamDataArray)
{
    GxmPrecomputedDrawState *state = gxm_precomputed_get(precomputedDraw->data);
    if (!state || !streamDataArray)
        return SCE_GXM_ERROR_INVALID_POINTER;
    memcpy(state->streams, streamDataArray, sizeof(state->streams));
    return 0;
}

EXPORT(SceGxm, 0x6C936214, int, sceGxmPrecomputedDrawSetVertexStream,
       SceGxmPrecomputedDraw *precomputedDraw, unsigned int streamIndex, const void *streamData)
{
    GxmPrecomputedDrawState *state = gxm_precomputed_get(precomputedDraw->data);
    if (!state || streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
        return SCE_GXM_ERROR_INVALID_VALUE;
    state->streams[streamIndex] = streamData;
    return 0;
}

EXPORT(SceGxm, 0x884D0D08, void, sceGxmPrecomputedDrawSetParams,
       SceGxmPrecomputedDraw *precomputedDraw, SceGxmPrimitiveType primType,
       SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount)
{
    GxmPrecomputedDrawState *state = gxm_precomputed_get(precomputedDraw->data);
    if (!state)
        return;
    state->primitive = primType;
    state->index_format = indexType;
    state->index_data = indexData;
    state->index_count = indexCount;
    state->index_wrap = indexCount;
}

EXPORT(SceGxm, 0x3A7B1633, void, sceGxmPrecomputedDrawSetParamsInstanced,
       SceGxmPrecomputedDraw *precomputedDraw, SceGxmPrimitiveType primType,
       SceGxmIndexFormat indexType, const void *indexData, unsigned int indexCount,
       unsigned int indexWrap)
{
    GxmPrecomputedDrawState *state = gxm_precomputed_get(precomputedDraw->data);
    if (!state)
        return;
    state->primitive = primType;
    state->index_format = indexType;
    state->index_data = indexData;
    state->index_count = indexCount;
    state->index_wrap = indexWrap;
}

EXPORT(SceGxm, 0xB7626A93, void, sceGxmSetPrecomputedVertexState, SceGxmContext *context,
       const SceGxmPrecomputedVertexState *precomputedState)
{
    context->precomputed_vertex = precomputedState;
}

EXPORT(SceGxm, 0xF8952750, void, sceGxmSetPrecomputedFragmentState, SceGxmContext *context,
       const SceGxmPrecomputedFragmentState *precomputedState)
{
    context->precomputed_fragment = precomputedState;
}

static void gxm_apply_precomputed_state(SceGxmContext *context)
{
    if (context->precomputed_vertex) {
        GxmPrecomputedVertex *state = gxm_precomputed_get(context->precomputed_vertex->data);

        if (state) {
            context->vertex_program = state->program;
            context->vertex_uniforms = state->default_uniform;
            memcpy(context->vertex_uniform_buffers, state->uniform_buffers,
                   sizeof(context->vertex_uniform_buffers));
            memcpy(context->vertex_textures, state->textures, sizeof(context->vertex_textures));
        }
    }

    if (context->precomputed_fragment) {
        GxmPrecomputedFragment *state = gxm_precomputed_get(context->precomputed_fragment->data);

        if (state) {
            context->fragment_program = state->program;
            context->fragment_uniforms = state->default_uniform;
            memcpy(context->fragment_uniform_buffers, state->uniform_buffers,
                   sizeof(context->fragment_uniform_buffers));
            memcpy(context->fragment_textures, state->textures,
                   sizeof(context->fragment_textures));
        }
    }
}

EXPORT(SceGxm, 0xED3F78B8, int, sceGxmDrawPrecomputed, SceGxmContext *context,
       const SceGxmPrecomputedDraw *precomputedDraw)
{
    GxmPrecomputedDrawState *state;

    if (!context || !precomputedDraw)
        return SCE_GXM_ERROR_INVALID_POINTER;

    state = gxm_precomputed_get(precomputedDraw->data);
    if (!state || !state->index_data)
        return SCE_GXM_ERROR_INVALID_VALUE;

    gxm_apply_precomputed_state(context);

    for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        if (state->streams[i])
            context->streams[i].data = state->streams[i];
    }

    if (state->index_wrap && state->index_wrap < state->index_count)
        return gxm_draw_common(context, state->primitive, state->index_format,
                               state->index_data, state->index_wrap,
                               state->index_count / state->index_wrap);

    return gxm_draw_common(context, state->primitive, state->index_format, state->index_data,
                           state->index_count, 1);
}

EXPORT(SceGxm, 0x4709CF5A, int, sceGxmBeginSceneEx, SceGxmContext *context, unsigned int flags,
       const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
       SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
       const SceGxmColorSurface *colorSurface,
       const SceGxmDepthStencilSurface *loadDepthStencilSurface,
       const SceGxmDepthStencilSurface *storeDepthStencilSurface)
{
    int result = sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject,
                                  fragmentSyncObject, colorSurface, loadDepthStencilSurface);

    if (result == 0 && context) {
        context->ds_store_surface =
            (storeDepthStencilSurface && !gxm_ds_surface_is_disabled(storeDepthStencilSurface))
                ? storeDepthStencilSurface
                : NULL;
        /*
         * The store surface may be the only one supplied: the depth data still
         * has to be written back at the end of the scene.
         */
        if (!context->ds_surface && context->ds_store_surface)
            context->ds_surface = context->ds_store_surface;
    }

    return result;
}

EXPORT(SceGxm, 0x2A1BCDDB, int, sceGxmGetContextType, const SceGxmContext *context,
       unsigned int *type)
{
    if (!context || !type)
        return SCE_GXM_ERROR_INVALID_POINTER;
    *type = context->deferred ? 1 : 0; /* SCE_GXM_CONTEXT_TYPE_DEFERRED / IMMEDIATE */
    return 0;
}

EXPORT(SceGxm, 0xCE0B0A76, SceGxmPassType, sceGxmFragmentProgramGetPassType,
       const SceGxmFragmentProgram *fragmentProgram)
{
    (void)fragmentProgram;
    return SCE_GXM_PASS_TYPE_OPAQUE;
}

EXPORT(SceGxm, 0x5748367E, SceBool, sceGxmFragmentProgramIsEnabled,
       const SceGxmFragmentProgram *fragmentProgram)
{
    return fragmentProgram && !fragmentProgram->mask_update;
}

EXPORT(SceGxm, 0xE11603B1, int, sceGxmProgramGetOutputRegisterFormat,
       const SceGxmProgram *program, SceGxmParameterType *type, unsigned int *componentCount)
{
    const uint8_t *base;

    if (!program || !type || !componentCount)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (!(program->program_flags & 1))
        return SCE_GXM_ERROR_INVALID_VALUE;

    base = (const uint8_t *)&program->varyings_offset + program->varyings_offset;
    *type = (SceGxmParameterType)base[10];
    *componentCount = base[11];
    return 0;
}

EXPORT(SceGxm, 0xE23C838C, SceBool, sceGxmProgramIsEquivalent, const SceGxmProgram *programA,
       const SceGxmProgram *programB)
{
    if (!programA || !programB)
        return 0;
    return programA->size == programB->size &&
                   memcmp(programA, programB, programA->size) == 0
               ? 1
               : 0;
}

EXPORT(SceGxm, 0x6ABF3F76, int, sceGxmGetParameterBufferThreshold, unsigned int *size)
{
    if (!size)
        return SCE_GXM_ERROR_INVALID_POINTER;
    *size = 256 * 1024;
    return 0;
}

EXPORT(SceGxm, 0x873B07C0, SceBool, sceGxmIsDebugVersion, void)
{
    return 0;
}

EXPORT(SceGxm, 0x12D18B3D, void, sceGxmSetWarningEnabled, SceBool enable)
{
    (void)enable;
}

EXPORT(SceGxm, 0xE05277D6, int, sceGxmVertexFence, SceGxmContext *context)
{
    (void)context;
    if (g_gl)
        g_gl->glFinish();
    return 0;
}

EXPORT(SceGxm, 0x8BD94593, int, sceGxmWaitEvent, void)
{
    if (g_gl)
        g_gl->glFinish();
    return 0;
}

EXPORT(SceGxm, 0xE9E81073, int, sceGxmAddRazorGpuCaptureBuffer, void)
{
    return 0;
}

EXPORT(SceGxm, 0x0468E3F2, int, sceGxmRemoveRazorGpuCaptureBuffer, void)
{
    return 0;
}

/* Underscore-prefixed aliases some applications import. */

EXPORT(SceGxm, 0xDBA33160, int, _sceGxmBeginScene, SceGxmContext *context, unsigned int flags,
       const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
       SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
       const SceGxmColorSurface *colorSurface, const SceGxmDepthStencilSurface *depthStencil)
{
    return sceGxmBeginScene(context, flags, renderTarget, validRegion, vertexSyncObject,
                            fragmentSyncObject, colorSurface, depthStencil);
}

EXPORT(SceGxm, 0x51FE0899, int, _sceGxmMidSceneFlush, SceGxmContext *context, unsigned int flags,
       SceGxmSyncObject *vertexSyncObject, const SceGxmNotification *vertexNotification)
{
    return sceGxmMidSceneFlush(context, flags, vertexSyncObject, vertexNotification);
}

EXPORT(SceGxm, 0x16C9D339, int, _sceGxmSetVertexTexture, SceGxmContext *context,
       unsigned int textureIndex, const SceGxmTexture *texture)
{
    return sceGxmSetVertexTexture(context, textureIndex, texture);
}

EXPORT(SceGxm, 0x5A690B60, int, _sceGxmTextureSetWidth, SceGxmTexture *texture,
       unsigned int width)
{
    return sceGxmTextureSetWidth(texture, width);
}

EXPORT(SceGxm, 0x1B20D5DF, int, _sceGxmTextureSetHeight, SceGxmTexture *texture,
       unsigned int height)
{
    return sceGxmTextureSetHeight(texture, height);
}

EXPORT(SceGxm, 0x7FFFDD7A, const SceGxmProgramParameter *,
       _sceGxmProgramFindParameterBySemantic, const SceGxmProgram *program,
       SceGxmParameterSemantic semantic, unsigned int index)
{
    return sceGxmProgramFindParameterBySemantic(program, semantic, index);
}

EXPORT(SceGxm, 0xAAFD61D5, SceGxmParameterSemantic, _sceGxmProgramParameterGetSemantic,
       const SceGxmProgramParameter *parameter)
{
    return sceGxmProgramParameterGetSemantic(parameter);
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
