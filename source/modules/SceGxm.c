/*
 * SceGxm on GLES 3.x.
 *
 * The guest renders into a GLES framebuffer owned by SceGxmRenderTarget. GXP
 * shaders are compiled through Vita3K's recompiler (GXP -> SPIR-V) and
 * SPIRV-Cross (SPIR-V -> GLSL ES 3.10). Default uniforms are fed through the
 * same bindings Vita3K's SPIR-V uses (0 = vertex, 1 = fragment) but as uniform
 * blocks: GLES 3.1 does not require vertex storage buffers to exist, and
 * Panfrost on Mali reports zero of them. Render info uses uniform buffers 2/3
 * and textures use sampler bindings equal to the GXM texture unit.
 *
 * At sceGxmEndScene the color target is read back into the guest's
 * SceGxmColorSurface, which is what sceDisplaySetFrameBuf presents.
 */
#include <limits.h>
#include <math.h>
#include <psp2/gxm.h>
#include <psp2/kernel/error.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "fs.h"
#include "gfx.h"
#include "gxm/texture.h"
#include "gxm/util.h"
#include "host.h"
#include "host_gfx_gl.h"
#include "log.h"
#include "module.h"
#include "spirv_cross_glsl_es.h"
#include "util.h"

/* stb_image_write is linked into the emulator, but its header is not on every
   target's include path (the smoke tests compile this file too). */
extern int stbi_write_png(const char *filename, int w, int h, int comp, const void *data,
                          int stride_in_bytes);
#include "vita3k_shader_recompiler_iface_c.h"

extern int vita3k_gxm_get_attribute_location(const SceGxmProgram *program, uint16_t reg_index);

#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

/* Not in GLES, but some desktop-derived blobs accept it in glReadPixels. */
#ifndef GL_STENCIL_INDEX
#define GL_STENCIL_INDEX 0x1901
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
    const void *data;
} GxmStream;

/*
 * Per-frame upload arena. Re-uploading into a buffer the GPU is still reading
 * makes the driver stall or reallocate, which on Panfrost cost more than the
 * draw itself (847 draws per VitaShell frame). All per-draw data is appended
 * into a fresh region instead and the whole arena is orphaned once per frame.
 */
typedef struct {
    GLuint buffer;
    uint32_t capacity;
    uint32_t used;
    uint8_t *stage;
    uint32_t stage_capacity;
} GxmBufferArena;

/*
 * Everything a draw needs besides its vertex and index data. Consecutive
 * draws whose state compares equal can be appended to one GL draw, which is
 * the difference between 847 tiny draws and a handful of larger ones on a
 * tile-based GPU.
 */
typedef struct {
    GLuint program;
    const SceGxmColorSurfaceInner *color_surface;
    uint32_t surface_width;
    uint32_t surface_height;
    const SceGxmDepthStencilSurface *ds_surface;
    SceGxmViewportMode viewport_enable;
    float viewport_x_offset, viewport_x_scale;
    float viewport_y_offset, viewport_y_scale;
    float viewport_z_offset, viewport_z_scale;
    SceGxmRegionClipMode region_clip_mode;
    unsigned int region_clip_xmin, region_clip_ymin;
    unsigned int region_clip_xmax, region_clip_ymax;
    SceGxmCullMode cull_mode;
    SceGxmPolygonMode polygon_mode;
    unsigned int point_line_width;
    int depth_bias_factor, depth_bias_units;
    SceGxmTwoSidedMode two_sided;
    SceGxmFragmentProgramMode front_fragment_program_enable;
    SceGxmFragmentProgramMode back_fragment_program_enable;
    SceGxmDepthFunc depth_func;
    SceGxmDepthWriteMode depth_write;
    SceGxmStencilFunc front_stencil_func, back_stencil_func;
    SceGxmStencilOp front_stencil_fail, front_stencil_depth_fail, front_stencil_pass;
    SceGxmStencilOp back_stencil_fail, back_stencil_depth_fail, back_stencil_pass;
    uint8_t front_stencil_compare_mask, front_stencil_write_mask;
    uint8_t back_stencil_compare_mask, back_stencil_write_mask;
    unsigned int front_stencil_ref, back_stencil_ref;
    SceGxmPrimitiveType prim;
    SceGxmIndexFormat index_format;
    unsigned int instances;
} GxmDrawState;

typedef struct {
    bool active;
    GxmDrawState state;
    const SceGxmVertexProgram *vertex_program;
    SceGxmTextureInner fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    SceGxmTextureInner vertex_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    uint8_t *vertex_uniforms;
    uint8_t *fragment_uniforms;
    uint32_t vertex_uniform_size;
    uint32_t fragment_uniform_size;
    uint32_t index_start;
    uint32_t index_count;
    uint32_t vertex_count;
    uint32_t draws;
    uint32_t stream_start[SCE_GXM_MAX_VERTEX_STREAMS];
    GLenum gl_index_type;
    GLenum gl_primitive;
} GxmDrawBatch;

typedef struct {
    GLuint tex;
    GLenum target;
    const void *data;
    const void *palette;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t mip_count;
    uint32_t gamma_mode;
    uint32_t hash;
    uint64_t hash_frame;
    uint64_t last_used;
    bool valid;
} GxmTextureCache;

/*
 * A single-slot cache per texture unit thrashes as soon as a frame alternates
 * two textures on one unit (VitaShell switches between its 512x512 font atlas
 * and small icons), which re-decoded the atlas every frame. The cache is
 * global and bounded instead; entries are keyed by content hash, so a texture
 * that is genuinely rewritten still gets re-uploaded.
 */
#define GXM_TEXTURE_CACHE_SLOTS 64

static GxmTextureCache g_texture_cache[GXM_TEXTURE_CACHE_SLOTS];
static uint64_t g_texture_clock;
static uint64_t g_gxm_frame;
static unsigned int g_profile_draws;
static unsigned int g_profile_vertices;
static unsigned int g_profile_uploads;
static uint64_t g_profile_upload_bytes;
static uint64_t g_profile_draw_ns;
static uint64_t g_profile_flush_ns;
static uint64_t g_profile_commit_ns;
static uint64_t g_profile_gldraw_ns;
static uint64_t g_profile_ds_ns;
static uint64_t g_profile_ds_load_ns;
static unsigned int g_profile_ds_load_calls;
static uint64_t g_profile_uniform_ns;
static uint64_t g_profile_state_ns;
static uint64_t g_profile_texture_ns;
static uint64_t g_profile_attrib_ns;
/* Why the previous batch could not absorb the next draw. */
static unsigned int g_batch_break_state;
static unsigned int g_batch_break_program;
static unsigned int g_batch_break_surface;
static unsigned int g_batch_break_clip;
static unsigned int g_batch_break_state_other;
static unsigned int g_batch_break_prim;
static unsigned int g_batch_break_viewport;
static unsigned int g_batch_break_depth;
static unsigned int g_batch_break_depth_func;
static unsigned int g_batch_break_depth_write;
static unsigned int g_batch_break_depth_bias;
static unsigned int g_batch_break_stencil;
static unsigned int g_batch_break_rest;
static unsigned int g_batch_break_size;
static unsigned int g_batch_break_uniform;
static unsigned int g_batch_break_texture;

/*
 * sRGB transfer functions. A gamma colour surface stores sRGB-encoded values
 * while shaders and blending work in linear space, so writeback encodes. Gamma
 * textures are decoded on the CPU during layout conversion so the result does
 * not depend on driver sRGB support.
 */
static uint8_t s_linear_to_srgb[256];
static uint8_t s_srgb_to_linear[256];
static bool s_color_tables_ready;

static uint8_t gxm_linear_to_srgb_u8(uint8_t value)
{
    float c = value / 255.0f;
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    int v = (int)(s * 255.0f + 0.5f);

    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static uint8_t gxm_srgb_to_linear_u8(uint8_t value)
{
    float c = value / 255.0f;
    float l = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    int v = (int)(l * 255.0f + 0.5f);

    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void gxm_color_tables_init(void)
{
    for (int i = 0; i < 256; i++) {
        s_linear_to_srgb[i] = gxm_linear_to_srgb_u8((uint8_t)i);
        s_srgb_to_linear[i] = gxm_srgb_to_linear_u8((uint8_t)i);
    }
    s_color_tables_ready = true;
}

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

    uint8_t *vertex_block_data;
    uint8_t *fragment_block_data;
    uint32_t vertex_block_size;
    uint32_t fragment_block_size;

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

    GLuint ubo[2];
    uint32_t ubo_capacity[2];
    GLuint uniform_block[2];
    uint32_t uniform_block_capacity[2];
    struct GXMRenderVertUniformBlock last_vert_info;
    struct GXMRenderFragUniformBlock last_frag_info;
    bool vert_info_valid;
    bool frag_info_valid;
    GxmBufferArena stream_arena[SCE_GXM_MAX_VERTEX_STREAMS];
    GxmBufferArena index_arena;
    GxmDrawBatch batch;
    uint8_t *index_scratch;
    uint32_t index_scratch_capacity;

    const SceGxmPrecomputedVertexState *precomputed_vertex;
    const SceGxmPrecomputedFragmentState *precomputed_fragment;

    bool owns_memory;
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

static void gxm_batch_flush(SceGxmContext *context);

typedef struct SceGxmSyncObject {
    int unused;
} SceGxmSyncObject;

typedef struct SceGxmRegisteredProgram {
    const SceGxmProgram *programHeader;
    Vita3kUniformBufferLayout uniform_buffers[GXM_MAX_UNIFORM_BUFFER_LAYOUT];
    uint32_t uniform_buffer_count;
    uint32_t uniform_buffer_size; /* total uniform container size */
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
    uint32_t shader_hash;
    int refcount;
} SceGxmVertexProgram;

typedef struct SceGxmFragmentProgram {
    SceGxmShaderPatcherId programId;
    SceGxmOutputRegisterFormat outputFormat;
    SceGxmMultisampleMode multisampleMode;
    SceGxmBlendInfo blendInfo;
    GLuint shader;
    uint32_t shader_hash;
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

    /* Multisampled draw target; resolved into fbo at EndScene. */
    GLuint msaa_fbo;
    GLuint msaa_color;
    GLuint msaa_depth;
    uint32_t samples;
} SceGxmRenderTarget;

typedef struct {
    uint32_t vs_hash;
    uint32_t fs_hash;
    GLuint program;
    GLint block_vertex_data;
    GLint block_fragment_data;
    GLint block_render_vert;
    GLint block_render_frag;
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
static unsigned int g_linked_next;

#define GXM_MAX_CACHED_SHADERS 128
typedef struct {
    uint32_t hash;
    GLenum stage;
    GLuint shader;
} GxmCachedShader;
static GxmCachedShader g_cached_shaders[GXM_MAX_CACHED_SHADERS];
static unsigned int g_cached_shader_count;
static SceGxmContext *g_current_context;
static GLuint g_bound_texture[2 * SCE_GXM_MAX_TEXTURE_UNITS];
static unsigned int g_active_texture_unit = UINT_MAX;
static GLuint g_bound_uniform_block[2];
static GLuint g_bound_render_ubo[2];
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

/* Keeps the GLSL that the driver rejected, for post-mortem inspection. */
static void gxm_dump_failed_shader(const char *source, const char *name)
{
    char path[PATH_MAX];
    FILE *file;

    if (snprintf(path, sizeof(path), "%s/failed_%s.glsl", VITA2NATIVE_DUMP_SHADER_PATH, name) >=
        (int)sizeof(path))
        return;

    file = fopen(path, "wb");
    if (!file)
        return;

    fwrite(source, 1, strlen(source), file);
    fclose(file);
}

static void gxm_compile_shader(GLuint shader, const char *source, const char *name)
{
    GLint ok = 0;
    GLsizei length = 0;
    char log[1024];

    memset(log, 0, sizeof(log));
    g_gl->glShaderSource(shader, 1, &source, NULL);
    g_gl->glCompileShader(shader);
    g_gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g_gl->glGetShaderInfoLog(shader, sizeof(log), &length, log);
        LOG("SceGxm: %s shader compilation failed (log %d bytes):\n%s", name, (int)length,
            log);
        gxm_dump_failed_shader(source, name);
    }
}

/*
 * GLSL ES source cache.
 *
 * Recompiling GXP -> SPIR-V -> GLSL for every launch is expensive on the
 * low-end ARM32 boards this fork targets. The generated text only depends on
 * the GXP bytes, the stage and (for vertex shaders) the attribute bindings, so
 * it is cached under <vfs root>/cache/shaders. Driver-level shader objects are
 * not portable and are never cached.
 */
#define GXM_SHADER_CACHE_VERSION 2u

static uint32_t gxm_hash_bytes(uint32_t hash, const void *data, size_t size);

static uint32_t gxm_shader_cache_hash(const SceGxmProgram *program, GLenum stage,
                                      const SceGxmVertexAttribute *attributes, unsigned int count)
{
    uint32_t hash = 2166136261u;
    uint32_t version = GXM_SHADER_CACHE_VERSION;
    uint32_t size = program->size;
    uint8_t stage_byte = stage == GL_VERTEX_SHADER ? 0 : 1;

    if (!size || size > (1u << 20))
        size = 0;

    hash = gxm_hash_bytes(hash, &version, sizeof(version));
    hash = gxm_hash_bytes(hash, &stage_byte, sizeof(stage_byte));
    hash = gxm_hash_bytes(hash, &size, sizeof(size));
    hash = gxm_hash_bytes(hash, program, size);
    if (attributes && count)
        hash = gxm_hash_bytes(hash, attributes, count * sizeof(*attributes));

    return hash;
}

static bool gxm_shader_cache_path(uint32_t hash, char *path, size_t path_size, bool create_dirs)
{
    const char *root = host_fs_root();
    char dir[PATH_MAX];

    if (!root || !root[0])
        return false;

    if (snprintf(dir, sizeof(dir), "%s/cache", root) >= (int)sizeof(dir))
        return false;
    if (create_dirs)
        mkdir(dir, 0755);

    if (snprintf(dir, sizeof(dir), "%s/cache/shaders", root) >= (int)sizeof(dir))
        return false;
    if (create_dirs)
        mkdir(dir, 0755);

    return snprintf(path, path_size, "%s/cache/shaders/%08x.glsl", root, hash) < (int)path_size;
}

static char *gxm_shader_cache_read(uint32_t hash)
{
    char path[PATH_MAX];
    struct stat st;
    FILE *file;
    char *text;

    if (!gxm_shader_cache_path(hash, path, sizeof(path), false))
        return NULL;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > (4 << 20))
        return NULL;

    file = fopen(path, "rb");
    if (!file)
        return NULL;

    text = malloc((size_t)st.st_size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }

    if (fread(text, 1, (size_t)st.st_size, file) != (size_t)st.st_size) {
        free(text);
        fclose(file);
        return NULL;
    }

    fclose(file);
    text[st.st_size] = '\0';

    if (strncmp(text, "#version", 8) != 0) {
        free(text);
        return NULL;
    }

    return text;
}

static void gxm_shader_cache_write(uint32_t hash, const char *text)
{
    char path[PATH_MAX];
    FILE *file;

    if (!gxm_shader_cache_path(hash, path, sizeof(path), true))
        return;

    file = fopen(path, "wb");
    if (!file)
        return;

    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

static void gxm_shader_cache_remove(uint32_t hash)
{
    char path[PATH_MAX];

    if (gxm_shader_cache_path(hash, path, sizeof(path), false))
        remove(path);
}

/* Optional dumps of the USSE disassembly (and SPIR-V) that the translator
   produces for a shader, keyed by the same hash as the GLSL cache. */
static uint32_t g_shader_dump_hash;
static const char *g_shader_dump_dir;

static bool gxm_shader_dump(const char *ext, const char *dump)
{
    char path[PATH_MAX];
    FILE *file;

    if (!g_shader_dump_dir)
        return false;

    snprintf(path, sizeof(path), "%s/%08x.%s", g_shader_dump_dir, g_shader_dump_hash, ext);
    file = fopen(path, "wb");
    if (!file)
        return false;

    fwrite(dump, 1, strlen(dump), file);
    fclose(file);
    return true;
}

static GLuint gxm_create_compiled_shader(GLenum stage, const char *name, const char *glsl,
                                        bool *compiled)
{
    GLuint shader = g_gl->glCreateShader(stage);
    GLint ok = 0;

    gxm_compile_shader(shader, glsl, name);
    g_gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    *compiled = ok != 0;

    return shader;
}

static GLuint gxm_translate_shader(const SceGxmProgram *program, GLenum stage, const char *name,
                                   const SceGxmVertexAttribute *attributes, unsigned int count,
                                   uint32_t *hash_out)
{
    uint32_t cache_hash = gxm_shader_cache_hash(program, stage, attributes, count);
    char *glsl;
    bool compiled = false;
    GLuint shader;

    if (hash_out)
        *hash_out = cache_hash;

    for (unsigned int i = 0; i < g_cached_shader_count; i++) {
        if (g_cached_shaders[i].hash == cache_hash && g_cached_shaders[i].stage == stage)
            return g_cached_shaders[i].shader;
    }

    glsl = gxm_shader_cache_read(cache_hash);

    if (glsl) {
        shader = gxm_create_compiled_shader(stage, name, glsl, &compiled);
        if (compiled) {
            LOG("SceGxm: shader cache hit %08x (%s) program %p", cache_hash, name,
                (const void *)program);
            free(glsl);
            goto cache_shader;
        }

        LOG("SceGxm: cached shader %08x failed to compile, regenerating", cache_hash);
        g_gl->glDeleteShader(shader);
        gxm_shader_cache_remove(cache_hash);
        free(glsl);
    }

    {
        uint32_t *spirv = NULL;
        uint32_t num_instr = 0;
        const char *dump_dir = getenv("VITA2NATIVE_GXM_SHADER_DUMP");

        g_shader_dump_hash = cache_hash;
        g_shader_dump_dir = dump_dir ? dump_dir : NULL;

        if (!convert_gxp_to_spirv_c(&spirv, &num_instr, program, name, false, false, false, false,
                                    attributes, count, false, false,
                                    g_shader_dump_dir ? gxm_shader_dump : NULL)) {
            g_shader_dump_dir = NULL;
            LOG("SceGxm: GXP -> SPIR-V failed for %s", name);
            return 0;
        }

        g_shader_dump_dir = NULL;

        if (spirv_to_glsl_es(spirv, num_instr, &glsl) != 0) {
            free(spirv);
            LOG("SceGxm: SPIR-V -> GLSL ES failed for %s", name);
            return 0;
        }

        free(spirv);
    }

    LOG("SceGxm: shader cache miss %08x (%s) program %p", cache_hash, name, (const void *)program);
    shader = gxm_create_compiled_shader(stage, name, glsl, &compiled);
    if (compiled)
        gxm_shader_cache_write(cache_hash, glsl);

    free(glsl);

cache_shader:
    /* Titles such as Unity patch the same GXP blobs into fresh program objects
       every frame; recompiling each time dominated the frame and the GL shader
       object is independent of the guest-side program anyway. */
    if (compiled && g_cached_shader_count < GXM_MAX_CACHED_SHADERS) {
        g_cached_shaders[g_cached_shader_count].hash = cache_hash;
        g_cached_shaders[g_cached_shader_count].stage = stage;
        g_cached_shaders[g_cached_shader_count].shader = shader;
        g_cached_shader_count++;
    }

    return shader;
}

static int gxm_program_trace(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_GXM_PROGRAM_TRACE") != NULL;

    return enabled;
}

static GLuint gxm_get_linked_program(const SceGxmVertexProgram *vs, const SceGxmFragmentProgram *fs,
                                     GxmLinkedProgram **linked_out)
{
    GLint ok = 0;
    char log[1024];
    GxmLinkedProgram *linked;
    uint32_t vs_hash = vs ? vs->shader_hash : 0;
    uint32_t fs_hash = fs ? fs->shader_hash : 0;

    for (unsigned int i = 0; i < g_linked_count; i++) {
        if (g_linked[i].vs_hash == vs_hash && g_linked[i].fs_hash == fs_hash) {
            if (linked_out)
                *linked_out = &g_linked[i];
            return g_linked[i].program;
        }
    }

    /* Titles that rebuild shader patcher programs every frame keep producing
       new object pointers for the same shader bytes. Keying on the content
       hash keeps one entry per material variant; the ring replacement below
       only matters if a title really has more variants than the table holds. */
    if (g_linked_count == GXM_MAX_LINKED_PROGRAMS) {
        linked = &g_linked[g_linked_next++ % GXM_MAX_LINKED_PROGRAMS];
        /* A batch that is still open may draw with this program in a moment;
           keep it alive in that case (the draw would otherwise use a deleted
           object). Eviction is rare, so the leak is bounded in practice. */
        if (linked->program &&
            !(g_current_context && g_current_context->batch.active &&
              g_current_context->batch.state.program == linked->program))
            g_gl->glDeleteProgram(linked->program);
        memset(linked, 0, sizeof(*linked));
    } else {
        linked = &g_linked[g_linked_count++];
    }

    GLuint program = g_gl->glCreateProgram();
    if (vs && vs->shader)
        g_gl->glAttachShader(program, vs->shader);
    if (fs && fs->shader)
        g_gl->glAttachShader(program, fs->shader);
    g_gl->glLinkProgram(program);
    g_gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_gl->glGetProgramInfoLog(program, sizeof(log), NULL, log);
        if (log[0] == '\0')
            snprintf(log, sizeof(log), "(no info log)");
        LOG("SceGxm: program link failed (vs=%p hash %08x, fs=%p hash %08x):\n%s",
            (const void *)vs, vs_hash, (const void *)fs, fs_hash, log);
        g_gl->glDeleteProgram(program);
        memset(linked, 0, sizeof(*linked));
        if (g_linked_count && linked == &g_linked[g_linked_count - 1])
            g_linked_count--;
        return 0;
    }

    linked->vs_hash = vs_hash;
    linked->fs_hash = fs_hash;
    linked->program = program;
    linked->block_vertex_data = g_gl->glGetUniformBlockIndex(program, "vertexDataType");
    linked->block_fragment_data = g_gl->glGetUniformBlockIndex(program, "fragmentDataType");
    linked->block_render_vert = g_gl->glGetUniformBlockIndex(program, "renderVertInfo");
    linked->block_render_frag = g_gl->glGetUniformBlockIndex(program, "renderFragInfo");

    /* Block bindings are static for the program's lifetime. */
    if (linked->block_vertex_data != GL_INVALID_INDEX)
        g_gl->glUniformBlockBinding(program, linked->block_vertex_data, 0);
    if (linked->block_fragment_data != GL_INVALID_INDEX)
        g_gl->glUniformBlockBinding(program, linked->block_fragment_data, 1);
    if (linked->block_render_vert != GL_INVALID_INDEX)
        g_gl->glUniformBlockBinding(program, linked->block_render_vert, 2);
    if (linked->block_render_frag != GL_INVALID_INDEX)
        g_gl->glUniformBlockBinding(program, linked->block_render_frag, 3);

    if (linked_out)
        *linked_out = linked;

    return program;
}

static uint32_t gxm_hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    size_t i = 0;

    /* Word-at-a-time: texture hashing runs on every draw, and a byte loop at
       -O0 made a 512x512 atlas cost more than the draw itself. */
    for (; i + 4 <= size; i += 4) {
        uint32_t word;

        memcpy(&word, bytes + i, sizeof(word));
        hash = (hash ^ word) * 16777619u;
        hash ^= hash >> 13;
    }
    for (; i < size; i++) {
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
        for (uint32_t level = 0;; level++) {
            GxmTextureLevel info;

            if (gxm_texture_get_level(texture, face, level, &info) != 0)
                break;
            hash = gxm_hash_bytes(hash, (const uint8_t *)data + info.offset, info.size);
        }
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

static uint64_t gxm_time_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static GxmTextureCache *gxm_texture_cache_find(const SceGxmTextureInner *texture,
                                               const void *data, GLenum target)
{
    uint32_t format = (uint32_t)gxm_texture_get_format(texture);
    uint32_t mip_count = gxm_texture_true_mip_count(texture);
    const void *palette = (const void *)(uintptr_t)(texture->palette_addr << 6);

    for (unsigned int i = 0; i < GXM_TEXTURE_CACHE_SLOTS; i++) {
        GxmTextureCache *cache = &g_texture_cache[i];

        if (!cache->valid)
            continue;
        if (cache->data == data && cache->target == target && cache->format == format &&
            cache->mip_count == mip_count && cache->gamma_mode == texture->gamma_mode &&
            cache->palette == palette)
            return cache;
    }

    return NULL;
}

static void gxm_forget_texture(GLuint tex);

static GxmTextureCache *gxm_texture_cache_alloc(void)
{
    GxmTextureCache *victim = &g_texture_cache[0];
    bool found = false;

    for (unsigned int i = 0; i < GXM_TEXTURE_CACHE_SLOTS; i++) {
        GxmTextureCache *cache = &g_texture_cache[i];

        if (!cache->valid) {
            victim = cache;
            found = true;
            break;
        }
        if (!found || cache->last_used < victim->last_used) {
            victim = cache;
            found = true;
        }
    }

    if (victim->tex)
        g_gl->glDeleteTextures(1, &victim->tex);
    gxm_forget_texture(victim->tex);
    memset(victim, 0, sizeof(*victim));

    return victim;
}

static void gxm_forget_texture(GLuint tex)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(g_bound_texture); i++) {
        if (g_bound_texture[i] == tex)
            g_bound_texture[i] = 0;
    }
}

static void gxm_texture_cache_flush(void)
{
    for (unsigned int i = 0; i < GXM_TEXTURE_CACHE_SLOTS; i++) {
        if (g_texture_cache[i].tex && g_gl)
            g_gl->glDeleteTextures(1, &g_texture_cache[i].tex);
        memset(&g_texture_cache[i], 0, sizeof(g_texture_cache[i]));
    }
    memset(g_bound_texture, 0, sizeof(g_bound_texture));
    g_active_texture_unit = UINT_MAX;
    g_texture_clock = 0;
    g_gxm_frame = 0;
}

static bool gxm_upload_texture(const SceGxmTextureInner *texture, unsigned int unit)
{
    SceGxmTextureType type = (SceGxmTextureType)gxm_texture_get_type(texture);
    bool cube = type == SCE_GXM_TEXTURE_CUBE || type == SCE_GXM_TEXTURE_CUBE_ARBITRARY;
    GLenum target = cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    const void *data = gxm_texture_get_data(texture);
    uint32_t width = (uint32_t)gxm_texture_get_width(texture);
    uint32_t height = (uint32_t)gxm_texture_get_height(texture);
    uint32_t mip_count = gxm_texture_true_mip_count(texture);
    const void *palette = (const void *)(uintptr_t)(texture->palette_addr << 6);
    uint32_t hash;
    GxmTextureCache *cache;
    uint8_t *rgba;
    uint64_t started;

    if (g_active_texture_unit != unit) {
        g_gl->glActiveTexture(GL_TEXTURE0 + unit);
        g_active_texture_unit = unit;
    }

    if (!data || !width || !height || width > 4096 || height > 4096) {
        if (g_bound_texture[unit]) {
            g_gl->glBindTexture(GL_TEXTURE_2D, 0);
            g_gl->glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
            g_bound_texture[unit] = 0;
        }
        return false;
    }

    cache = gxm_texture_cache_find(texture, data, target);

    /*
     * A texture can be bound many times per frame (the same font atlas backs
     * every label), so its content is hashed at most once per frame. Content
     * written mid-frame is picked up on the next frame.
     */
    if (cache && cache->hash_frame == g_gxm_frame) {
        if (g_bound_texture[unit] != cache->tex) {
            g_gl->glBindTexture(target, cache->tex);
            g_bound_texture[unit] = cache->tex;
        }
        gxm_texture_params(texture, target, mip_count);
        cache->last_used = ++g_texture_clock;
        return true;
    }

    hash = gxm_texture_hash(texture, data);

    if (cache && cache->hash == hash) {
        cache->hash_frame = g_gxm_frame;
        if (g_bound_texture[unit] != cache->tex) {
            g_gl->glBindTexture(target, cache->tex);
            g_bound_texture[unit] = cache->tex;
        }
        gxm_texture_params(texture, target, mip_count);
        cache->last_used = ++g_texture_clock;
        return true;
    }

    if (cache) {
        /* Same key, different content: re-upload into the same slot. */
        if (cache->tex) {
            gxm_forget_texture(cache->tex);
            g_gl->glDeleteTextures(1, &cache->tex);
        }
        memset(cache, 0, sizeof(*cache));
        cache->target = target;
    } else {
        cache = gxm_texture_cache_alloc();
        cache->target = target;
    }

    g_gl->glGenTextures(1, &cache->tex);
    g_gl->glBindTexture(target, cache->tex);
    g_bound_texture[unit] = cache->tex;

    rgba = malloc((size_t)width * height * 4);
    if (!rgba) {
        if (cache->tex) {
            gxm_forget_texture(cache->tex);
            g_gl->glDeleteTextures(1, &cache->tex);
        }
        memset(cache, 0, sizeof(*cache));
        g_bound_texture[unit] = 0;
        g_gl->glBindTexture(target, 0);
        return false;
    }

    started = gxm_time_ns();
    g_profile_uploads++;
    g_profile_upload_bytes += (uint64_t)width * height * 4;

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
                    LOG("SceGxm: texture decode failed: fmt=0x%08x type=%d %ux%u layout %ux%u face %u level %u",
                        format, (int)gxm_texture_get_type(texture), (unsigned)info.width,
                        (unsigned)info.height, (unsigned)info.layout_width,
                        (unsigned)info.layout_height, face, level);
                }
                free(rgba);
                gxm_forget_texture(cache->tex);
                g_gl->glDeleteTextures(1, &cache->tex);
                memset(cache, 0, sizeof(*cache));
                g_bound_texture[unit] = 0;
                g_gl->glBindTexture(target, 0);
                return false;
            }

            if (cube)
                upload_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + face;

            if (level == 0 && face == 0) {
                static const char *dump_dir;
                static unsigned int dumped;

                if (!dump_dir)
                    dump_dir = getenv("VITA2NATIVE_DUMP_TEXTURES");
                if (dump_dir && dumped < 200) {
                    char path[PATH_MAX];

                    snprintf(path, sizeof(path), "%s/tex_%08x_%ux%u_%08x.png", dump_dir,
                             (unsigned)gxm_texture_get_format(texture), (unsigned)width,
                             (unsigned)height, hash);
                    if (stbi_write_png(path, (int)width, (int)height, 4, rgba, (int)width * 4))
                        dumped++;
                }
            }

            /*
             * A gamma texture stores sRGB-encoded texels. Decoding is done on
             * the CPU after layout conversion so every driver sees identical
             * linear data (Mesa's GLES path does not always apply the sRGB
             * internal-format decode to texture() reads). The public gamma
             * constants cannot express per-channel modes (the header aliases
             * BGR to R), so any non-zero mode means full sRGB, matching
             * Vita3K.
             */
            if (texture->gamma_mode) {
                if (!s_color_tables_ready)
                    gxm_color_tables_init();
                for (uint32_t i = 0; i < info.width * info.height; i++) {
                    rgba[i * 4 + 0] = s_srgb_to_linear[rgba[i * 4 + 0]];
                    rgba[i * 4 + 1] = s_srgb_to_linear[rgba[i * 4 + 1]];
                    rgba[i * 4 + 2] = s_srgb_to_linear[rgba[i * 4 + 2]];
                }
            }

            g_gl->glTexImage2D(upload_target, (GLint)level, GL_RGBA8, (GLsizei)info.width,
                               (GLsizei)info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }

    free(rgba);

    if (getenv("VITA2NATIVE_TRACE_TEXTURES") || gxm_time_ns() - started >= 50000000ull) {
        LOG("SceGxm: texture upload unit %u %ux%u fmt=0x%08x type=%d mips=%u data=%p "
            "hash=%08x took %.1f ms",
            unit, width, height, (uint32_t)gxm_texture_get_format(texture), (int)type, mip_count,
            data, hash, (double)(gxm_time_ns() - started) / 1e6);
    }

    gxm_texture_params(texture, target, mip_count);

    cache->data = data;
    cache->palette = palette;
    cache->width = width;
    cache->height = height;
    cache->format = (uint32_t)gxm_texture_get_format(texture);
    cache->mip_count = mip_count;
    cache->gamma_mode = texture->gamma_mode;
    cache->hash = hash;
    cache->hash_frame = g_gxm_frame;
    cache->last_used = ++g_texture_clock;
    cache->valid = true;

    return true;
}

static void gxm_upload_textures(SceGxmContext *context)
{
    for (unsigned int i = 0; i < SCE_GXM_MAX_TEXTURE_UNITS; i++) {
        gxm_upload_texture(&context->fragment_textures[i], i);
        gxm_upload_texture(&context->vertex_textures[i], SCE_GXM_MAX_TEXTURE_UNITS + i);
    }
}

static uint32_t gxm_align_up(uint32_t value, uint32_t alignment)
{
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

static void gxm_arena_init(GxmBufferArena *arena, GLenum target, uint32_t initial_capacity)
{
    if (arena->buffer)
        return;

    g_gl->glGenBuffers(1, &arena->buffer);
    arena->capacity = initial_capacity;
    arena->used = 0;
    arena->stage = malloc(initial_capacity);
    arena->stage_capacity = arena->stage ? initial_capacity : 0;
    g_gl->glBindBuffer(target, arena->buffer);
    g_gl->glBufferData(target, (GLsizeiptr)arena->capacity, NULL, GL_STREAM_DRAW);
}

static void gxm_arena_orphan(GxmBufferArena *arena, GLenum target)
{
    if (!arena->buffer)
        return;

    g_gl->glBindBuffer(target, arena->buffer);
    g_gl->glBufferData(target, (GLsizeiptr)arena->capacity, NULL, GL_STREAM_DRAW);
    arena->used = 0;
}

/*
 * Makes room for `size` more bytes. Growing discards the arena's contents, so
 * callers must flush the pending draw batch before calling this.
 */
static void gxm_arena_reserve(GxmBufferArena *arena, GLenum target, uint32_t size,
                              uint32_t alignment)
{
    uint32_t offset;

    if (!arena->buffer)
        gxm_arena_init(arena, target, 256 * 1024);

    offset = gxm_align_up(arena->used, alignment);
    if (offset + size <= arena->capacity && arena->stage)
        return;

    while (arena->capacity < offset + size)
        arena->capacity *= 2;

    g_gl->glBindBuffer(target, arena->buffer);
    g_gl->glBufferData(target, (GLsizeiptr)arena->capacity, NULL, GL_STREAM_DRAW);
    arena->used = 0;

    if (arena->stage_capacity < arena->capacity) {
        uint8_t *stage = realloc(arena->stage, arena->capacity);

        if (stage) {
            arena->stage = stage;
            arena->stage_capacity = arena->capacity;
        }
    }
}

static uint32_t gxm_arena_reserve_offset(GxmBufferArena *arena, uint32_t alignment)
{
    if (!arena->buffer)
        gxm_arena_init(arena, GL_ARRAY_BUFFER, 256 * 1024);
    arena->used = gxm_align_up(arena->used, alignment);
    return arena->used;
}

static bool gxm_arena_fits(const GxmBufferArena *arena, uint32_t size, uint32_t alignment)
{
    uint32_t offset = gxm_align_up(arena->used, alignment);

    return arena->buffer && offset + size <= arena->capacity;
}

/* Copies into the CPU mirror; the data reaches GL in gxm_arena_commit(). */
static uint32_t gxm_arena_append(GxmBufferArena *arena, const void *data, uint32_t size,
                                 uint32_t alignment)
{
    uint32_t offset = gxm_align_up(arena->used, alignment);

    if (data && size && arena->stage && offset + size <= arena->stage_capacity)
        memcpy(arena->stage + offset, data, size);
    arena->used = offset + size;

    return offset;
}

static void gxm_arena_commit(GxmBufferArena *arena, GLenum target, uint32_t start, uint32_t end)
{
    if (!arena->buffer || !arena->stage || end <= start || end > arena->stage_capacity)
        return;

    g_gl->glBindBuffer(target, arena->buffer);
    g_gl->glBufferSubData(target, (GLintptr)start, (GLsizeiptr)(end - start), arena->stage + start);
}

static uint32_t gxm_arena_upload(GxmBufferArena *arena, GLenum target, const void *data,
                                 uint32_t size, uint32_t alignment)
{
    uint32_t offset = gxm_arena_append(arena, data, size, alignment);

    gxm_arena_commit(arena, target, offset, arena->used);
    return offset;
}

static uint32_t gxm_build_uniforms(SceGxmContext *context, const SceGxmRegisteredProgram *program,
                                 bool vertex)
{
    const void *default_buffer = vertex ? context->vertex_uniforms : context->fragment_uniforms;
    const void *const *buffers =
        vertex ? context->vertex_uniform_buffers : context->fragment_uniform_buffers;
    uint8_t **data = vertex ? &context->vertex_block_data : &context->fragment_block_data;
    uint32_t *capacity = vertex ? &context->vertex_block_size : &context->fragment_block_size;
    uint32_t total = program ? program->uniform_buffer_size : 0;

    if (!total)
        return 0;

    if (*capacity != total) {
        free(*data);
        *data = malloc(total);
        *capacity = *data ? total : 0;
    }
    if (!*data)
        return 0;

    memset(*data, 0, total);

    for (uint32_t i = 0; i < program->uniform_buffer_count; i++) {
        const Vita3kUniformBufferLayout *layout = &program->uniform_buffers[i];
        const void *source = NULL;
        size_t size;

        if (gxm_program_trace() && i == 0) {
            LOG("GXM program %p: default_uniforms %u uniforms %u literals %u literal_bufs %u "
                "containers %u",
                (const void *)program->programHeader,
                program->programHeader->default_uniform_buffer_count,
                program->programHeader->uniform_buffer_count,
                program->programHeader->literals_count,
                program->programHeader->literal_buffer_count,
                program->programHeader->container_count);
        }

        if (layout->guest_index == 14)
            source = default_buffer;
        else if (layout->guest_index < SCE_GXM_MAX_UNIFORM_BUFFERS)
            source = buffers[layout->guest_index];

        if (gxm_program_trace()) {
            static unsigned int traced_layouts;

            if (traced_layouts < 6) {
                LOG("GXM uniforms %s program %p: total %u, buf %u guest %u offset %u size %u"
                    " member %u default %p guest %p",
                    vertex ? "vertex" : "fragment", (const void *)program->programHeader, total,
                    i, layout->guest_index, layout->offset, layout->guest_size,
                    layout->member_size, default_buffer, source);
                if (i + 1 == program->uniform_buffer_count) {
                    if (default_buffer && program->uniform_buffers[0].guest_size >= 36 * 4) {
                        const float *gf = (const float *)default_buffer;

                        for (unsigned int row = 0; row < 9; row++)
                            LOG("  guest u%d: %.4f %.4f %.4f %.4f", row, gf[row * 4 + 0],
                                gf[row * 4 + 1], gf[row * 4 + 2], gf[row * 4 + 3]);
                    }
                    traced_layouts++;
                }
            }
        }

        if (!source)
            continue;

        size = layout->guest_size < layout->member_size ? layout->guest_size
                                                        : layout->member_size;
        memcpy(*data + layout->offset, source, size);
    }

    return total;
}

/* Uploads the container most recently filled in by gxm_build_uniforms(). */
static void gxm_upload_uniforms(SceGxmContext *context, const SceGxmRegisteredProgram *program,
                                bool vertex)
{
    uint32_t total = program ? program->uniform_buffer_size : 0;
    unsigned int index = vertex ? 0 : 1;
    uint8_t *data = vertex ? context->vertex_block_data : context->fragment_block_data;
    GLuint buffer = context->uniform_block[index];

    if (!total || !data)
        return;

    if (!buffer) {
        g_gl->glGenBuffers(1, &buffer);
        context->uniform_block[index] = buffer;
    }

    g_gl->glBindBuffer(GL_UNIFORM_BUFFER, buffer);
    if (context->uniform_block_capacity[index] < total) {
        g_gl->glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)total, data, GL_STREAM_DRAW);
        context->uniform_block_capacity[index] = total;
    } else {
        g_gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)total, data);
    }

    if (g_bound_uniform_block[index] != buffer) {
        g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, index, buffer);
        g_bound_uniform_block[index] = buffer;
    }
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
    static int enabled = -1;
    GLenum error;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_GL_CHECK") != NULL;
    if (!enabled)
        return;

    error = g_gl->glGetError();

    if (error != GL_NO_ERROR)
        LOG("SceGxm: GL error 0x%x at %s", error, where);
}

typedef struct {
    uint32_t depth_bits; /* 0 = no depth, 16/24/32 = storage width */
    bool has_stencil;
    bool packed; /* depth and stencil share one 32-bit word (S8D24) */
} GxmDsLayout;

static GxmDsLayout gxm_ds_layout(SceGxmDepthStencilFormat format);
static int gxm_state_trace(void);

static void gxm_flush_state(SceGxmContext *context, const GxmLinkedProgram *linked,
                           const uint32_t *stream_starts)
{
    const SceGxmVertexProgram *vp = context->vertex_program;
    const SceGxmFragmentProgram *fp = context->fragment_program;
    GLuint program = linked->program;
    uint32_t surface_width = context->color_surface && context->color_surface->width
                                 ? context->color_surface->width
                                 : 960;
    uint32_t surface_height = context->color_surface && context->color_surface->height
                                  ? context->color_surface->height
                                  : 544;
    bool both_side_disabled;
    uint64_t prof = gxm_time_ns();

    g_gl->glUseProgram(program);

    /* Contexts are created on whichever thread runs the guest, which may not be
       the one that created the device; make sure the buffers exist regardless
       of whether GL was current when the context was instantiated. */
    if (g_gl->glGenBuffers) {
        if (!context->ubo[0] || !context->ubo[1])
            g_gl->glGenBuffers(2, context->ubo);
    }

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
        if (both_side_disabled)
            g_gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[0]);
        if (!context->vert_info_valid ||
            memcmp(&context->last_vert_info, &vert, sizeof(vert)) != 0) {
            if (context->ubo_capacity[0] < sizeof(vert)) {
                g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(vert), &vert, GL_STREAM_DRAW);
                context->ubo_capacity[0] = sizeof(vert);
            } else {
                g_gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(vert), &vert);
            }
            context->last_vert_info = vert;
            context->vert_info_valid = true;
        }
        if (g_bound_render_ubo[0] != context->ubo[0]) {
            g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 2, context->ubo[0]);
            g_bound_render_ubo[0] = context->ubo[0];
        }

        g_gl->glBindBuffer(GL_UNIFORM_BUFFER, context->ubo[1]);
        if (!context->frag_info_valid ||
            memcmp(&context->last_frag_info, &frag, sizeof(frag)) != 0) {
            if (context->ubo_capacity[1] < sizeof(frag)) {
                g_gl->glBufferData(GL_UNIFORM_BUFFER, sizeof(frag), &frag, GL_STREAM_DRAW);
                context->ubo_capacity[1] = sizeof(frag);
            } else {
                g_gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(frag), &frag);
            }
            context->last_frag_info = frag;
            context->frag_info_valid = true;
        }
        if (g_bound_render_ubo[1] != context->ubo[1]) {
            g_gl->glBindBufferBase(GL_UNIFORM_BUFFER, 3, context->ubo[1]);
            g_bound_render_ubo[1] = context->ubo[1];
        }
    }
    if (vp)
        gxm_upload_uniforms(context, vp->programId, true);
    if (fp)
        gxm_upload_uniforms(context, fp->programId, false);

    g_profile_uniform_ns += gxm_time_ns() - prof;
    prof = gxm_time_ns();

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

    g_profile_state_ns += gxm_time_ns() - prof;
    prof = gxm_time_ns();

    gxm_upload_textures(context);

    g_profile_texture_ns += gxm_time_ns() - prof;
    prof = gxm_time_ns();

    /* Vertex attributes read from the per-stream arenas; stream_starts are the
       byte offsets that batch vertex 0 was written to. */
    if (vp && stream_starts) {
        for (unsigned int i = 0; i < vp->attributeCount; i++) {
            const SceGxmVertexAttribute *a = &vp->attributes[i];
            const SceGxmVertexStream *s;
            int location;
            GxmStream *stream;
            GLenum type;

            if (a->streamIndex >= vp->streamCount ||
                a->streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
                continue;

            location = vita3k_gxm_get_attribute_location(vp->programId->programHeader, a->regIndex);
            s = &vp->streams[a->streamIndex];
            stream = &context->streams[a->streamIndex];
            if (location < 0)
                continue;

            if (!stream->data || !s->stride)
                continue;

            type = gxm_attribute_type((SceGxmAttributeFormat)a->format);
            g_gl->glEnableVertexAttribArray((GLuint)location);
            g_gl->glBindBuffer(GL_ARRAY_BUFFER, context->stream_arena[a->streamIndex].buffer);
            g_gl->glVertexAttribPointer(
                (GLuint)location, a->componentCount, type,
                gxm_attribute_normalized((SceGxmAttributeFormat)a->format), s->stride,
                (const void *)(uintptr_t)(stream_starts[a->streamIndex] + a->offset));
        }
    }

    g_profile_attrib_ns += gxm_time_ns() - prof;
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
    g_linked_next = 0;
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
        for (unsigned int i = 0; i < g_cached_shader_count; i++)
            g_gl->glDeleteShader(g_cached_shaders[i].shader);
        gxm_texture_cache_flush();
        memset(g_bound_uniform_block, 0, sizeof(g_bound_uniform_block));
        memset(g_bound_render_ubo, 0, sizeof(g_bound_render_ubo));
    }
    g_linked_count = 0;
    g_linked_next = 0;
    g_cached_shader_count = 0;
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

    /* Pending draws would otherwise never signal the notification. */
    if (g_current_context)
        gxm_batch_flush(g_current_context);

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

    /* GL objects are created here, and the guest thread is not the thread that
       created the device. */
    host_gpu_thread_enter(g_device);

    if (!params || !context)
        return SCE_GXM_ERROR_INVALID_POINTER;

    if (params->hostMem && params->hostMemSize >= sizeof(SceGxmContext)) {
        ctx = params->hostMem;
        memset(ctx, 0, sizeof(*ctx));
    } else {
        /*
         * Applications size hostMem from the SDK's SceGxmContext, which is much
         * smaller than this implementation's. Use host memory when the guest
         * buffer fits, otherwise allocate the translation-layer state.
         */
        LOG("SceGxm: context hostMem too small (0x%x < 0x%zx), allocating",
            params->hostMemSize, sizeof(SceGxmContext));
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx)
            return SCE_GXM_ERROR_OUT_OF_MEMORY;
        ctx->owns_memory = true;
    }

    ctx->params = *params;
    gxm_context_init_defaults(ctx, false);

    if (!ctx->vertex_uniform_arena || !ctx->fragment_uniform_arena)
        goto oom;

    *context = ctx;
    return 0;

oom:
    free(ctx->vertex_uniform_arena);
    free(ctx->fragment_uniform_arena);
    if (ctx->owns_memory)
        free(ctx);
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
}

EXPORT(SceGxm, 0xEDDC5FB2, int, sceGxmDestroyContext, SceGxmContext *context)
{
    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;

    if (g_gl && !context->deferred) {
        gxm_batch_flush(context);
        if (context->ubo[0] || context->ubo[1])
            g_gl->glDeleteBuffers(2, context->ubo);
        for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
            if (context->stream_arena[i].buffer)
                g_gl->glDeleteBuffers(1, &context->stream_arena[i].buffer);
            free(context->stream_arena[i].stage);
        }
        if (context->index_arena.buffer)
            g_gl->glDeleteBuffers(1, &context->index_arena.buffer);
        free(context->index_arena.stage);
        if (context->uniform_block[0] || context->uniform_block[1])
            g_gl->glDeleteBuffers(2, context->uniform_block);
    }

    free(context->batch.vertex_uniforms);
    free(context->batch.fragment_uniforms);
    free(context->index_scratch);
    free(context->vertex_block_data);
    free(context->fragment_block_data);
    free(context->vertex_uniform_arena);
    free(context->fragment_uniform_arena);
    gxm_command_free(context->cmd_head);
    context->cmd_head = NULL;
    context->cmd_tail = NULL;
    if (g_current_context == context)
        g_current_context = NULL;
    if (context->owns_memory)
        free(context);
    return 0;
}

EXPORT(SceGxm, 0x0733D8AE, void, sceGxmFinish, SceGxmContext *context)
{
    if (context)
        gxm_batch_flush(context);
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

    host_gpu_thread_enter(g_device);

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
                                          attributes, attributeCount, &vp->shader_hash);
    if (g_gl && !vp->shader) {
        free(vp->attributes);
        free(vp->streams);
        free(vp);
        return SCE_GXM_ERROR_INVALID_VALUE;
    }

    *vertexProgram = vp;

    if (gxm_program_trace())
        LOG("SceGxm: create vp %p shader %u program %p", (void *)vp, vp->shader,
            (const void *)programId->programHeader);

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
    /* The GL shader is owned by the content-addressed shader cache. */
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

    host_gpu_thread_enter(g_device);

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
                                          NULL, 0, &fp->shader_hash);
    if (g_gl && !fp->shader) {
        free(fp);
        return SCE_GXM_ERROR_INVALID_VALUE;
    }

    *fragmentProgram = fp;

    if (gxm_program_trace())
        LOG("SceGxm: create fp %p shader %u program %p", (void *)fp, fp->shader,
            (const void *)programId->programHeader);

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

    if (gxm_program_trace())
        LOG("SceGxm: release fp %p shader %u", (void *)fragmentProgram, fragmentProgram->shader);

    /* The GL shader is owned by the content-addressed shader cache. */
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

    host_gpu_thread_enter(g_device);

    if (!g_gxm_initialized)
        return SCE_GXM_ERROR_UNINITIALIZED;
    if (!params)
        return SCE_GXM_ERROR_INVALID_POINTER;

    rt = calloc(1, sizeof(*rt));
    if (!rt)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    rt->params = *params;
    LOG("SceGxm: render target %ux%u multisampleMode=%u", params->width, params->height,
        params->multisampleMode);
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

        /* Optional multisampled draw target, resolved into the FBO above. */
        rt->samples = params->multisampleMode == SCE_GXM_MULTISAMPLE_4X   ? 4
                      : params->multisampleMode == SCE_GXM_MULTISAMPLE_2X ? 2
                                                                          : 0;
        if (rt->samples && g_gl->glRenderbufferStorageMultisample) {
            rt->samples = rt->samples > 4 ? 4 : rt->samples;
            g_gl->glGenRenderbuffers(1, &rt->msaa_color);
            g_gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->msaa_color);
            g_gl->glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)rt->samples, GL_RGBA8,
                                                   params->width, params->height);
            g_gl->glGenRenderbuffers(1, &rt->msaa_depth);
            g_gl->glBindRenderbuffer(GL_RENDERBUFFER, rt->msaa_depth);
            g_gl->glRenderbufferStorageMultisample(GL_RENDERBUFFER, (GLsizei)rt->samples,
                                                   GL_DEPTH24_STENCIL8, params->width,
                                                   params->height);
            g_gl->glGenFramebuffers(1, &rt->msaa_fbo);
            g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->msaa_fbo);
            g_gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                            rt->msaa_color);
            g_gl->glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                            GL_RENDERBUFFER, rt->msaa_depth);
            if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                LOG("SceGxm: multisampled framebuffer incomplete, falling back");
                g_gl->glDeleteFramebuffers(1, &rt->msaa_fbo);
                g_gl->glDeleteRenderbuffers(1, &rt->msaa_color);
                g_gl->glDeleteRenderbuffers(1, &rt->msaa_depth);
                rt->msaa_fbo = 0;
                rt->msaa_color = 0;
                rt->msaa_depth = 0;
                /*
                 * Keep rt->samples as the requested count: the surface scale
                 * mode is a guest-visible property, and a driver without MSAA
                 * still needs the supersampled/downscaled writeback shape.
                 */
            }
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
        if (renderTarget->msaa_fbo)
            g_gl->glDeleteFramebuffers(1, &renderTarget->msaa_fbo);
        if (renderTarget->msaa_color)
            g_gl->glDeleteRenderbuffers(1, &renderTarget->msaa_color);
        if (renderTarget->msaa_depth)
            g_gl->glDeleteRenderbuffers(1, &renderTarget->msaa_depth);
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

static float gxm_ds_get_background_depth_softfp(const SceGxmDepthStencilSurface *surface);
static float gxm_ds_get_background_depth_softfp(const SceGxmDepthStencilSurface *surface)
{
    return surface->backgroundDepth;
}
EXPORT_FUNC(SceGxm, 0x269B56BE, gxm_ds_get_background_depth_softfp);

static void gxm_ds_set_background_depth_softfp(SceGxmDepthStencilSurface *surface, float depth);
static void gxm_ds_set_background_depth_softfp(SceGxmDepthStencilSurface *surface, float depth)
{
    surface->backgroundDepth = depth;
}
EXPORT_FUNC(SceGxm, 0x32F280F0, gxm_ds_set_background_depth_softfp);

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

static void gxm_set_viewport(SceGxmContext *context, float x_offset, float x_scale,
                             float y_offset, float y_scale, float z_offset,
                             float z_scale);
static void gxm_set_viewport(SceGxmContext *context, float x_offset, float x_scale,
                             float y_offset, float y_scale, float z_offset, float z_scale)
{
    context->viewport_x_offset = x_offset;
    context->viewport_x_scale = x_scale;
    context->viewport_y_offset = y_offset;
    context->viewport_y_scale = y_scale;
    context->viewport_z_offset = z_offset;
    context->viewport_z_scale = z_scale;
    context->viewport_enable = SCE_GXM_VIEWPORT_ENABLED;
}
EXPORT_FUNC(SceGxm, 0x3EB3380B, gxm_set_viewport);

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

static void gxm_set_w_clamp_value_softfp(SceGxmContext *context, float value);
static void gxm_set_w_clamp_value_softfp(SceGxmContext *context, float value)
{
    context->w_clamp_value = value;
}
EXPORT_FUNC(SceGxm, 0xD096336E, gxm_set_w_clamp_value_softfp);

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
    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    gxm_batch_flush(context);
    if (g_gl)
        g_gl->glFinish();

    /* Unity waits on this fence from its GfxDevice worker after streaming
       draws, so it has to be published once the batch has really executed. */
    if (vertexNotification && vertexNotification->address) {
        __sync_synchronize();
        *vertexNotification->address = vertexNotification->value;
        __sync_synchronize();
    }

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

    if (gxm_program_trace()) {
        static unsigned int traced;

        if (traced < 40) {
            const float *f = (const float *)bufferData;

            LOG("GXM set vubuf %u -> %p: %g %g %g %g", bufferIndex, bufferData, f[0], f[1],
                f[2], f[3]);
            traced++;
        }
    }

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
 * Element offset in a depth/stencil surface. Tiled surfaces use the same
 * 32x32 tile order as tiled colour surfaces and textures. The guest never
 * inspects this layout directly; load and store only have to agree with each
 * other, which they do as long as both use this helper.
 */
static size_t gxm_ds_element_offset(const SceGxmDepthStencilSurface *surface, uint32_t x,
                                    uint32_t y, uint32_t stride, uint32_t element_bytes)
{
    uint32_t type = (surface->zlsControl >> SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_FORMAT_OFFSET) &
                    SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_MASK;

    if (type == ((uint32_t)SCE_GXM_DEPTH_STENCIL_SURFACE_TILED >> 12)) {
        uint32_t tiles_per_row = (stride + 31) >> 5;
        uint32_t tile = (x >> 5) + tiles_per_row * (y >> 5);
        uint32_t index = (tile << 10) | (x & 31) | ((y & 31) << 5);

        return (size_t)index * element_bytes;
    }

    return ((size_t)y * stride + x) * element_bytes;
}

/*
 * Uploads the guest depth/stencil surface into the render target's GL depth
 * texture. Only called when the surface asks for a force load.
 */
static bool gxm_ds_load_surface(SceGxmRenderTarget *rt,
                                const SceGxmDepthStencilSurface *surface)
{
    GxmDsLayout layout;
    SceGxmDepthStencilFormat format;
    uint32_t stride, width, height, bg_stencil;
    uint32_t ds_type;
    bool tiled;
    const uint8_t *depth_data;
    const uint8_t *stencil_data;
    uint32_t *packed;
    bool loaded = false;
    uint64_t ds_started;

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

    ds_started = gxm_time_ns();

    width = (uint32_t)rt->params.width;
    height = (uint32_t)rt->params.height;
    stride = gxm_ds_surface_get_stride_in_samples(surface);
    if (stride < width)
        stride = width;
    ds_type = (surface->zlsControl >> SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_FORMAT_OFFSET) &
              SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_TYPE_MASK;
    tiled = ds_type == ((uint32_t)SCE_GXM_DEPTH_STENCIL_SURFACE_TILED >> 12);
    bg_stencil = surface->backgroundControl & SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK;
    depth_data = layout.packed ? (const uint8_t *)(surface->depthData ? surface->depthData
                                                                     : surface->stencilData)
                               : (const uint8_t *)surface->depthData;
    stencil_data = (const uint8_t *)surface->stencilData;

    /* Reused across scenes: the buffer is a couple of MB and BeginScene can
       run many times per frame. */
    {
        static uint32_t *packed_buffer;
        static size_t packed_capacity;
        size_t needed = (size_t)width * height * sizeof(uint32_t);

        if (packed_capacity < needed) {
            uint32_t *grown = realloc(packed_buffer, needed);

            if (!grown)
                return false;
            packed_buffer = grown;
            packed_capacity = needed;
        }
        packed = packed_buffer;
    }

    /* A linear, packed S8D24 surface converts to the GL depth-stencil layout
       word for word, so it is a strided copy. Unity keeps depth surfaces in
       this layout and force-loads them whenever a pass samples depth. */
    if (layout.packed && !tiled) {
        size_t row_bytes = (size_t)width * sizeof(uint32_t);

        for (uint32_t y = 0; y < height; y++)
            memcpy(packed + (size_t)y * width,
                   depth_data + (size_t)y * stride * sizeof(uint32_t), row_bytes);
    } else {
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t d24 = 0xFFFFFFu;
                uint32_t s8 = bg_stencil;

                if (layout.packed) {
                    uint32_t value = *(const uint32_t *)(depth_data +
                                                         gxm_ds_element_offset(surface, x, y,
                                                                               stride, 4));
                    d24 = value >> 8;
                    s8 = value & 0xFFu;
                } else {
                    if (layout.depth_bits == 32 && depth_data) {
                        float value = *(const float *)(depth_data +
                                                       gxm_ds_element_offset(surface, x, y,
                                                                             stride, 4));
                        d24 = gxm_depth_to_u24(value);
                    } else if (layout.depth_bits == 16 && depth_data) {
                        uint32_t value = *(const uint16_t *)(depth_data +
                                                             gxm_ds_element_offset(surface, x, y,
                                                                                   stride, 2));
                        d24 = (uint32_t)(((uint64_t)value * 0xFFFFFFu + 0x7FFFu) / 0xFFFFu);
                    }
                    if (layout.has_stencil && stencil_data)
                        s8 = stencil_data[gxm_ds_element_offset(surface, x, y, stride, 1)];
                }

                packed[(size_t)y * width + x] = (d24 << 8) | s8;
            }
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

    g_profile_ds_load_ns += gxm_time_ns() - ds_started;
    g_profile_ds_load_calls++;

    return loaded;
    return loaded;
}

/*
 * Reads the render target's depth/stencil back into the guest surface when the
 * surface asks for a force store. The GL origin is bottom-left, like the color
 * readback in sceGxmEndScene, so rows are flipped.
 */
typedef struct {
    GLuint fbo;
    GLuint stencil_fbo;
    GLuint color;
    GLuint program;
    GLuint stencil_program;
    GLint depth_uniform;
    uint32_t width;
    uint32_t height;
    int stencil_fetch; /* -1 unknown, 0 unavailable, 1 available */
} GxmDepthExport;

static GxmDepthExport g_depth_export = { .stencil_fetch = -1 };

/*
 * Some vendor GLES blobs (ARM Bifrost here) reject glReadPixels for depth and
 * stencil formats with GL_INVALID_ENUM. When the render target's depth is a
 * texture, render it into an RGBA8 target with a packing shader and read that
 * back instead. 8-bit-per-channel packing keeps about 32 bits of precision.
 */
static bool gxm_depth_export_ensure(uint32_t width, uint32_t height)
{
    static const char *const vert_src =
        "#version 300 es\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  v_uv = p;\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *const frag_src =
        "#version 300 es\n"
        "precision highp float;\n"
        "uniform sampler2D u_depth;\n"
        "in vec2 v_uv;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "  float d = texture(u_depth, v_uv).r;\n"
        "  vec4 enc = vec4(1.0, 255.0, 65025.0, 16581375.0) * d;\n"
        "  enc = fract(enc);\n"
        "  enc -= enc.yzww * vec4(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0, 0.0);\n"
        "  o_color = enc;\n"
        "}\n";
    GLint ok = 0;
    char log[512];
    GLuint vs, fs;

    if (g_depth_export.program && g_depth_export.width == width &&
        g_depth_export.height == height)
        return true;

    if (g_depth_export.program) {
        g_gl->glDeleteProgram(g_depth_export.program);
        g_gl->glDeleteTextures(1, &g_depth_export.color);
        g_gl->glDeleteFramebuffers(1, &g_depth_export.fbo);
        memset(&g_depth_export, 0, sizeof(g_depth_export));
    }

    vs = g_gl->glCreateShader(GL_VERTEX_SHADER);
    fs = g_gl->glCreateShader(GL_FRAGMENT_SHADER);
    g_gl->glShaderSource(vs, 1, &vert_src, NULL);
    g_gl->glCompileShader(vs);
    g_gl->glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g_gl->glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        LOG("SceGxm: depth export vertex shader failed: %s", log);
        g_gl->glDeleteShader(vs);
        g_gl->glDeleteShader(fs);
        return false;
    }
    g_gl->glShaderSource(fs, 1, &frag_src, NULL);
    g_gl->glCompileShader(fs);
    g_gl->glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g_gl->glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        LOG("SceGxm: depth export fragment shader failed: %s", log);
        g_gl->glDeleteShader(vs);
        g_gl->glDeleteShader(fs);
        return false;
    }

    g_depth_export.program = g_gl->glCreateProgram();
    g_gl->glAttachShader(g_depth_export.program, vs);
    g_gl->glAttachShader(g_depth_export.program, fs);
    g_gl->glLinkProgram(g_depth_export.program);
    g_gl->glDeleteShader(vs);
    g_gl->glDeleteShader(fs);
    g_gl->glGetProgramiv(g_depth_export.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_gl->glGetProgramInfoLog(g_depth_export.program, sizeof(log), NULL, log);
        LOG("SceGxm: depth export link failed: %s", log);
        g_gl->glDeleteProgram(g_depth_export.program);
        memset(&g_depth_export, 0, sizeof(g_depth_export));
        return false;
    }
    g_depth_export.depth_uniform = g_gl->glGetUniformLocation(g_depth_export.program, "u_depth");

    g_gl->glGenTextures(1, &g_depth_export.color);
    g_gl->glBindTexture(GL_TEXTURE_2D, g_depth_export.color);
    g_gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA,
                       GL_UNSIGNED_BYTE, NULL);
    g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g_gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_gl->glGenFramebuffers(1, &g_depth_export.fbo);
    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, g_depth_export.fbo);
    g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                 g_depth_export.color, 0);
    if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG("SceGxm: depth export framebuffer incomplete");
        g_gl->glDeleteFramebuffers(1, &g_depth_export.fbo);
        g_gl->glDeleteTextures(1, &g_depth_export.color);
        g_gl->glDeleteProgram(g_depth_export.program);
        memset(&g_depth_export, 0, sizeof(g_depth_export));
        return false;
    }

    g_depth_export.width = width;
    g_depth_export.height = height;
    return true;
}

static bool gxm_depth_export_read(GLuint depth_texture, uint32_t width, uint32_t height,
                                  float *out)
{
    uint8_t *bytes;
    bool ok;

    if (!depth_texture || !out || !gxm_depth_export_ensure(width, height))
        return false;

    bytes = malloc((size_t)width * height * 4);
    if (!bytes)
        return false;

    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, g_depth_export.fbo);
    g_gl->glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    g_gl->glDisable(GL_DEPTH_TEST);
    g_gl->glDisable(GL_STENCIL_TEST);
    g_gl->glDisable(GL_BLEND);
    g_gl->glDisable(GL_SCISSOR_TEST);
    g_gl->glDisable(GL_CULL_FACE);
    g_gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl->glUseProgram(g_depth_export.program);
    g_gl->glActiveTexture(GL_TEXTURE0 + 7);
    g_gl->glBindTexture(GL_TEXTURE_2D, depth_texture);
    g_gl->glUniform1i(g_depth_export.depth_uniform, 7);
    g_gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
    ok = g_gl->glGetError() == GL_NO_ERROR;
    g_gl->glUseProgram(0);

    if (ok) {
        for (size_t i = 0; i < (size_t)width * height; i++) {
            const uint8_t *p = bytes + i * 4;

            out[i] = (float)p[0] / 255.0f +
                     (float)p[1] / (255.0f * 255.0f) +
                     (float)p[2] / (255.0f * 65025.0f) +
                     (float)p[3] / (255.0f * 16581375.0f);
        }
    }

    free(bytes);
    return ok;
}

static bool gxm_stencil_export_ensure(uint32_t width, uint32_t height)
{
    static const char *const vert_src =
        "#version 300 es\n"
        "void main() {\n"
        "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char *const frag_src =
        "#version 300 es\n"
        "#extension GL_ARM_shader_framebuffer_fetch_depth_stencil : require\n"
        "precision highp float;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "  o_color = vec4(float(gl_LastFragStencilARM) / 255.0, 0.0, 0.0, 1.0);\n"
        "}\n";
    GLint ok = 0;
    char log[512];
    GLuint vs, fs;

    if (g_depth_export.stencil_program && g_depth_export.width == width &&
        g_depth_export.height == height)
        return true;
    if (g_depth_export.stencil_fetch < 0) {
        const char *extensions = (const char *)g_gl->glGetString(GL_EXTENSIONS);

        g_depth_export.stencil_fetch =
            extensions && strstr(extensions, "GL_ARM_shader_framebuffer_fetch_depth_stencil")
                ? 1
                : 0;
        if (!g_depth_export.stencil_fetch)
            LOG("SceGxm: stencil export unavailable (no framebuffer fetch extension)");
    }
    if (!g_depth_export.stencil_fetch)
        return false;

    vs = g_gl->glCreateShader(GL_VERTEX_SHADER);
    fs = g_gl->glCreateShader(GL_FRAGMENT_SHADER);
    g_gl->glShaderSource(vs, 1, &vert_src, NULL);
    g_gl->glCompileShader(vs);
    g_gl->glShaderSource(fs, 1, &frag_src, NULL);
    g_gl->glCompileShader(fs);
    g_gl->glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        g_gl->glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        LOG("SceGxm: stencil export shader failed: %s", log);
        g_gl->glDeleteShader(vs);
        g_gl->glDeleteShader(fs);
        g_depth_export.stencil_fetch = 0;
        return false;
    }

    g_depth_export.stencil_program = g_gl->glCreateProgram();
    g_gl->glAttachShader(g_depth_export.stencil_program, vs);
    g_gl->glAttachShader(g_depth_export.stencil_program, fs);
    g_gl->glLinkProgram(g_depth_export.stencil_program);
    g_gl->glDeleteShader(vs);
    g_gl->glDeleteShader(fs);
    g_gl->glGetProgramiv(g_depth_export.stencil_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        g_gl->glGetProgramInfoLog(g_depth_export.stencil_program, sizeof(log), NULL, log);
        LOG("SceGxm: stencil export link failed: %s", log);
        g_gl->glDeleteProgram(g_depth_export.stencil_program);
        g_depth_export.stencil_program = 0;
        g_depth_export.stencil_fetch = 0;
        return false;
    }

    if (!g_depth_export.stencil_fbo)
        g_gl->glGenFramebuffers(1, &g_depth_export.stencil_fbo);
    return true;
}

static bool gxm_stencil_export_read(GLuint depth_texture, uint32_t width, uint32_t height,
                                    uint8_t *out)
{
    uint8_t *bytes;
    bool ok;

    if (!depth_texture || !out || !gxm_stencil_export_ensure(width, height))
        return false;

    bytes = malloc((size_t)width * height * 4);
    if (!bytes)
        return false;

    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, g_depth_export.stencil_fbo);
    g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                 g_depth_export.color, 0);
    g_gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                                 depth_texture, 0);
    if (g_gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        free(bytes);
        return false;
    }

    g_gl->glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    g_gl->glDisable(GL_DEPTH_TEST);
    g_gl->glDisable(GL_STENCIL_TEST);
    g_gl->glDisable(GL_BLEND);
    g_gl->glDisable(GL_SCISSOR_TEST);
    g_gl->glDisable(GL_CULL_FACE);
    g_gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g_gl->glUseProgram(g_depth_export.stencil_program);
    g_gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
    ok = g_gl->glGetError() == GL_NO_ERROR;
    g_gl->glUseProgram(0);

    if (ok) {
        for (size_t i = 0; i < (size_t)width * height; i++)
            out[i] = bytes[i * 4];
    }

    free(bytes);
    return ok;
}

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
    uint8_t *stencil_read = NULL;
    bool have_stencil = false;
    bool have_depth = false;
    bool debug = getenv("VITA2NATIVE_DS_DEBUG") != NULL;

    if (debug)
        LOG("SceGxm: depth store entry: surface=%p depthData=%p zls=0x%x rt_depth=%d/%p",
            (void *)surface, surface ? surface->depthData : NULL,
            surface ? surface->zlsControl : 0, rt->depth_is_texture, (void *)rt->depth_rb);

    if (!surface || gxm_ds_surface_is_disabled(surface)) {
        if (debug)
            LOG("SceGxm: depth store skipped: no surface");
        return;
    }
    if (!(surface->zlsControl & SCE_GXM_DEPTH_STENCIL_ZLS_CTRL_FORCE_STORE_BIT)) {
        if (debug)
            LOG("SceGxm: depth store skipped: force store off (zls=0x%x)", surface->zlsControl);
        return;
    }
    if (!surface->depthData && !surface->stencilData) {
        if (debug)
            LOG("SceGxm: depth store skipped: no guest data");
        return;
    }
    if (!rt->depth_is_texture && !rt->depth_rb) {
        if (debug)
            LOG("SceGxm: depth store skipped: render target has no depth attachment");
        return;
    }

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
     * Read the depth/stencil attachment. GLES 3.0 offers packed
     * DEPTH_STENCIL/UNSIGNED_INT_24_8 and DEPTH_COMPONENT/UNSIGNED_INT; some
     * drivers support only one of them, and vendor blobs (ARM Bifrost) reject
     * both. The last resort samples the depth texture with a packing shader.
     */
    {
        bool packed_valid = false;
        GLenum packed_error;

        g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_STENCIL,
                           GL_UNSIGNED_INT_24_8, packed_values);
        packed_error = g_gl->glGetError();
        if (packed_error == GL_NO_ERROR) {
            packed_valid = true;
            have_depth = true;
            have_stencil = true;
        }

        if (!have_depth) {
            uint32_t *depth_bits = malloc((size_t)width * height * sizeof(uint32_t));
            GLenum error = GL_NO_ERROR;

            if (depth_bits) {
                g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT,
                                   GL_UNSIGNED_INT, depth_bits);
                error = g_gl->glGetError();
                if (error == GL_NO_ERROR) {
                    depth_values = malloc((size_t)width * height * sizeof(float));
                    if (depth_values) {
                        for (size_t i = 0; i < (size_t)width * height; i++)
                            depth_values[i] = (float)((double)depth_bits[i] / 4294967295.0);
                        have_depth = true;
                    }
                }
                free(depth_bits);
            }
            if (debug)
                LOG("SceGxm: depth readback unsigned int: error=0x%x", error);
        }

        if (!have_depth) {
            GLenum error = GL_NO_ERROR;

            depth_values = malloc((size_t)width * height * sizeof(float));
            if (depth_values) {
                g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_DEPTH_COMPONENT,
                                   GL_FLOAT, depth_values);
                error = g_gl->glGetError();
                have_depth = error == GL_NO_ERROR;
            }
            if (debug)
                LOG("SceGxm: depth readback float: error=0x%x", error);
        }

        if (!have_depth && rt->depth_is_texture && rt->depth && depth_values) {
            have_depth = gxm_depth_export_read(rt->depth, width, height, depth_values);
            g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
            if (debug)
                LOG("SceGxm: depth export fallback %s", have_depth ? "ok" : "failed");
        }

        if (!have_stencil) {
            stencil_read = malloc((size_t)width * height);
            if (stencil_read) {
                g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_STENCIL_INDEX,
                                   GL_UNSIGNED_BYTE, stencil_read);
                if (g_gl->glGetError() != GL_NO_ERROR) {
                    free(stencil_read);
                    stencil_read = NULL;
                } else {
                    have_stencil = true;
                }
            }
        }

        if (!have_stencil && rt->depth_is_texture && rt->depth) {
            stencil_read = malloc((size_t)width * height);
            if (stencil_read) {
                have_stencil = gxm_stencil_export_read(rt->depth, width, height, stencil_read);
                g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
                if (!have_stencil) {
                    free(stencil_read);
                    stencil_read = NULL;
                }
                if (debug)
                    LOG("SceGxm: stencil export fallback %s", have_stencil ? "ok" : "failed");
            }
        }

        if (debug)
            LOG("SceGxm: depth store %ux%u packed=%d depth=%d stencil=%d", width, height,
                packed_valid, have_depth, have_stencil);

        if (!have_depth && !have_stencil) {
            free(packed_values);
            free(depth_values);
            free(stencil_read);
            return;
        }

        for (uint32_t y = 0; y < height; y++) {
            uint32_t src_y = height - 1 - y;

            for (uint32_t x = 0; x < width; x++) {
                size_t src = (size_t)src_y * width + x;
                size_t depth_offset = gxm_ds_element_offset(surface, x, y, stride, 4);
                size_t depth16_offset = gxm_ds_element_offset(surface, x, y, stride, 2);
                size_t stencil_offset = gxm_ds_element_offset(surface, x, y, stride, 1);
                float depth = 1.0f;
                uint32_t d24;
                uint8_t s8 = (uint8_t)(surface->backgroundControl &
                                       SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK);

                if (packed_valid) {
                    d24 = packed_values[src] >> 8;
                    s8 = (uint8_t)(packed_values[src] & 0xFFu);
                    depth = (float)d24 / 16777215.0f;
                } else {
                    if (depth_values)
                        depth = depth_values[src];
                    d24 = gxm_depth_to_u24(depth);
                    if (stencil_read)
                        s8 = stencil_read[src];
                }

                if (layout.packed) {
                    *(uint32_t *)(depth_data + depth_offset) = (d24 << 8) | s8;
                } else {
                    if (depth_data && have_depth) {
                        if (layout.depth_bits == 32)
                            *(float *)(depth_data + depth_offset) = depth;
                        else if (layout.depth_bits == 16)
                            *(uint16_t *)(depth_data + depth16_offset) =
                                (uint16_t)(depth * 65535.0f + 0.5f);
                    }
                    if (stencil_data && have_stencil && layout.has_stencil)
                        stencil_data[stencil_offset] = s8;
                }
            }
        }
    }

    free(packed_values);
    free(depth_values);
    free(stencil_read);
}

static void gxm_resolve_render_target(SceGxmRenderTarget *rt)
{
    if (!rt->samples || !rt->msaa_fbo)
        return;

    g_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->msaa_fbo);
    g_gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->fbo);
    g_gl->glBlitFramebuffer(0, 0, (GLint)rt->params.width, (GLint)rt->params.height, 0, 0,
                            (GLint)rt->params.width, (GLint)rt->params.height,
                            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
}

static void gxm_blit_depth_stencil_to_msaa(SceGxmRenderTarget *rt)
{
    if (!rt->samples || !rt->msaa_fbo)
        return;

    g_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    g_gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->msaa_fbo);
    g_gl->glBlitFramebuffer(0, 0, (GLint)rt->params.width, (GLint)rt->params.height, 0, 0,
                            (GLint)rt->params.width, (GLint)rt->params.height,
                            GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, rt->msaa_fbo);
}

EXPORT(SceGxm, 0x8734FF4E, int, sceGxmBeginScene, SceGxmContext *context, unsigned int flags,
       const SceGxmRenderTarget *renderTarget, const SceGxmValidRegion *validRegion,
       SceGxmSyncObject *vertexSyncObject, SceGxmSyncObject *fragmentSyncObject,
       const SceGxmColorSurface *colorSurface, const SceGxmDepthStencilSurface *depthStencil)
{
    (void)flags;
    (void)validRegion;
    (void)vertexSyncObject;

    host_gpu_thread_enter(g_device);

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
    g_current_context = context;

    if (g_gl) {
        GLuint draw_fbo = renderTarget->msaa_fbo ? renderTarget->msaa_fbo : renderTarget->fbo;

        /* The GPU is idle here (the previous frame ended with a finish), so
           orphaning the upload arenas once per frame is cheap. */
        for (unsigned int i = 0; i < SCE_GXM_MAX_VERTEX_STREAMS; i++)
            gxm_arena_orphan(&context->stream_arena[i], GL_ARRAY_BUFFER);
        gxm_arena_orphan(&context->index_arena, GL_ELEMENT_ARRAY_BUFFER);
        context->batch.active = false;

        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
        g_gl->glViewport(0, 0, renderTarget->params.width, renderTarget->params.height);
        g_gl->glDisable(GL_SCISSOR_TEST);

        if (gxm_ds_load_surface((SceGxmRenderTarget *)renderTarget, context->ds_surface)) {
            gxm_blit_depth_stencil_to_msaa((SceGxmRenderTarget *)renderTarget);
        } else {
            float bg_depth = 1.0f;
            unsigned int bg_stencil = 0;

            if (context->ds_surface) {
                bg_depth = context->ds_surface->backgroundDepth;
                bg_stencil = context->ds_surface->backgroundControl &
                             SCE_GXM_DEPTH_STENCIL_BG_CTRL_STENCIL_MASK;
            }

            if (gxm_state_trace()) {
                static unsigned int traced_scenes;

                if (traced_scenes < 8) {
                    LOG("SceGxm: scene ds=%p zls=0x%x depthData=%p clear=%.3f stencil=%u",
                        (const void *)context->ds_surface,
                        context->ds_surface ? context->ds_surface->zlsControl : 0,
                        context->ds_surface ? context->ds_surface->depthData : NULL, bg_depth,
                        bg_stencil);
                    traced_scenes++;
                }
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

static uint32_t gxm_spread_bits(uint32_t value)
{
    value &= 0x0000ffffu;
    value = (value | (value << 8)) & 0x00ff00ffu;
    value = (value | (value << 4)) & 0x0f0f0f0fu;
    value = (value | (value << 2)) & 0x33333333u;
    value = (value | (value << 1)) & 0x55555555u;
    return value;
}

static uint32_t gxm_morton2(uint32_t x, uint32_t y)
{
    return gxm_spread_bits(x) | (gxm_spread_bits(y) << 1);
}

/*
 * Byte offset of a colour surface pixel. Tiled surfaces use the same 32x32
 * row-major tile layout as SCE_GXM_TEXTURE_TILED, and square power-of-two
 * swizzled surfaces use the Morton order of SCE_GXM_TEXTURE_SWIZZLED, so a
 * surface written here can be sampled back through the matching texture.
 */
static size_t gxm_color_pixel_offset(const SceGxmColorSurfaceInner *surface, uint32_t x,
                                     uint32_t y, uint32_t bytes_per_pixel)
{
    uint32_t stride = surface->strideInPixels ? surface->strideInPixels : surface->width;

    if (surface->surfaceType == SCE_GXM_COLOR_SURFACE_TILED) {
        uint32_t tiles_per_row = (stride + 31) >> 5;
        uint32_t tile = (x >> 5) + tiles_per_row * (y >> 5);
        uint32_t index = (tile << 10) | (x & 31) | ((y & 31) << 5);

        return (size_t)index * bytes_per_pixel;
    }

    if (surface->surfaceType == SCE_GXM_COLOR_SURFACE_SWIZZLED &&
        surface->width == surface->height &&
        (surface->width & (surface->width - 1)) == 0)
        return (size_t)gxm_morton2(x, y) * bytes_per_pixel;

    return ((size_t)y * stride + x) * bytes_per_pixel;
}

/* 4x4 ordered dither matrix (Bayer). */
static const uint8_t gxm_dither_matrix[4][4] = {
    { 0, 8, 2, 10 },
    { 12, 4, 14, 6 },
    { 3, 11, 1, 9 },
    { 15, 7, 13, 5 },
};

/*
 * Quantise an 8-bit channel to `bits`. Without dithering the high bits are
 * kept (the historical behaviour); with dithering the ordered threshold biases
 * each pixel so the average over a 4x4 cell matches the full-precision value.
 * The threshold is deliberately not applied to alpha: dithering coverage makes
 * transparency shimmer.
 */
static uint32_t gxm_quantize_channel(uint8_t value, unsigned int bits, bool dither, uint32_t x,
                                     uint32_t y)
{
    unsigned int max = (1u << bits) - 1;
    unsigned int threshold;
    unsigned int q;

    if (!dither)
        return (uint32_t)value >> (8 - bits);

    threshold = ((unsigned int)gxm_dither_matrix[y & 3][x & 3] * 255u) >> 4;
    q = ((unsigned int)value * max + threshold) / 255u;
    return q > max ? max : q;
}

static void gxm_write_color_pixel(uint8_t *dst, const uint8_t rgba[4], SceGxmColorFormat format,
                                  uint32_t x, uint32_t y, bool gamma, bool dither)
{
    uint32_t base = format & SCE_GXM_COLOR_BASE_FORMAT_MASK;
    uint32_t swizzle = (format >> 20) & 3;
    uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];

    if (gamma) {
        if (!s_color_tables_ready)
            gxm_color_tables_init();
        r = s_linear_to_srgb[r];
        g = s_linear_to_srgb[g];
        b = s_linear_to_srgb[b];
    }

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
        uint32_t rq = gxm_quantize_channel(r, 5, dither, x, y);
        uint32_t gq = gxm_quantize_channel(g, 6, dither, x, y);
        uint32_t bq = gxm_quantize_channel(b, 5, dither, x, y);
        uint16_t value = swizzle & 1 ? (uint16_t)((rq << 11) | (gq << 5) | bq)
                                     : (uint16_t)((bq << 11) | (gq << 5) | rq);
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U1U5U5U5: {
        uint32_t rq = gxm_quantize_channel(r, 5, dither, x, y);
        uint32_t gq = gxm_quantize_channel(g, 5, dither, x, y);
        uint32_t bq = gxm_quantize_channel(b, 5, dither, x, y);
        uint16_t value = (uint16_t)(((a >> 7) << 15) | (bq << 10) | (gq << 5) | rq);
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U4U4U4U4: {
        uint32_t rq = gxm_quantize_channel(r, 4, dither, x, y);
        uint32_t gq = gxm_quantize_channel(g, 4, dither, x, y);
        uint32_t bq = gxm_quantize_channel(b, 4, dither, x, y);
        uint16_t value = (uint16_t)(((a >> 4) << 12) | (bq << 8) | (gq << 4) | rq);
        dst[0] = (uint8_t)(value & 0xff);
        dst[1] = (uint8_t)(value >> 8);
        break;
    }

    case SCE_GXM_COLOR_BASE_FORMAT_U8U3U3U2: {
        uint32_t rq = gxm_quantize_channel(r, 3, dither, x, y);
        uint32_t gq = gxm_quantize_channel(g, 3, dither, x, y);
        uint32_t bq = gxm_quantize_channel(b, 2, dither, x, y);
        uint16_t value = (uint16_t)(((uint16_t)a << 8) | (rq << 5) | (gq << 2) | bq);
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

static void gxm_profile_frame(uint64_t resolve_ns, uint64_t finish_ns, uint64_t read_ns,
                              uint64_t convert_ns)
{
    static uint64_t resolve_total, finish_total, read_total, convert_total, last;
    static unsigned int frames;
    uint64_t now;

    if (!getenv("VITA2NATIVE_PROFILE"))
        return;

    resolve_total += resolve_ns;
    finish_total += finish_ns;
    read_total += read_ns;
    convert_total += convert_ns;
    frames++;

    now = gxm_time_ns();
    if (!last) {
        last = now;
        return;
    }
    if (now - last < 2000000000ull)
        return;

    LOG("SceGxm: %u frames in %.1f s: draw %.1f (flush %.1f [commit %.1f gldraw %.1f], uniform "
        "%.1f state %.1f tex %.1f attrib %.1f) resolve %.1f finish %.1f read %.1f convert "
        "%.1f ds %.1f ms/frame (%u gl draws, %u vertices, %u uploads/%llu KiB per frame, "
        "dsload %.1f ms (%u calls), "
        "breaks %u/%u/%u/%u [prog%u prim%u dfunc%u dwrite%u dbias%u stencil%u])",
        frames, (double)(now - last) / 1e9, (double)g_profile_draw_ns / frames / 1e6,
        (double)g_profile_flush_ns / frames / 1e6, (double)g_profile_commit_ns / frames / 1e6,
        (double)g_profile_gldraw_ns / frames / 1e6, (double)g_profile_uniform_ns / frames / 1e6,
        (double)g_profile_state_ns / frames / 1e6, (double)g_profile_texture_ns / frames / 1e6,
        (double)g_profile_attrib_ns / frames / 1e6, (double)resolve_total / frames / 1e6,
        (double)finish_total / frames / 1e6, (double)read_total / frames / 1e6,
        (double)convert_total / frames / 1e6, (double)g_profile_ds_ns / frames / 1e6,
        g_profile_draws / frames, g_profile_vertices / frames, g_profile_uploads / frames,
        (unsigned long long)(g_profile_upload_bytes / frames / 1024),
        (double)g_profile_ds_load_ns / frames / 1e6, g_profile_ds_load_calls / frames,
        g_batch_break_state / frames,
        g_batch_break_size / frames, g_batch_break_uniform / frames,
        g_batch_break_texture / frames, g_batch_break_program / frames,
        g_batch_break_prim / frames, g_batch_break_depth_func / frames,
        g_batch_break_depth_write / frames, g_batch_break_depth_bias / frames,
        g_batch_break_stencil / frames);

    resolve_total = finish_total = read_total = convert_total = 0;
    g_profile_draws = g_profile_vertices = g_profile_uploads = 0;
    g_profile_ds_ns = 0;
    g_profile_ds_load_ns = 0;
    g_profile_ds_load_calls = 0;
    g_profile_flush_ns = 0;
    g_profile_commit_ns = 0;
    g_profile_gldraw_ns = 0;
    g_profile_upload_bytes = 0;
    g_profile_draw_ns = 0;
    g_profile_uniform_ns = 0;
    g_profile_state_ns = 0;
    g_profile_texture_ns = 0;
    g_profile_attrib_ns = 0;
    g_batch_break_state = 0;
    g_batch_break_program = 0;
    g_batch_break_surface = 0;
    g_batch_break_clip = 0;
    g_batch_break_state_other = 0;
    g_batch_break_prim = 0;
    g_batch_break_viewport = 0;
    g_batch_break_depth = 0;
    g_batch_break_depth_func = 0;
    g_batch_break_depth_write = 0;
    g_batch_break_depth_bias = 0;
    g_batch_break_stencil = 0;
    g_batch_break_rest = 0;
    g_batch_break_size = 0;
    g_batch_break_uniform = 0;
    g_batch_break_texture = 0;
    frames = 0;
    last = now;
}

static int gxm_frame_trace(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_GXM_FRAME_TRACE") != NULL;
    return enabled;
}

static int gxm_state_trace(void)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("VITA2NATIVE_GXM_STATE_TRACE") != NULL;
    return enabled;
}

EXPORT(SceGxm, 0xFE300E2F, int, sceGxmEndScene, SceGxmContext *context,
       const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification)
{
    const SceGxmColorSurfaceInner *surface;
    uint32_t width, height;
    uint8_t *rgba;
    uint64_t t_resolve, t0, t1, t2;

    (void)vertexNotification;
    (void)fragmentNotification;

    host_gpu_thread_enter(g_device);

    if (!context)
        return SCE_GXM_ERROR_INVALID_POINTER;
    if (context->deferred)
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (!context->in_scene)
        return SCE_GXM_ERROR_NOT_WITHIN_SCENE;

    context->in_scene = false;
    g_gxm_frame++;

    gxm_batch_flush(context);

    if (!g_gl || !context->render_target)
        return 0;

    {
        uint64_t start = gxm_time_ns();

        gxm_resolve_render_target((SceGxmRenderTarget *)context->render_target);
        t_resolve = gxm_time_ns() - start;
    }
    g_gl->glBindFramebuffer(GL_FRAMEBUFFER, context->render_target->fbo);
    t0 = gxm_time_ns();
    g_gl->glFinish();
    t1 = gxm_time_ns();

    surface = context->color_surface;
    if (surface && surface->data && !surface->disabled) {
        const SceGxmRenderTarget *target = context->render_target;
        uint32_t scale = 1;

        width = surface->width < target->params.width ? surface->width : target->params.width;
        height = surface->height < target->params.height ? surface->height : target->params.height;

        /*
         * Without MSAA_DOWNSCALE the guest expects the supersampled image in
         * the colour surface (twice the render-target size), then reduces it
         * itself with sceGxmTransferDownscale. We cannot expose the individual
         * GL samples, so resolve and replicate each pixel into its 2x2 block.
         * A subsequent average reconstructs the resolved image.
         */
        if (target->samples && !surface->downscale &&
            surface->width >= target->params.width * 2 &&
            surface->height >= target->params.height * 2) {
            scale = 2;
            width = target->params.width;
            height = target->params.height;
        }

        rgba = malloc((size_t)width * height * 4);
        if (!rgba)
            return SCE_GXM_ERROR_OUT_OF_MEMORY;

        g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        g_gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        t2 = gxm_time_ns();

        if (gxm_frame_trace() && (g_gxm_frame % 60) == 0) {
            uint32_t sum = 0;
            uint32_t nonzero = 0;

            for (size_t i = 0; i < (size_t)width * height * 4; i += 4096) {
                sum += rgba[i];
                if (rgba[i])
                    nonzero++;
            }
            LOG("SceGxm frame %llu: fbo=%u surf=%p %ux%u stride=%u fmt=0x%x "
                "clip=%u,%u..%u,%u scale=%u sum=%08x nz=%u px0=%08x",
                g_gxm_frame, context->render_target->fbo, (void *)surface->data, surface->width,
                surface->height, surface->strideInPixels, surface->colorFormat, surface->clipXMin,
                surface->clipYMin, surface->clipXMax, surface->clipYMax, scale, sum, nonzero,
                *(const uint32_t *)rgba);
        }

        /* GL's origin is bottom-left; Vita surfaces are top-left. */
        {
            uint32_t bytes_per_pixel = gxm_color_format_bytes(surface->colorFormat);
            uint32_t clip_x0 = surface->clipXMin;
            uint32_t clip_y0 = surface->clipYMin;
            uint32_t clip_x1 = surface->clipXMax;
            uint32_t clip_y1 = surface->clipYMax;
            bool gamma = surface->gamma_mode != 0;
            bool dither = surface->dither_mode != 0;
            uint32_t row_pixels = surface->width < width ? surface->width : width;
            uint32_t row_count = surface->height < height ? surface->height : height;
            uint32_t stride = surface->strideInPixels ? surface->strideInPixels : surface->width;
            bool direct = !gamma && !dither && scale == 1 && bytes_per_pixel == 4 &&
                          surface->surfaceType != SCE_GXM_COLOR_SURFACE_TILED &&
                          surface->surfaceType != SCE_GXM_COLOR_SURFACE_SWIZZLED &&
                          (surface->colorFormat & SCE_GXM_COLOR_BASE_FORMAT_MASK) ==
                              SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8 &&
                          ((surface->colorFormat >> 20) & 3) == 0; /* ABGR == GL byte order */

            if (clip_x1 >= surface->width)
                clip_x1 = surface->width - 1;
            if (clip_y1 >= surface->height)
                clip_y1 = surface->height - 1;

            /* The common Vita format (A8B8G8R8) is byte-identical to the GL
               readback, so full-surface frames are a row-wise memcpy. */
            if (direct && clip_x0 == 0 && clip_y0 == 0 && clip_x1 + 1 == row_pixels &&
                clip_y1 + 1 == row_count) {
                for (uint32_t y = 0; y < row_count; y++) {
                    memcpy((uint8_t *)surface->data + (size_t)y * stride * 4,
                           rgba + (size_t)(height - 1 - y) * width * 4,
                           (size_t)row_pixels * 4);
                }
                free(rgba);
                gxm_profile_frame(t_resolve, t1 - t0, t2 - t1, gxm_time_ns() - t2);
                goto convert_done;
            }

            for (uint32_t y = clip_y0; y <= clip_y1; y++) {
                uint32_t sy = scale > 1 ? y / scale : y;
                const uint8_t *src;

                if (sy >= height)
                    continue;
                src = rgba + (size_t)(height - 1 - sy) * width * 4;

                for (uint32_t x = clip_x0; x <= clip_x1; x++) {
                    uint32_t sx = scale > 1 ? x / scale : x;
                    uint8_t *dst;

                    if (sx >= width)
                        continue;
                    dst = (uint8_t *)surface->data +
                          gxm_color_pixel_offset(surface, x, y, bytes_per_pixel);

                    gxm_write_color_pixel(dst, src + (size_t)sx * 4, surface->colorFormat, x, y,
                                          gamma, dither);
                }
            }
        }

        free(rgba);
        gxm_profile_frame(t_resolve, t1 - t0, t2 - t1, gxm_time_ns() - t2);
    }

convert_done:

    if (context->ds_store_surface) {
        uint64_t ds_started = gxm_time_ns();

        gxm_ds_store_surface((SceGxmRenderTarget *)context->render_target,
                             context->ds_store_surface);
        g_profile_ds_ns += gxm_time_ns() - ds_started;
    }

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

    /* The callback is where libvita2d blits the render target to the front
       buffer, so it runs GL work on this thread. */
    host_gpu_thread_enter(g_device);

    if (g_current_context)
        gxm_batch_flush(g_current_context);

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
    if (gxm_program_trace() && fragmentProgram && !fragmentProgram->shader)
        LOG("SceGxm: bind fragment program %p without a GL shader (programId %p header %p type %d)",
            (const void *)fragmentProgram, (const void *)fragmentProgram->programId,
            fragmentProgram->programId ? (const void *)fragmentProgram->programId->programHeader
                                       : NULL,
            fragmentProgram->programId && fragmentProgram->programId->programHeader
                ? (int)(fragmentProgram->programId->programHeader->program_flags & 1)
                : -1);

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

    if (gxm_program_trace() && uniformBuffer) {
        static unsigned int traced;

        if (traced < 40) {
            const float *f = (const float *)uniformBuffer;
            uint32_t words = context->vertex_program
                                 ? context->vertex_program->programId->programHeader
                                       ->default_uniform_buffer_count
                                 : 36;

            LOG("GXM set vdefault -> %p (%u words)", uniformBuffer, words);
            for (uint32_t row = 0; row < words; row += 4)
                LOG("  u%u: %08x %08x %08x %08x", row, ((const uint32_t *)f)[row],
                    ((const uint32_t *)f)[row + 1], ((const uint32_t *)f)[row + 2],
                    ((const uint32_t *)f)[row + 3]);
            traced++;
        }
    }

    return 0;
}

static uint32_t gxm_uniform_padding(void)
{
    static int padding = -1;

    if (padding < 0) {
        const char *value = getenv("VITA2NATIVE_UNIFORM_PADDING");

        padding = value ? atoi(value) : 0;
    }

    return padding > 0 ? (uint32_t)padding : 0;
}

EXPORT(SceGxm, 0x97118913, int, sceGxmReserveVertexDefaultUniformBuffer, SceGxmContext *context,
       void **uniformBuffer)
{
    uint32_t size;
    uint32_t padding;

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
    /* Debug aid: reserve extra arena space behind each default uniform buffer so
       a title writing past the declared size does not get clobbered by the next
       reservation before the draw. */
    padding = gxm_uniform_padding();
    if (context->vertex_uniform_head + size + padding > GXM_UNIFORM_ARENA_SIZE)
        context->vertex_uniform_head = 0;

    context->vertex_uniforms = context->vertex_uniform_arena + context->vertex_uniform_head;
    context->vertex_uniform_size = size;
    /* Diagnose which default-uniform words a title actually writes. */
    if (getenv("VITA2NATIVE_POISON_UNIFORMS"))
        memset(context->vertex_uniforms, 0xCD, size + padding);
    context->vertex_uniform_head += size + padding;
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
    uint32_t padding;

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
    padding = gxm_uniform_padding();
    if (context->fragment_uniform_head + size + padding > GXM_UNIFORM_ARENA_SIZE)
        context->fragment_uniform_head = 0;

    context->fragment_uniforms = context->fragment_uniform_arena + context->fragment_uniform_head;
    context->fragment_uniform_size = size;
    if (getenv("VITA2NATIVE_POISON_UNIFORMS"))
        memset(context->fragment_uniforms, 0xCD, size + padding);
    context->fragment_uniform_head += size + padding;
    *uniformBuffer = context->fragment_uniforms;
    return 0;
}

static uint16_t gxm_float_to_half(float value)
{
    uint32_t bits;
    uint32_t sign;
    uint32_t raw_exponent;
    uint32_t mantissa;
    int exponent;

    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 16) & 0x8000u;
    raw_exponent = (bits >> 23) & 0xffu;
    mantissa = bits & 0x7fffffu;

    if (raw_exponent == 0xffu)
        return (uint16_t)(sign | (mantissa ? 0x7e00u : 0x7c00u));

    /* Rebias the exponent from 127 to 15. */
    exponent = (int)raw_exponent - 127 + 15;

    if (exponent >= 0x1f)
        return (uint16_t)(sign | 0x7c00u);

    if (exponent <= 0) {
        uint32_t half;
        uint32_t shift;
        uint32_t remainder;

        if (exponent < -10)
            return (uint16_t)sign;

        mantissa |= 0x800000u;
        shift = (uint32_t)(14 - exponent);
        half = mantissa >> shift;
        remainder = mantissa & ((1u << shift) - 1u);
        if (remainder > (1u << (shift - 1)) ||
            (remainder == (1u << (shift - 1)) && (half & 1u)))
            half++;

        return (uint16_t)(sign | half);
    }

    {
        uint32_t half = mantissa >> 13;
        uint32_t remainder = mantissa & 0x1fffu;
        uint16_t result = (uint16_t)(sign | ((uint32_t)exponent << 10) | half);

        if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
            result++;

        return result;
    }
}

EXPORT(SceGxm, 0x65DD0C84, int, sceGxmSetUniformDataF, void *uniformBuffer,
       const SceGxmProgramParameter *parameter, unsigned int componentOffset,
       unsigned int componentCount, const float *sourceData)
{
    bool is_float;
    uint32_t alignment, scalar_size, vector_size, vector_index, scalar_index;
    uint32_t vector_mod_align, vector_padding_bytes, offset, scalars_to_copy;

    if (!uniformBuffer || !parameter || !sourceData)
        return SCE_GXM_ERROR_INVALID_POINTER;

    is_float = (parameter->type == SCE_GXM_PARAMETER_TYPE_F16) ||
               (parameter->type == SCE_GXM_PARAMETER_TYPE_F32);

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
        char *dest = (char *)uniformBuffer + offset;

        if (parameter->type == SCE_GXM_PARAMETER_TYPE_F16) {
            /* The API takes floats regardless of the parameter type; F16
               parameters are packed two to a 32-bit word in the buffer. */
            uint16_t *half = (uint16_t *)dest;

            for (uint32_t i = 0; i < scalars_to_copy; i++)
                half[i] = gxm_float_to_half(sourceData[i]);
        } else {
            memcpy(dest, sourceData, scalars_to_copy * scalar_size);
        }

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

    if (gammaMode & ~0x18000000u)
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

EXPORT(SceGxm, 0x7B9023C3, SceGxmParameterType, sceGxmProgramParameterGetType,
       const SceGxmProgramParameter *parameter)
{
    if (getenv("VITA2NATIVE_TRACE_PARAMS") && parameter) {
        static unsigned int logged;

        if (logged < 32) {
            const char *name = (const char *)((uintptr_t)parameter + parameter->name_offset);

            LOG("sceGxmProgramParameterGetType(%s) -> %u comps=%u res=%u", name,
                (unsigned)parameter->type, (unsigned)parameter->component_count,
                parameter->resource_index);
            logged++;
        }
    }
    return parameter->type;
}
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

static void gxm_fill_draw_state(SceGxmContext *context, GLuint program,
                                SceGxmPrimitiveType prim, SceGxmIndexFormat index_format,
                                unsigned int instances, GxmDrawState *state)
{
    memset(state, 0, sizeof(*state));
    state->program = program;
    state->color_surface = context->color_surface;
    state->surface_width =
        context->color_surface && context->color_surface->width ? context->color_surface->width : 960;
    state->surface_height = context->color_surface && context->color_surface->height
                                ? context->color_surface->height
                                : 544;
    state->ds_surface = context->ds_surface;
    state->viewport_enable = context->viewport_enable;
    state->viewport_x_offset = context->viewport_x_offset;
    state->viewport_x_scale = context->viewport_x_scale;
    state->viewport_y_offset = context->viewport_y_offset;
    state->viewport_y_scale = context->viewport_y_scale;
    state->viewport_z_offset = context->viewport_z_offset;
    state->viewport_z_scale = context->viewport_z_scale;
    state->region_clip_mode = context->region_clip_mode;
    state->region_clip_xmin = context->region_clip_xmin;
    state->region_clip_ymin = context->region_clip_ymin;
    state->region_clip_xmax = context->region_clip_xmax;
    state->region_clip_ymax = context->region_clip_ymax;
    state->cull_mode = context->cull_mode;
    state->polygon_mode = context->front_polygon_mode;
    state->point_line_width = context->front_point_line_width;
    state->depth_bias_factor = context->front_depth_bias_factor;
    state->depth_bias_units = context->front_depth_bias_units;
    state->two_sided = context->two_sided;
    state->front_fragment_program_enable = context->front_fragment_program_enable;
    state->back_fragment_program_enable = context->back_fragment_program_enable;
    state->depth_func = context->depth_func;
    state->depth_write = context->depth_write;
    state->front_stencil_func = context->front_stencil_func;
    state->back_stencil_func = context->back_stencil_func;
    state->front_stencil_fail = context->front_stencil_fail;
    state->front_stencil_depth_fail = context->front_stencil_depth_fail;
    state->front_stencil_pass = context->front_stencil_pass;
    state->back_stencil_fail = context->back_stencil_fail;
    state->back_stencil_depth_fail = context->back_stencil_depth_fail;
    state->back_stencil_pass = context->back_stencil_pass;
    state->front_stencil_compare_mask = context->front_stencil_compare_mask;
    state->front_stencil_write_mask = context->front_stencil_write_mask;
    state->back_stencil_compare_mask = context->back_stencil_compare_mask;
    state->back_stencil_write_mask = context->back_stencil_write_mask;
    state->front_stencil_ref = context->front_stencil_ref;
    state->back_stencil_ref = context->back_stencil_ref;
    state->prim = prim;
    state->index_format = index_format;
    state->instances = instances;
}

static bool gxm_batch_matches(SceGxmContext *context, const GxmDrawState *state,
                              uint32_t vertex_uniform_size, uint32_t fragment_uniform_size)
{
    const GxmDrawBatch *batch = &context->batch;

    if (!batch->active)
        return false;
    if (memcmp(&batch->state, state, sizeof(*state)) != 0) {
        g_batch_break_state++;
        if (batch->state.program != state->program)
            g_batch_break_program++;
        else if (batch->state.color_surface != state->color_surface ||
                 batch->state.ds_surface != state->ds_surface)
            g_batch_break_surface++;
        else if (batch->state.region_clip_mode != state->region_clip_mode ||
                 batch->state.region_clip_xmin != state->region_clip_xmin ||
                 batch->state.region_clip_ymin != state->region_clip_ymin ||
                 batch->state.region_clip_xmax != state->region_clip_xmax ||
                 batch->state.region_clip_ymax != state->region_clip_ymax)
            g_batch_break_clip++;
        else if (batch->state.prim != state->prim ||
                 batch->state.index_format != state->index_format ||
                 batch->state.instances != state->instances)
            g_batch_break_prim++;
        else if (batch->state.viewport_enable != state->viewport_enable ||
                 batch->state.viewport_x_offset != state->viewport_x_offset ||
                 batch->state.viewport_x_scale != state->viewport_x_scale ||
                 batch->state.viewport_y_offset != state->viewport_y_offset ||
                 batch->state.viewport_y_scale != state->viewport_y_scale ||
                 batch->state.viewport_z_offset != state->viewport_z_offset ||
                 batch->state.viewport_z_scale != state->viewport_z_scale)
            g_batch_break_viewport++;
        else if (batch->state.depth_func != state->depth_func) {
            g_batch_break_depth++;
            g_batch_break_depth_func++;
        } else if (batch->state.depth_write != state->depth_write) {
            g_batch_break_depth++;
            g_batch_break_depth_write++;
        } else if (batch->state.depth_bias_factor != state->depth_bias_factor ||
                   batch->state.depth_bias_units != state->depth_bias_units) {
            g_batch_break_depth++;
            g_batch_break_depth_bias++;
        } else if (batch->state.front_stencil_func != state->front_stencil_func ||
                   batch->state.back_stencil_func != state->back_stencil_func ||
                   batch->state.front_stencil_fail != state->front_stencil_fail ||
                   batch->state.front_stencil_depth_fail != state->front_stencil_depth_fail ||
                   batch->state.front_stencil_pass != state->front_stencil_pass ||
                   batch->state.back_stencil_fail != state->back_stencil_fail ||
                   batch->state.back_stencil_depth_fail != state->back_stencil_depth_fail ||
                   batch->state.back_stencil_pass != state->back_stencil_pass) {
            g_batch_break_depth++;
            g_batch_break_stencil++;
            if (getenv("VITA2NATIVE_BATCH_DEBUG")) {
                static unsigned int shown;

                if (shown++ < 12)
                    LOG("stencil break: front %d %d/%d/%d back %d %d/%d/%d -> "
                        "front %d %d/%d/%d back %d %d/%d/%d",
                        batch->state.front_stencil_func, batch->state.front_stencil_fail,
                        batch->state.front_stencil_depth_fail, batch->state.front_stencil_pass,
                        batch->state.back_stencil_func, batch->state.back_stencil_fail,
                        batch->state.back_stencil_depth_fail, batch->state.back_stencil_pass,
                        state->front_stencil_func, state->front_stencil_fail,
                        state->front_stencil_depth_fail, state->front_stencil_pass,
                        state->back_stencil_func, state->back_stencil_fail,
                        state->back_stencil_depth_fail, state->back_stencil_pass);
            }
        } else
            g_batch_break_rest++;
        return false;
    }
    if (batch->vertex_uniform_size != vertex_uniform_size ||
        batch->fragment_uniform_size != fragment_uniform_size) {
        g_batch_break_size++;
        return false;
    }
    if (vertex_uniform_size &&
        memcmp(batch->vertex_uniforms, context->vertex_block_data, vertex_uniform_size) != 0) {
        g_batch_break_uniform++;
        return false;
    }
    if (fragment_uniform_size &&
        memcmp(batch->fragment_uniforms, context->fragment_block_data, fragment_uniform_size) != 0) {
        g_batch_break_uniform++;
        return false;
    }
    if (memcmp(batch->fragment_textures, context->fragment_textures,
               sizeof(batch->fragment_textures)) != 0) {
        g_batch_break_texture++;
        return false;
    }
    if (memcmp(batch->vertex_textures, context->vertex_textures, sizeof(batch->vertex_textures)) !=
        0) {
        g_batch_break_texture++;
        return false;
    }

    return true;
}

static int gxm_flush_trace_from(void)
{
    static int frame = -2;

    if (frame == -2) {
        const char *value = getenv("VITA2NATIVE_GXM_FLUSH_TRACE");
        frame = value ? atoi(value) : -1;
    }
    return frame;
}

/* Debug aid: write the render target after every flush of one frame, so each
   draw's contribution can be inspected on its own. */
static void gxm_dump_flush(SceGxmContext *context, const GxmDrawBatch *batch)
{
    static int frame = -2;
    static unsigned int seq;
    const char *dir;
    uint32_t width, height;
    uint8_t *rgba, *flipped;
    char path[PATH_MAX];

    if (frame == -2) {
        const char *value = getenv("VITA2NATIVE_GXM_FRAME_DUMP");
        frame = value ? atoi(value) : -1;
    }
    if (frame < 0 || g_gxm_frame != (uint64_t)frame)
        return;

    if (context->render_target)
        g_gl->glBindFramebuffer(GL_FRAMEBUFFER, context->render_target->fbo);

    dir = getenv("VITA2NATIVE_GXM_FRAME_DUMP_DIR");
    if (!dir)
        dir = "/tmp";

    width = batch->state.surface_width;
    height = batch->state.surface_height;
    if (!width || !height || width > 4096 || height > 4096)
        return;

    rgba = malloc((size_t)width * height * 4);
    flipped = malloc((size_t)width * height * 4);
    if (!rgba || !flipped) {
        free(rgba);
        free(flipped);
        return;
    }

    g_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_gl->glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (uint32_t y = 0; y < height; y++)
        memcpy(flipped + (size_t)y * width * 4,
               rgba + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);

    snprintf(path, sizeof(path), "%s/f%llu_%02u_prog%u_idx%u.png", dir,
             (unsigned long long)g_gxm_frame, seq++, batch->state.program, batch->index_count);
    stbi_write_png(path, (int)width, (int)height, 4, flipped, (int)width * 4);

    free(rgba);
    free(flipped);
}

static void gxm_trace_batch(SceGxmContext *context, const GxmDrawBatch *batch)
{
    static unsigned int traced;
    const SceGxmTextureInner *t0 = &batch->fragment_textures[0];
    const uint32_t *td = gxm_texture_get_data(t0);
    static int vert_frame = -2;

    if (vert_frame == -2) {
        const char *value = getenv("VITA2NATIVE_GXM_VERT_TRACE");
        vert_frame = value ? atoi(value) : -1;
    }

    if (vert_frame >= 0 && g_gxm_frame == (uint64_t)vert_frame) {
        const SceGxmVertexProgram *vp = batch->vertex_program;

        if (vp) {
            LOG("vert frame %llu: attrs=%u streams=%u idx=%u verts=%u",
                (unsigned long long)g_gxm_frame, vp->attributeCount, vp->streamCount,
                batch->index_count, batch->vertex_count);
            for (unsigned int a = 0; a < vp->attributeCount; a++) {
                const SceGxmVertexAttribute *attr = &vp->attributes[a];

                LOG("  attr %u: reg=%u stream=%u offset=%u fmt=%u comps=%u", a, attr->regIndex,
                    attr->streamIndex, attr->offset, attr->format, attr->componentCount);
            }
            for (unsigned int s = 0; s < vp->streamCount; s++) {
                const GxmBufferArena *arena = &context->stream_arena[s];
                uint32_t stride = vp->streams[s].stride;

                if (!stride || !arena->stage)
                    continue;
                LOG("  stream %u: stride=%u first=%p", s, stride,
                    (const void *)(arena->stage + batch->stream_start[s]));
                for (unsigned int v = 0; v < 12 && v < batch->vertex_count; v++) {
                    const uint32_t *w =
                        (const uint32_t *)(arena->stage + batch->stream_start[s] + (size_t)v * stride);

                    LOG("    v%u: %08x %08x %08x %08x %08x %08x %08x %08x", v, w[0], w[1], w[2],
                        w[3], w[4], w[5], w[6], w[7]);
                }
            }
        }
    }

    if (traced >= 3000)
        return;
    traced++;

    LOG("flush %llu: prog=%u draws=%u idx=%u verts=%u surf=%ux%u ds=%p tex=%p %ux%u fmt=0x%x "
        "t0=%08x depth=%d/%d vp=%.1f,%.1f %.1f,%.1f clip=%u,%u..%u,%u prim=%u blend=%x/%d/%d/%d/%d",
        (unsigned long long)g_gxm_frame, batch->state.program, batch->draws, batch->index_count,
        batch->vertex_count, batch->state.surface_width, batch->state.surface_height,
        (const void *)batch->state.ds_surface, (const void *)td,
        (unsigned)gxm_texture_get_width(t0), (unsigned)gxm_texture_get_height(t0),
        (unsigned)gxm_texture_get_format(t0), td ? td[0] : 0, (int)batch->state.depth_func,
        (int)batch->state.depth_write, batch->state.viewport_x_offset,
        batch->state.viewport_x_scale, batch->state.viewport_y_offset,
        batch->state.viewport_y_scale, batch->state.region_clip_xmin,
        batch->state.region_clip_ymin, batch->state.region_clip_xmax,
        batch->state.region_clip_ymax, (unsigned)batch->state.prim,
        batch->fragment_uniform_size ? ((const uint32_t *)batch->fragment_uniforms)[0] : 0,
        batch->state.front_stencil_func, batch->state.front_stencil_fail,
        batch->state.front_stencil_depth_fail, batch->state.front_stencil_pass,
        batch->state.front_stencil_ref);

    {
        const SceGxmVertexProgram *vp = batch->vertex_program;
        const SceGxmProgram *prog = vp ? vp->programId->programHeader : NULL;
        const GxmBufferArena *raw = &context->stream_arena[0];
        uint32_t raw_stride = vp && vp->streamCount ? vp->streams[0].stride : 0;

        if (raw->stage && raw_stride) {
            const uint8_t *raw_base = raw->stage + batch->stream_start[0];

            for (unsigned int v = 0; v < 3 && v < batch->vertex_count; v++) {
                const float *p = (const float *)(raw_base + (size_t)v * raw_stride);

                LOG("flush raw %u: %.3f %.3f %.3f %.3f | %08x %08x", v, p[0], p[1], p[2], p[3],
                    ((const uint32_t *)p)[4], ((const uint32_t *)p)[5]);
            }
        }
        const SceGxmProgramParameter *pos =
            prog ? sceGxmProgramFindParameterBySemantic(prog, SCE_GXM_PARAMETER_SEMANTIC_POSITION, 0)
                 : NULL;

        for (unsigned int a = 0; pos && a < vp->attributeCount; a++) {
            const SceGxmVertexAttribute *attr = &vp->attributes[a];
            const GxmBufferArena *arena;
            uint32_t stride;
            const uint8_t *base;

            if (attr->regIndex != (uint16_t)pos->resource_index)
                continue;
            if (attr->streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
                break;
            arena = &context->stream_arena[attr->streamIndex];
            stride = vp->streams[attr->streamIndex].stride;
            if (!arena->stage || !stride || attr->format != SCE_GXM_ATTRIBUTE_FORMAT_F32)
                break;
            base = arena->stage + batch->stream_start[attr->streamIndex] + attr->offset;
            for (unsigned int v = 0; v < 4 && v < batch->vertex_count; v++) {
                const float *p = (const float *)(base + (size_t)v * stride);

                LOG("flush vert %u: %.2f %.2f %.2f %.2f", v, p[0], p[1], p[2],
                    attr->componentCount > 3 ? p[3] : 1.0f);
            }
            break;
        }
    }
}

static void gxm_batch_flush(SceGxmContext *context)
{
    GxmDrawBatch *batch = &context->batch;
    uint64_t started;

    if (!batch->active)
        return;

    started = gxm_time_ns();

    if (batch->index_count && context->index_arena.buffer) {
        const SceGxmVertexProgram *vp = batch->vertex_program;
        uint64_t commit_started = gxm_time_ns();

        if (vp) {
            for (unsigned int i = 0; i < vp->streamCount && i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
                if (!vp->streams[i].stride)
                    continue;
                gxm_arena_commit(&context->stream_arena[i], GL_ARRAY_BUFFER,
                                 batch->stream_start[i], context->stream_arena[i].used);
            }
        }
        gxm_arena_commit(&context->index_arena, GL_ELEMENT_ARRAY_BUFFER, batch->index_start,
                         context->index_arena.used);

        g_profile_commit_ns += gxm_time_ns() - commit_started;
        commit_started = gxm_time_ns();

        g_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, context->index_arena.buffer);

        if (gxm_state_trace()) {
            static unsigned int traced;

            if (traced < 8) {
                const SceGxmTextureInner *t0 = &batch->fragment_textures[0];
                const uint32_t *td = gxm_texture_get_data(t0);
                const uint32_t *vu = (const uint32_t *)batch->vertex_uniforms;
                const uint32_t *fu = (const uint32_t *)batch->fragment_uniforms;

                LOG("state %u (gxp %p): vp=%.1f,%.1f,%.1f,%.1f en=%d clip=%u,%u..%u,%u depth=%d/%d "
                    "tex=%p %ux%u fmt=0x%x t0=%08x vu=%08x,%08x,%08x,%08x fu=%08x,%08x blend=%x/%d/%d/%d/%d",
                    traced, vp ? (const void *)vp->programId->programHeader : NULL,
                    batch->state.viewport_x_offset, batch->state.viewport_y_offset,
                    batch->state.viewport_x_scale, batch->state.viewport_y_scale,
                    (int)batch->state.viewport_enable,
                    batch->state.region_clip_xmin, batch->state.region_clip_ymin,
                    batch->state.region_clip_xmax, batch->state.region_clip_ymax,
                    (int)batch->state.depth_func, (int)batch->state.depth_write, (void *)td,
                    (unsigned)gxm_texture_get_width(t0), (unsigned)gxm_texture_get_height(t0),
                    (unsigned)gxm_texture_get_format(t0), td ? td[0] : 0,
                    vu ? vu[0] : 0, vu ? vu[1] : 0, vu ? vu[2] : 0, vu ? vu[3] : 0,
                    fu ? fu[0] : 0, fu ? fu[1] : 0,
                    context->fragment_program ? (unsigned)context->fragment_program->blendInfo.colorMask : 0,
                    context->fragment_program ? (int)context->fragment_program->blendInfo.colorFunc : -1,
                    context->fragment_program ? (int)context->fragment_program->blendInfo.alphaFunc : -1,
                    context->fragment_program ? (int)context->fragment_program->blendInfo.colorSrc : -1,
                    context->fragment_program ? (int)context->fragment_program->blendInfo.colorDst : -1);
                if (vu) {
                    const float *vf = (const float *)vu;

                    for (unsigned int row = 0; row < 9; row++)
                        LOG("state %u u%d: %.4f %.4f %.4f %.4f", traced, row,
                            vf[row * 4 + 0], vf[row * 4 + 1], vf[row * 4 + 2], vf[row * 4 + 3]);
                }
                if (vp) {
                    const SceGxmProgram *prog = vp->programId->programHeader;
                    const SceGxmProgramParameter *pos =
                        sceGxmProgramFindParameterBySemantic(prog, SCE_GXM_PARAMETER_SEMANTIC_POSITION, 0);

                    for (unsigned int a = 0; pos && a < vp->attributeCount; a++) {
                        const SceGxmVertexAttribute *attr = &vp->attributes[a];
                        const GxmBufferArena *arena;
                        uint32_t stride;
                        const uint8_t *base;

                        if (attr->regIndex != (uint16_t)pos->resource_index)
                            continue;
                        if (attr->streamIndex >= SCE_GXM_MAX_VERTEX_STREAMS)
                            break;
                        arena = &context->stream_arena[attr->streamIndex];
                        stride = vp->streams[attr->streamIndex].stride;
                        if (!arena->stage || !stride || attr->format != SCE_GXM_ATTRIBUTE_FORMAT_F32)
                            break;
                        base = arena->stage + batch->stream_start[attr->streamIndex] + attr->offset;
                        for (unsigned int v = 0; v < 3 && v < batch->vertex_count; v++) {
                            const float *p = (const float *)(base + (size_t)v * stride);

                            LOG("state %u vert %u: %.2f %.2f %.2f %.2f", traced, v, p[0], p[1],
                                p[2], attr->componentCount > 3 ? p[3] : 1.0f);
                        }
                        break;
                    }
                }
                traced++;
            }
        }

        if ((gxm_flush_trace_from() >= 0 &&
             g_gxm_frame >= (uint64_t)gxm_flush_trace_from()) ||
            getenv("VITA2NATIVE_GXM_VERT_TRACE"))
            gxm_trace_batch(context, batch);

        if (getenv("VITA2NATIVE_GXM_FLUSH_PIXEL") && batch->state.surface_width &&
            batch->state.surface_height) {
            uint8_t px[4] = {0, 0, 0, 0};

            g_gl->glReadPixels((GLint)(batch->state.surface_width / 2),
                               (GLint)(batch->state.surface_height / 2), 1, 1, GL_RGBA,
                               GL_UNSIGNED_BYTE, px);
            LOG("flush %llu prog=%u idx=%u -> centre pixel %02x%02x%02x%02x",
                (unsigned long long)g_gxm_frame, batch->state.program, batch->index_count, px[0],
                px[1], px[2], px[3]);
        }

        if (getenv("VITA2NATIVE_GXM_FRAME_DUMP"))
            gxm_dump_flush(context, batch);

        if (batch->state.instances > 1 && batch->gl_primitive != GL_POINTS)
            g_gl->glDrawElementsInstanced(batch->gl_primitive, (GLsizei)batch->index_count,
                                          batch->gl_index_type,
                                          (const void *)(uintptr_t)batch->index_start,
                                          (GLsizei)batch->state.instances);
        else
            g_gl->glDrawElements(batch->gl_primitive, (GLsizei)batch->index_count,
                                 batch->gl_index_type,
                                 (const void *)(uintptr_t)batch->index_start);

        g_profile_draws++;
        g_profile_vertices += batch->index_count;
        g_profile_gldraw_ns += gxm_time_ns() - commit_started;
    }

    g_profile_flush_ns += gxm_time_ns() - started;
    batch->active = false;
}

static void gxm_batch_begin(SceGxmContext *context, const GxmLinkedProgram *linked,
                            const GxmDrawState *state, uint32_t vertex_uniform_size,
                            uint32_t fragment_uniform_size, GLenum gl_index_type,
                            GLenum gl_primitive, uint32_t index_alignment)
{
    GxmDrawBatch *batch = &context->batch;
    const SceGxmVertexProgram *vp = context->vertex_program;

    batch->state = *state;
    batch->vertex_program = context->vertex_program;
    batch->gl_index_type = gl_index_type;
    batch->gl_primitive = gl_primitive;
    batch->index_count = 0;
    batch->vertex_count = 0;
    batch->draws = 0;

    if (batch->vertex_uniform_size != vertex_uniform_size) {
        free(batch->vertex_uniforms);
        batch->vertex_uniforms = vertex_uniform_size ? malloc(vertex_uniform_size) : NULL;
        batch->vertex_uniform_size = batch->vertex_uniforms ? vertex_uniform_size : 0;
    }
    if (batch->fragment_uniform_size != fragment_uniform_size) {
        free(batch->fragment_uniforms);
        batch->fragment_uniforms = fragment_uniform_size ? malloc(fragment_uniform_size) : NULL;
        batch->fragment_uniform_size = batch->fragment_uniforms ? fragment_uniform_size : 0;
    }
    if (batch->vertex_uniforms && vertex_uniform_size)
        memcpy(batch->vertex_uniforms, context->vertex_block_data, vertex_uniform_size);
    if (batch->fragment_uniforms && fragment_uniform_size)
        memcpy(batch->fragment_uniforms, context->fragment_block_data, fragment_uniform_size);

    memcpy(batch->fragment_textures, context->fragment_textures,
           sizeof(batch->fragment_textures));
    memcpy(batch->vertex_textures, context->vertex_textures, sizeof(batch->vertex_textures));

    memset(batch->stream_start, 0, sizeof(batch->stream_start));
    for (unsigned int i = 0; i < vp->streamCount && i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        uint32_t stride = vp->streams[i].stride;

        if (!stride)
            continue;
        batch->stream_start[i] =
            gxm_arena_reserve_offset(&context->stream_arena[i], stride);
    }
    batch->index_start = gxm_arena_reserve_offset(&context->index_arena, index_alignment);

    gxm_flush_state(context, linked, batch->stream_start);
    batch->active = true;
}

static uint32_t gxm_batch_expanded_count(SceGxmPrimitiveType prim, unsigned int count)
{
    switch (prim) {
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
        return count >= 3 ? 3 * (count - 2) : 0;
    default:
        return count;
    }
}

/*
 * Writes the draw's indices as a triangle list (or plain lines/points) so
 * several draws can share one GL call. A GL triangle strip cannot simply be
 * concatenated with the next strip: that adds bridging triangles between the
 * two strips, which is exactly how the first batching attempt corrupted text.
 */
static void gxm_batch_write_indices(const void *indexData, SceGxmIndexFormat format,
                                    SceGxmPrimitiveType prim, unsigned int count, uint32_t rebase,
                                    uint8_t *out, uint32_t index_size)
{
    uint32_t written = 0;

    for (unsigned int i = 0; i < count; i++) {
        uint32_t index;

#define EMIT(value)                                                                                \
    do {                                                                                           \
        uint32_t emit_value = (value) + rebase;                                                    \
        if (index_size == 2)                                                                       \
            ((uint16_t *)out)[written] = (uint16_t)emit_value;                                     \
        else                                                                                       \
            ((uint32_t *)out)[written] = emit_value;                                               \
        written++;                                                                                 \
    } while (0)

        index = gxm_read_index(indexData, format, i);

        switch (prim) {
        case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
            if (i >= 2) {
                uint32_t k = i - 2;

                if (k & 1) {
                    EMIT(gxm_read_index(indexData, format, k + 1));
                    EMIT(gxm_read_index(indexData, format, k));
                } else {
                    EMIT(gxm_read_index(indexData, format, k));
                    EMIT(gxm_read_index(indexData, format, k + 1));
                }
                EMIT(gxm_read_index(indexData, format, k + 2));
            }
            break;
        case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
            if (i >= 2) {
                EMIT(gxm_read_index(indexData, format, 0));
                EMIT(gxm_read_index(indexData, format, i - 1));
                EMIT(index);
            }
            break;
        default:
            EMIT(index);
            break;
        }
#undef EMIT
    }
}

static bool gxm_batch_append(SceGxmContext *context, uint32_t max_index, const void *indexData,
                             unsigned int indexCount, uint32_t index_size)
{
    GxmDrawBatch *batch = &context->batch;
    const SceGxmVertexProgram *vp = context->vertex_program;
    uint32_t expanded = gxm_batch_expanded_count(batch->state.prim, indexCount);
    uint32_t bytes = expanded * index_size;
    uint32_t rebase = batch->vertex_count;
    uint8_t *scratch;

    for (unsigned int i = 0; i < vp->streamCount && i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
        uint32_t stride = vp->streams[i].stride;
        uint32_t size;

        if (!stride)
            continue;

        size = (max_index + 1) * stride;
        gxm_arena_append(&context->stream_arena[i],
                         context->streams[i].data, size, stride);
    }

    if (context->index_scratch_capacity < bytes) {
        uint32_t capacity = context->index_scratch_capacity ? context->index_scratch_capacity : 4096;

        while (capacity < bytes)
            capacity *= 2;
        free(context->index_scratch);
        context->index_scratch = malloc(capacity);
        context->index_scratch_capacity = context->index_scratch ? capacity : 0;
    }
    if (!context->index_scratch)
        return false;

    scratch = context->index_scratch;
    gxm_batch_write_indices(indexData, batch->state.index_format, batch->state.prim, indexCount,
                            rebase, scratch, index_size);

    gxm_arena_append(&context->index_arena, scratch, bytes, index_size);

    batch->index_count += expanded;
    batch->vertex_count += max_index + 1;
    batch->draws++;

    return true;
}

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

    gxm_batch_flush(context);

    return 0;
}

static void gxm_uniform_trace(SceGxmContext *context, unsigned int indexCount)
{
    static unsigned int traces;
    static uint32_t last_signature[4];
    static unsigned int last_count;
    const char *value = getenv("VITA2NATIVE_GXM_UNIFORM_TRACE");
    const uint32_t *vu, *fu;
    uint32_t vwords, fwords;
    uint32_t hash;
    uint32_t sig[4] = {0, 0, 0, 0};
    const SceGxmTextureInner *t0;
    const uint32_t *td;

    if (!value || traces >= 64 || !context->vertex_program || !context->fragment_program)
        return;

    hash = (uint32_t)strtoul(value, NULL, 16);
    if (context->fragment_program->shader_hash != hash)
        return;

    vu = context->vertex_uniforms;
    fu = context->fragment_uniforms;
    vwords = context->vertex_program->programId->programHeader->default_uniform_buffer_count;
    fwords = context->fragment_program->programId->programHeader->default_uniform_buffer_count;
    t0 = &context->fragment_textures[0];
    td = gxm_texture_get_data(t0);

    if (fwords > 0 && fu)
        sig[0] = fu[0];
    if (fwords > 1 && fu)
        sig[1] = fu[1];
    sig[2] = gxm_texture_get_format(t0);
    sig[3] = (uint32_t)gxm_texture_get_width(t0) << 16 | gxm_texture_get_height(t0);

    if (traces && sig[0] == last_signature[0] && sig[1] == last_signature[1] &&
        sig[2] == last_signature[2] && sig[3] == last_signature[3] &&
        indexCount == last_count)
        return;

    memcpy(last_signature, sig, sizeof(sig));
    last_count = indexCount;
    traces++;

    LOG("uniform draw %u frame %llu count %u: f u0=%08x u1=%08x u2=%08x u3=%08x | t0 %ux%u fmt=0x%x "
        "data=%p texel0=%08x | v u32=%08x u33=%08x u34=%08x u35=%08x",
        traces, (unsigned long long)g_gxm_frame, indexCount, sig[0], sig[1],
        fu && fwords > 2 ? fu[2] : 0, fu && fwords > 3 ? fu[3] : 0,
        (unsigned)gxm_texture_get_width(t0), (unsigned)gxm_texture_get_height(t0),
        (unsigned)gxm_texture_get_format(t0), (const void *)td,
        td ? td[0] : 0,
        vwords > 32 && vu ? vu[32] : 0, vwords > 33 && vu ? vu[33] : 0,
        vwords > 34 && vu ? vu[34] : 0, vwords > 35 && vu ? vu[35] : 0);
}

static int gxm_draw_common(SceGxmContext *context, SceGxmPrimitiveType primType,
                           SceGxmIndexFormat indexType, const void *indexData,
                           unsigned int indexCount, unsigned int instances)
{
    GLuint program;
    GxmLinkedProgram *linked = NULL;
    GLenum gl_index_type;
    GLenum gl_primitive;
    size_t index_size;
    uint32_t vertex_uniform_size, fragment_uniform_size;
    uint32_t max_index = 0;
    SceGxmPolygonMode polygon_mode;
    GxmDrawState state;
    bool can_merge;
    uint64_t draw_started = gxm_time_ns();

    host_gpu_thread_enter(g_device);

    /* A rejected draw is invisible: the guest believes it rendered, so tracing
       only the flushed batches makes missing geometry impossible to explain. */
    if (getenv("VITA2NATIVE_GXM_DRAW_ERRORS")) {
        static unsigned int logged;

        if (logged < 32) {
            const char *reason = NULL;

            if (!context->in_scene)
                reason = "not within scene";
            else if (!context->vertex_program || !context->fragment_program)
                reason = "null program";
            else if (!indexData || !indexCount)
                reason = "no index data";
            if (reason) {
                LOG("draw rejected (%s): prim=%u count=%u index=%p", reason, (unsigned)primType,
                    indexCount, indexData);
                logged++;
            }
        }
    }

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

    program = gxm_get_linked_program(context->vertex_program, context->fragment_program, &linked);
    if (!program) {
        if (getenv("VITA2NATIVE_GXM_DRAW_ERRORS")) {
            static unsigned int logged;

            if (logged < 64) {
                LOG("frame %llu ctx %p: draw rejected (unlinked program, cache %u/128): "
                    "vert=%p frag=%p count=%u",
                    (unsigned long long)g_gxm_frame, (void *)context, g_linked_count,
                    (const void *)context->vertex_program, (const void *)context->fragment_program,
                    indexCount);
                logged++;
            }
        }
        return SCE_GXM_ERROR_INVALID_VALUE;
    }

    for (unsigned int i = 0; i < indexCount; i++) {
        uint32_t index = gxm_read_index(indexData, indexType, i);
        if (index > max_index)
            max_index = index;
    }

    polygon_mode = context->front_polygon_mode;

    /* Batched draws are expanded to lists, so strips and fans become
       GL_TRIANGLES here. */
    switch (primType) {
    case SCE_GXM_PRIMITIVE_TRIANGLES:
    case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
    case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
        gl_primitive = GL_TRIANGLES;
        break;
    case SCE_GXM_PRIMITIVE_LINES:
        gl_primitive = GL_LINES;
        break;
    case SCE_GXM_PRIMITIVE_POINTS:
        gl_primitive = GL_POINTS;
        break;
    default:
        gl_primitive = gxm_primitive_to_gl(primType);
        break;
    }
    if (gxm_polygon_is_point(polygon_mode))
        gl_primitive = GL_POINTS;

    if (indexType == SCE_GXM_INDEX_FORMAT_U32) {
        gl_index_type = GL_UNSIGNED_INT;
        index_size = 4;
    } else {
        gl_index_type = GL_UNSIGNED_SHORT;
        index_size = 2;
    }

    /* Wireframe mode expands every triangle into lines; keep it unbatched. */
    if (gxm_polygon_is_line(polygon_mode)) {
        uint32_t wire_count = 0;
        uint32_t *wire;
        uint32_t stream_starts[SCE_GXM_MAX_VERTEX_STREAMS] = { 0 };
        const SceGxmVertexProgram *vp = context->vertex_program;

        gxm_batch_flush(context);

        for (unsigned int i = 0; i < vp->streamCount && i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
            const SceGxmVertexStream *s = &vp->streams[i];

            if (s->stride)
                gxm_arena_reserve(&context->stream_arena[i], GL_ARRAY_BUFFER,
                                  (max_index + 1) * s->stride, s->stride);
        }
        gxm_arena_reserve(&context->index_arena, GL_ELEMENT_ARRAY_BUFFER,
                          (uint32_t)(indexCount * index_size), 4);

        for (unsigned int i = 0; i < vp->streamCount && i < SCE_GXM_MAX_VERTEX_STREAMS; i++) {
            const SceGxmVertexStream *s = &vp->streams[i];
            uint32_t size;

            if (!s->stride)
                continue;
            size = (max_index + 1) * s->stride;
            stream_starts[i] = gxm_arena_reserve_offset(&context->stream_arena[i], s->stride);
            gxm_arena_upload(&context->stream_arena[i], GL_ARRAY_BUFFER,
                             context->streams[i].data, size, s->stride);
        }

        gxm_flush_state(context, linked, stream_starts);

        wire = gxm_wireframe_indices(primType, indexType, indexData, indexCount, &wire_count);
        if (wire) {
            uint32_t wire_offset;

            gxm_arena_reserve_offset(&context->index_arena, 4);
            wire_offset = gxm_arena_upload(&context->index_arena, GL_ELEMENT_ARRAY_BUFFER, wire,
                                           wire_count * 4, 4);
            free(wire);
            g_gl->glDrawElements(GL_LINES, (GLsizei)wire_count, GL_UNSIGNED_INT,
                                 (const void *)(uintptr_t)wire_offset);
            g_profile_draws++;
            g_profile_vertices += indexCount;
            g_profile_draw_ns += gxm_time_ns() - draw_started;
        }
        return 0;
    }

    vertex_uniform_size = gxm_build_uniforms(context, context->vertex_program->programId, true);
    fragment_uniform_size = gxm_build_uniforms(context, context->fragment_program->programId, false);

    gxm_uniform_trace(context, indexCount);

    gxm_fill_draw_state(context, program, primType, indexType, instances, &state);

    if (getenv("VITA2NATIVE_GXM_VERT_TRACE")) {
        static int blend_frame = -2;

        if (blend_frame == -2) {
            const char *value = getenv("VITA2NATIVE_GXM_VERT_TRACE");
            blend_frame = value ? atoi(value) : -1;
        }
        if (blend_frame >= 0 && g_gxm_frame == (uint64_t)blend_frame &&
            context->fragment_program) {
            const SceGxmBlendInfo *b = &context->fragment_program->blendInfo;

            LOG("  draw count=%u mask=%x func=%d/%d src=%d/%d dst=%d/%d mask_update=%d frag=%08x vsh=%08x",
                indexCount, b->colorMask, (int)b->colorFunc, (int)b->alphaFunc, (int)b->colorSrc,
                (int)b->colorDst, (int)b->alphaSrc, (int)b->alphaDst,
                (int)context->fragment_program->mask_update,
                context->fragment_program->shader_hash,
                context->vertex_program ? context->vertex_program->shader_hash : 0);
            {
                const float *fu = (const float *)context->fragment_block_data;

                if (fu)
                    LOG("    frag uniforms: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                        fu[0], fu[1], fu[2], fu[3], fu[4], fu[5], fu[6], fu[7]);
            }
            {
                const float *vu = (const float *)context->vertex_block_data;

                if (vu) {
                    for (unsigned int row = 0; row < 9; row++)
                        LOG("    vert uniform %u: %.4f %.4f %.4f %.4f", row, vu[row * 4 + 0],
                            vu[row * 4 + 1], vu[row * 4 + 2], vu[row * 4 + 3]);
                    LOG("    vert raw: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                        "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                        "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                        ((const unsigned *)vu)[0], ((const unsigned *)vu)[1],
                        ((const unsigned *)vu)[2], ((const unsigned *)vu)[3],
                        ((const unsigned *)vu)[4], ((const unsigned *)vu)[5],
                        ((const unsigned *)vu)[6], ((const unsigned *)vu)[7],
                        ((const unsigned *)vu)[8], ((const unsigned *)vu)[9],
                        ((const unsigned *)vu)[10], ((const unsigned *)vu)[11],
                        ((const unsigned *)vu)[12], ((const unsigned *)vu)[13],
                        ((const unsigned *)vu)[14], ((const unsigned *)vu)[15],
                        ((const unsigned *)vu)[16], ((const unsigned *)vu)[17],
                        ((const unsigned *)vu)[18], ((const unsigned *)vu)[19],
                        ((const unsigned *)vu)[20], ((const unsigned *)vu)[21],
                        ((const unsigned *)vu)[22], ((const unsigned *)vu)[23],
                        ((const unsigned *)vu)[24], ((const unsigned *)vu)[25],
                        ((const unsigned *)vu)[26], ((const unsigned *)vu)[27],
                        ((const unsigned *)vu)[28], ((const unsigned *)vu)[29],
                        ((const unsigned *)vu)[30], ((const unsigned *)vu)[31],
                        ((const unsigned *)vu)[32], ((const unsigned *)vu)[33],
                        ((const unsigned *)vu)[34], ((const unsigned *)vu)[35]);
                }
            }
        }
    }

    if (getenv("VITA2NATIVE_BATCH_DEBUG")) {
        static bool logged;

        if (!logged) {
            logged = true;
            LOG("draw surfaces: color=%p depth/stencil=%p", (const void *)context->color_surface,
                (const void *)context->ds_surface);
        }
    }

    can_merge = gxm_batch_matches(context, &state, vertex_uniform_size, fragment_uniform_size);
    if (getenv("VITA2NATIVE_NO_BATCH"))
        can_merge = false;
    if (can_merge) {
        const char *limit = getenv("VITA2NATIVE_BATCH_LIMIT");

        if (limit && context->batch.draws >= (uint32_t)atoi(limit))
            can_merge = false;
    }

    /* A 16-bit index buffer cannot address vertices past 65535. */
    if (can_merge && index_size == 2 &&
        context->batch.vertex_count + max_index + 1 > 0xFFFFu)
        can_merge = false;

    /* Appending must not overflow an arena; growing it would discard the
       batch's earlier vertices. */
    if (can_merge) {
        for (unsigned int i = 0; i < context->vertex_program->streamCount &&
                                 i < SCE_GXM_MAX_VERTEX_STREAMS;
             i++) {
            const SceGxmVertexStream *s = &context->vertex_program->streams[i];

            if (!s->stride)
                continue;
            if (!gxm_arena_fits(&context->stream_arena[i], (max_index + 1) * s->stride,
                                s->stride))
                can_merge = false;
        }
        if (!gxm_arena_fits(&context->index_arena, (uint32_t)(indexCount * index_size),
                            (uint32_t)index_size))
            can_merge = false;
    }

    if (!can_merge) {
        gxm_batch_flush(context);

        for (unsigned int i = 0; i < context->vertex_program->streamCount &&
                                 i < SCE_GXM_MAX_VERTEX_STREAMS;
             i++) {
            const SceGxmVertexStream *s = &context->vertex_program->streams[i];

            if (!s->stride)
                continue;
            gxm_arena_reserve(&context->stream_arena[i], GL_ARRAY_BUFFER,
                              (max_index + 1) * s->stride, s->stride);
        }
        gxm_arena_reserve(&context->index_arena, GL_ELEMENT_ARRAY_BUFFER,
                          (uint32_t)(indexCount * index_size), (uint32_t)index_size);

        gxm_batch_begin(context, linked, &state, vertex_uniform_size, fragment_uniform_size,
                        gl_index_type, gl_primitive, (uint32_t)index_size);
    }

    if (!gxm_batch_append(context, max_index, indexData, indexCount, (uint32_t)index_size)) {
        gxm_batch_flush(context);
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    }

    if (getenv("VITA2NATIVE_TRACE_BATCH")) {
        static unsigned int traced;

        if (traced < 24) {
            const SceGxmVertexProgram *tvp = context->vertex_program;
            uint32_t i0 = indexCount > 0 ? gxm_read_index(indexData, indexType, 0) : 0;
            uint32_t i1 = indexCount > 1 ? gxm_read_index(indexData, indexType, 1) : 0;
            const uint8_t *vd = tvp && tvp->streamCount ? context->streams[0].data : NULL;

            LOG("batch draw %u: prog=%u max=%u count=%u rebase=%u stream0=%p stride0=%u "
                "offset0=%u idx=%u,%u v0=%08x,%08x,%08x,%08x,%08x",
                traced, program, max_index, indexCount, context->batch.vertex_count - (max_index + 1),
                vd, tvp && tvp->streamCount ? tvp->streams[0].stride : 0,
                tvp && tvp->attributeCount ? tvp->attributes[0].offset : 0, i0, i1,
                vd ? ((const uint32_t *)vd)[0] : 0, vd ? ((const uint32_t *)vd)[1] : 0,
                vd ? ((const uint32_t *)vd)[2] : 0, vd ? ((const uint32_t *)vd)[3] : 0,
                vd ? ((const uint32_t *)vd)[4] : 0);
            traced++;
        }
    }

    gxm_gl_check("draw");
    g_profile_draw_ns += gxm_time_ns() - draw_started;

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
    /* GXM's built-in mask-update fragment program has no GXP of its own: it
       exists to drive the stencil write with the colour mask off. GL still
       needs some fragment shader attached to link and draw. */
    static const char mask_update_fragment[] = "#version 310 es\n"
                                               "void main() {}\n";
    SceGxmFragmentProgram *fp = calloc(1, sizeof(*fp));

    (void)shaderPatcher;
    if (!fp)
        return SCE_KERNEL_ERROR_NO_MEMORY;

    fp->refcount = 1;
    fp->mask_update = true;
    fp->shader_hash = 0x4D41534Bu; /* "MASK": never collides with a GXP hash */
    fp->blendInfo = (SceGxmBlendInfo){
        SCE_GXM_COLOR_MASK_NONE, SCE_GXM_BLEND_FUNC_NONE, SCE_GXM_BLEND_FUNC_NONE,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
        SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ZERO,
    };

    if (g_gl) {
        bool compiled = false;

        fp->shader = gxm_create_compiled_shader(GL_FRAGMENT_SHADER, "mask-update",
                                                mask_update_fragment, &compiled);
        if (!compiled) {
            g_gl->glDeleteShader(fp->shader);
            free(fp);
            return SCE_GXM_ERROR_INVALID_VALUE;
        }
    }

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

    if (g_current_context)
        gxm_batch_flush(g_current_context);

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

    if (g_current_context)
        gxm_batch_flush(g_current_context);

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

    if (g_current_context)
        gxm_batch_flush(g_current_context);

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

    host_gpu_thread_enter(g_device);

    if (!params || !context)
        return SCE_GXM_ERROR_INVALID_POINTER;

    if (params->hostMem && params->hostMemSize >= sizeof(SceGxmContext)) {
        ctx = params->hostMem;
        memset(ctx, 0, sizeof(*ctx));
    } else {
        ctx = calloc(1, sizeof(*ctx));
        if (!ctx)
            return SCE_GXM_ERROR_OUT_OF_MEMORY;
        ctx->owns_memory = true;
    }

    gxm_context_init_defaults(ctx, true);
    if (!ctx->vertex_uniform_arena || !ctx->fragment_uniform_arena) {
        free(ctx->vertex_uniform_arena);
        free(ctx->fragment_uniform_arena);
        if (ctx->owns_memory)
            free(ctx);
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
    if (g_current_context == context)
        g_current_context = NULL;
    if (context->owns_memory)
        free(context);
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

/* Used by the system-app (live-area) path. There is no VSH process here, so
   this is the same initialisation as the regular entry point. */
EXPORT(SceGxmInternalForVsh, 0xA04F5FAC, int, sceGxmVshInitialize,
       const SceGxmInitializeParams *params)
{
    return sceGxmInitialize(params);
}

/*
 * Cross-process sync objects only exist between the shell and a title. There
 * is no shell process on the host, so the VSH-side entry points report that
 * the feature is unavailable instead of returning dangling handles.
 */
EXPORT(SceGxmInternalForVsh, 0x2A956594, int, sceGxmVshSyncObjectCreate, void)
{
    return SCE_GXM_ERROR_UNSUPPORTED;
}

EXPORT(SceGxmInternalForVsh, 0xAD3FE572, int, sceGxmVshSyncObjectOpen, void)
{
    return SCE_GXM_ERROR_UNSUPPORTED;
}

EXPORT(SceGxmInternalForVsh, 0xBEB4F93D, int, sceGxmVshSyncObjectClose, void)
{
    return SCE_GXM_ERROR_UNSUPPORTED;
}

EXPORT(SceGxmInternalForVsh, 0x9B8FC52F, int, sceGxmVshSyncObjectDestroy, void)
{
    return SCE_GXM_ERROR_UNSUPPORTED;
}

DECLARE_LIBRARY(SceGxm, 0xf76b66bd);
DECLARE_LIBRARY(SceGxmInternalForVsh, 0xc98aeb79);

int SceGxm_init(HostGpuDevice device)
{
    g_device = device;
    gxm_color_tables_init();
    return 0;
}

int SceGxm_finish(void)
{
    if (g_gl)
        g_gl->glFinish();
    return 0;
}
