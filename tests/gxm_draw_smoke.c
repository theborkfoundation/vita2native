/*
 * End-to-end GXM draw test.
 *
 * This exercises the real SceGxm entry points with real GXP shaders and a real
 * GLES context (Mesa llvmpipe on a headless Linux box): shader patcher,
 * vertex program, attributes, default uniforms, a texture, a render target,
 * viewport/clip/cull state, a draw and the color-surface readback.
 *
 * Usage: gxm_draw_smoke <shader-dir>
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include <psp2/gxm.h>

#include "fs.h"
#include "gfx.h"
#include "gxm/util.h"
#include "modules/SceGxm.h"

#define WIDTH  64
#define HEIGHT 64

void log_print(const char *str)
{
    fprintf(stderr, "%s\n", str);
}

void log_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

void fatal_error(const char *message, const char *detail)
{
    fprintf(stderr, "fatal: %s: %s\n", message, detail);
    abort();
}

/*
 * GXM stores texture addresses as 30-bit words (address >> 2). On a 64-bit
 * test host, allocate texture data below 2 GiB so the packing round-trips, as
 * it does for guest memory on the real 32-bit target.
 */
static void *low_alloc(size_t size)
{
#ifdef MAP_32BIT
    void *pointer = mmap(NULL, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);

    if (pointer != MAP_FAILED) {
        memset(pointer, 0, size);
        return pointer;
    }
#endif
    return calloc(1, size);
}

static void *read_file(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    long length;
    void *data;

    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (length <= 0) {
        fclose(fp);
        return NULL;
    }

    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, fp) != (size_t)length) {
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *size = (size_t)length;
    return data;
}

typedef struct {
    SceGxmContext *context;
    SceGxmRenderTarget *target;
    SceGxmShaderPatcherId vertex_id;
    SceGxmShaderPatcherId fragment_id;
    SceGxmVertexProgram *vertex_program;
    SceGxmFragmentProgram *fragment_program;
    const SceGxmProgramParameter *wvp;
    SceGxmColorSurface color_surface;
    SceGxmDepthStencilSurface depth_surface;
    float *depth_values;
    uint32_t *packed_depth_stencil;
    uint8_t *stencil_values;
    uint32_t *pixels;
    const SceGxmProgramParameter *position;
    const SceGxmProgramParameter *texcoord;
    void *vertex_gxp;
    void *fragment_gxp;
} Scene;

static int setup_programs(Scene *scene, SceGxmShaderPatcher *patcher, const char *shader_dir)
{
    char path[1024];
    void *vertex_gxp;
    void *fragment_gxp;
    size_t vertex_size;
    size_t fragment_size;
    SceGxmVertexAttribute attributes[2];
    SceGxmVertexStream streams[2];
    snprintf(path, sizeof(path), "%s/texture_v.gxp", shader_dir);
    vertex_gxp = read_file(path, &vertex_size);
    snprintf(path, sizeof(path), "%s/texture_f.gxp", shader_dir);
    fragment_gxp = read_file(path, &fragment_size);
    if (!vertex_gxp || !fragment_gxp) {
        fprintf(stderr, "cannot read texture_v.gxp / texture_f.gxp from %s\n", shader_dir);
        return -1;
    }
    scene->vertex_gxp = vertex_gxp;
    scene->fragment_gxp = fragment_gxp;

    if (sceGxmProgramCheck((const SceGxmProgram *)vertex_gxp) != 0 ||
        sceGxmProgramCheck((const SceGxmProgram *)fragment_gxp) != 0) {
        fprintf(stderr, "GXP program check failed\n");
        return -1;
    }

    if (sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)vertex_gxp,
                                           &scene->vertex_id) != 0 ||
        sceGxmShaderPatcherRegisterProgram(patcher, (const SceGxmProgram *)fragment_gxp,
                                           &scene->fragment_id) != 0) {
        fprintf(stderr, "register program failed\n");
        return -1;
    }

    scene->position = sceGxmProgramFindParameterByName((const SceGxmProgram *)vertex_gxp,
                                                       "aPosition");
    scene->texcoord = sceGxmProgramFindParameterByName((const SceGxmProgram *)vertex_gxp,
                                                       "aTexcoord");
    scene->wvp = sceGxmProgramFindParameterByName((const SceGxmProgram *)vertex_gxp, "wvp");

    if (!scene->position || !scene->texcoord || !scene->wvp) {
        fprintf(stderr, "missing shader parameters (aPosition=%p aTexcoord=%p wvp=%p)\n",
                (const void *)scene->position, (const void *)scene->texcoord,
                (const void *)scene->wvp);
        return -1;
    }

    printf("parameters: aPosition reg=%u, aTexcoord reg=%u, wvp reg=%u count=%u\n",
           sceGxmProgramParameterGetResourceIndex(scene->position),
           sceGxmProgramParameterGetResourceIndex(scene->texcoord),
           sceGxmProgramParameterGetResourceIndex(scene->wvp),
           sceGxmProgramParameterGetArraySize(scene->wvp));

    attributes[0].streamIndex = 0;
    attributes[0].offset = 0;
    attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[0].componentCount = 4;
    attributes[0].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(scene->position);

    attributes[1].streamIndex = 1;
    attributes[1].offset = 0;
    attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
    attributes[1].componentCount = 2;
    attributes[1].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(scene->texcoord);

    streams[0].stride = 4 * sizeof(float);
    streams[0].indexSource = 0;
    streams[1].stride = 2 * sizeof(float);
    streams[1].indexSource = 0;

    if (sceGxmShaderPatcherCreateVertexProgram(patcher, scene->vertex_id, attributes, 2, streams,
                                               2, &scene->vertex_program) != 0) {
        fprintf(stderr, "create vertex program failed\n");
        return -1;
    }

    if (sceGxmShaderPatcherCreateFragmentProgram(
            patcher, scene->fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
            SCE_GXM_MULTISAMPLE_NONE, NULL, (const SceGxmProgram *)vertex_gxp,
            &scene->fragment_program) != 0) {
        fprintf(stderr, "create fragment program failed\n");
        return -1;
    }

    return 0;
}

static const float quad_vertices[4][4] = {
    { -1.0f, -1.0f, 0.0f, 1.0f },
    { 1.0f, -1.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f },
    { -1.0f, 1.0f, 0.0f, 1.0f },
};
static const uint16_t quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
static const float quad_uvs[4][2] = {
    { 0.0f, 1.0f },
    { 1.0f, 1.0f },
    { 1.0f, 0.0f },
    { 0.0f, 0.0f },
};

static void set_identity_wvp(Scene *scene)
{
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    void *buffer = NULL;

    assert(sceGxmReserveVertexDefaultUniformBuffer(scene->context, &buffer) == 0);
    assert(sceGxmSetUniformDataF(buffer, scene->wvp, 0, 16, identity) == 0);
}

static void draw_quad(Scene *scene, const SceGxmTexture *texture)
{
    sceGxmSetVertexProgram(scene->context, scene->vertex_program);
    sceGxmSetFragmentProgram(scene->context, scene->fragment_program);
    assert(sceGxmSetFragmentTexture(scene->context, 0, texture) == 0);
    assert(sceGxmSetVertexStream(scene->context, 0, quad_vertices) == 0);
    assert(sceGxmSetVertexStream(scene->context, 1, quad_uvs) == 0);
    set_identity_wvp(scene);
    assert(sceGxmDraw(scene->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                      quad_indices, 6) == 0);
}

static void draw_quad_at_z(Scene *scene, const SceGxmTexture *texture, float z)
{
    float vertices[4][4];

    for (int i = 0; i < 4; i++) {
        vertices[i][0] = quad_vertices[i][0];
        vertices[i][1] = quad_vertices[i][1];
        vertices[i][2] = z;
        vertices[i][3] = quad_vertices[i][3];
    }

    sceGxmSetVertexProgram(scene->context, scene->vertex_program);
    sceGxmSetFragmentProgram(scene->context, scene->fragment_program);
    assert(sceGxmSetFragmentTexture(scene->context, 0, texture) == 0);
    assert(sceGxmSetVertexStream(scene->context, 0, vertices) == 0);
    assert(sceGxmSetVertexStream(scene->context, 1, quad_uvs) == 0);
    set_identity_wvp(scene);
    assert(sceGxmDraw(scene->context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                      quad_indices, 6) == 0);
}

static void set_solid_color(uint8_t *solid_pixels, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (uint32_t i = 0; i < 8 * 8; i++) {
        solid_pixels[i * 4 + 0] = r;
        solid_pixels[i * 4 + 1] = g;
        solid_pixels[i * 4 + 2] = b;
        solid_pixels[i * 4 + 3] = a;
    }
}

static uint32_t *begin_frame(Scene *scene)
{
    memset(scene->pixels, 0, WIDTH * HEIGHT * 4);
    assert(sceGxmBeginScene(scene->context, 0, scene->target, NULL, NULL, NULL,
                            &scene->color_surface, &scene->depth_surface) == 0);
    sceGxmSetDefaultRegionClipAndViewport(scene->context, WIDTH, HEIGHT);
    sceGxmSetRegionClip(scene->context, SCE_GXM_REGION_CLIP_NONE, 0, 0, WIDTH - 1, HEIGHT - 1);
    sceGxmSetCullMode(scene->context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontPolygonMode(scene->context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    return scene->pixels;
}

static void begin_frame_with_surface(Scene *scene, SceGxmColorSurface *surface)
{
    assert(sceGxmBeginScene(scene->context, 0, scene->target, NULL, NULL, NULL, surface, NULL) == 0);
    sceGxmSetDefaultRegionClipAndViewport(scene->context, WIDTH, HEIGHT);
    sceGxmSetRegionClip(scene->context, SCE_GXM_REGION_CLIP_NONE, 0, 0, WIDTH - 1, HEIGHT - 1);
    sceGxmSetCullMode(scene->context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontPolygonMode(scene->context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
}

static void end_frame(Scene *scene)
{
    assert(sceGxmEndScene(scene->context, NULL, NULL) == 0);
    sceGxmFinish(scene->context);
}

static bool pixel_is(const uint32_t *pixels, int x, int y, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a)
{
    const uint8_t *p = (const uint8_t *)&pixels[y * WIDTH + x];

    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

int main(int argc, char **argv)
{
    const char *shader_dir = argc > 1 ? argv[1] : "external/Vita3K/tools/native-tool/src/shaders";
    HostGpuConfig gpu_config = { WIDTH, HEIGHT, true };
    HostGpuDevice device;
    Scene scene;
    SceGxmInitializeParams init_params;
    SceGxmContextParams context_params;
    SceGxmShaderPatcherParams patcher_params;
    SceGxmShaderPatcher *patcher = NULL;
    SceGxmRenderTargetParams target_params;
    SceGxmTexture solid_texture;
    SceGxmTexture quad_texture;
    uint8_t *solid_pixels;
    uint8_t *quad_pixels;
    void *context_mem;
    void *deferred_mem = NULL;
    SceGxmContext *deferred = NULL;
    SceGxmCommandList command_list;
    int result = 1;

    memset(&scene, 0, sizeof(scene));
    memset(&context_params, 0, sizeof(context_params));
    memset(&patcher_params, 0, sizeof(patcher_params));
    memset(&target_params, 0, sizeof(target_params));
    memset(&init_params, 0, sizeof(init_params));

    device = host_gpu_create(&gpu_config);
    if (!device)
        return 1;

    /* Enable the on-disk GLSL cache under a stable path so a second run hits it. */
    {
        char cache_root[] = "/tmp/v2n-gxm-shader-cache";
        if (mkdir(cache_root, 0755) == 0 || errno == EEXIST)
            host_fs_init(cache_root, NULL);
    }

    printf("GPU: %s\n", host_gpu_description(device));
    if (host_gpu_backend(device) != HOST_GPU_BACKEND_GLES) {
        printf("gxm_draw_smoke: skipped (no GLES backend)\n");
        host_gpu_destroy(device);
        return 0;
    }

    assert(SceGxm_init(device) == 0);
    assert(sceGxmInitialize(&init_params) == 0);

    context_mem = calloc(1, 1024 * 1024);
    assert(context_mem);

    solid_pixels = low_alloc(8 * 8 * 4);
    quad_pixels = low_alloc(8 * 8 * 4);
    assert(solid_pixels && quad_pixels);

    context_params.hostMem = context_mem;
    context_params.hostMemSize = 1024 * 1024;
    assert(sceGxmCreateContext(&context_params, &scene.context) == 0);

    assert(sceGxmShaderPatcherCreate(&patcher_params, &patcher) == 0);
    if (setup_programs(&scene, patcher, shader_dir) != 0)
        goto done;

    target_params.flags = 0;
    target_params.width = WIDTH;
    target_params.height = HEIGHT;
    target_params.scenesPerFrame = 1;
    target_params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    target_params.driverMemBlock = 0;
    assert(sceGxmCreateRenderTarget(&target_params, &scene.target) == 0);

    scene.pixels = calloc(WIDTH * HEIGHT, 4);
    assert(scene.pixels);
    assert(sceGxmColorSurfaceInit(&scene.color_surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                  SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                  SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, WIDTH, HEIGHT, WIDTH,
                                  scene.pixels) == 0);
    assert(sceGxmDepthStencilSurfaceInitDisabled(&scene.depth_surface) == 0);

    scene.depth_values = calloc((size_t)WIDTH * HEIGHT, sizeof(float));
    scene.packed_depth_stencil = calloc((size_t)WIDTH * HEIGHT, sizeof(uint32_t));
    scene.stencil_values = calloc((size_t)WIDTH * HEIGHT, 1);
    assert(scene.depth_values && scene.packed_depth_stencil && scene.stencil_values);

    /* Solid texture: bytes R,G,B,A for A8B8G8R8. Linear rows are padded to 8
     * pixels by GXM, so fill an 8x8 block. */
    for (uint32_t i = 0; i < 8 * 8; i++) {
        solid_pixels[i * 4 + 0] = 10;
        solid_pixels[i * 4 + 1] = 20;
        solid_pixels[i * 4 + 2] = 30;
        solid_pixels[i * 4 + 3] = 40;
    }
    assert(sceGxmTextureInitLinear(&solid_texture, solid_pixels,
                                   SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 2, 2, 0) == 0);
    sceGxmTextureSetMinFilter(&solid_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetMagFilter(&solid_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    sceGxmTextureSetUAddrMode(&solid_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&solid_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);

    /* 2x2 checkerboard sampled with NEAREST: four distinct colours. */
    {
        static const uint8_t colors[4][4] = {
            { 255, 0, 0, 255 },
            { 0, 255, 0, 255 },
            { 0, 0, 255, 255 },
            { 255, 255, 255, 255 },
        };
        for (uint32_t y = 0; y < 8; y++) {
            for (uint32_t x = 0; x < 8; x++)
                memcpy(quad_pixels + (y * 8 + x) * 4, colors[(y & 1) * 2 + (x & 1)], 4);
        }
        assert(sceGxmTextureInitLinear(&quad_texture, quad_pixels,
                                       SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, 2, 2, 0) == 0);
        sceGxmTextureSetMinFilter(&quad_texture, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&quad_texture, SCE_GXM_TEXTURE_FILTER_POINT);
    }

    /* 1: full-surface textured quad. */
    begin_frame(&scene);
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);

    {
        uint32_t matched = 0;

        for (int y = 0; y < HEIGHT; y++) {
            for (int x = 0; x < WIDTH; x++) {
                if (pixel_is(scene.pixels, x, y, 10, 20, 30, 40))
                    matched++;
            }
        }

        if (matched != WIDTH * HEIGHT) {
            fprintf(stderr, "frame 1: %u/%u pixels matched\n", matched, WIDTH * HEIGHT);
            for (int y = 0; y < HEIGHT; y += 16) {
                fprintf(stderr, "  row %2d:", y);
                for (int x = 0; x < WIDTH; x += 16) {
                    const uint8_t *p = (const uint8_t *)&scene.pixels[y * WIDTH + x];
                    fprintf(stderr, " %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
                }
                fprintf(stderr, "\n");
            }
            goto done;
        }
    }
    printf("solid textured quad: ok\n");

    /* 2: region clip restricts the second draw to the top-left quadrant. */
    begin_frame(&scene);
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);
    begin_frame(&scene);
    sceGxmSetRegionClip(scene.context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0, WIDTH / 2 - 1,
                        HEIGHT / 2 - 1);
    for (uint32_t i = 0; i < 8 * 8; i++) {
        solid_pixels[i * 4 + 0] = 200;
        solid_pixels[i * 4 + 1] = 50;
        solid_pixels[i * 4 + 2] = 25;
        solid_pixels[i * 4 + 3] = 255;
    }
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);

    if (!pixel_is(scene.pixels, 4, 4, 200, 50, 25, 255) ||
        !pixel_is(scene.pixels, WIDTH - 4, HEIGHT - 4, 10, 20, 30, 40)) {
        fprintf(stderr, "region clip failed: corner (%02x %02x %02x), far (%02x %02x %02x)\n",
                ((uint8_t *)&scene.pixels[0])[0], ((uint8_t *)&scene.pixels[0])[1],
                ((uint8_t *)&scene.pixels[0])[2],
                ((uint8_t *)&scene.pixels[(HEIGHT - 1) * WIDTH + WIDTH - 1])[0],
                ((uint8_t *)&scene.pixels[(HEIGHT - 1) * WIDTH + WIDTH - 1])[1],
                ((uint8_t *)&scene.pixels[(HEIGHT - 1) * WIDTH + WIDTH - 1])[2]);
        goto done;
    }
    printf("region clip: ok\n");

    /* 2b: the colour surface clip restricts the writeback area. */
    memset(scene.pixels, 0, WIDTH * HEIGHT * 4);
    set_solid_color(solid_pixels, 255, 0, 0, 255);
    begin_frame_with_surface(&scene, &scene.color_surface);
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);

    set_solid_color(solid_pixels, 0, 0, 255, 255);
    sceGxmColorSurfaceSetClip(&scene.color_surface, 0, 0, WIDTH / 2 - 1, HEIGHT / 2 - 1);
    begin_frame_with_surface(&scene, &scene.color_surface);
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);
    sceGxmColorSurfaceSetClip(&scene.color_surface, 0, 0, WIDTH - 1, HEIGHT - 1);

    if (!pixel_is(scene.pixels, 4, 4, 0, 0, 255, 255) ||
        !pixel_is(scene.pixels, WIDTH - 4, HEIGHT - 4, 255, 0, 0, 255)) {
        fprintf(stderr, "colour surface clip failed\n");
        goto done;
    }
    printf("colour surface clip: ok\n");

    /* 3: 2x2 nearest-sampled texture produces at least four distinct colours. */
    begin_frame(&scene);
    draw_quad(&scene, &quad_texture);
    end_frame(&scene);
    {
        uint32_t seen[8] = { 0 };
        uint32_t count = 0;

        for (int y = 0; y < HEIGHT; y += 4) {
            for (int x = 0; x < WIDTH; x += 4) {
                uint32_t color = scene.pixels[y * WIDTH + x];
                bool found = false;

                for (uint32_t i = 0; i < count; i++)
                    found = found || seen[i] == color;
                if (!found && count < 8)
                    seen[count++] = color;
            }
        }

        if (count < 4) {
            fprintf(stderr, "texture sampling produced only %u distinct colours\n", count);
            goto done;
        }
        printf("texture sampling: %u distinct colours\n", count);
    }

    /* 4: line polygon mode emits fewer pixels than a fill. */
    begin_frame(&scene);
    sceGxmSetFrontPolygonMode(scene.context, SCE_GXM_POLYGON_MODE_LINE);
    for (uint32_t i = 0; i < 8 * 8; i++) {
        solid_pixels[i * 4 + 0] = 0;
        solid_pixels[i * 4 + 1] = 255;
        solid_pixels[i * 4 + 2] = 0;
        solid_pixels[i * 4 + 3] = 255;
    }
    draw_quad(&scene, &solid_texture);
    end_frame(&scene);
    {
        uint32_t green = 0;

        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            const uint8_t *p = (const uint8_t *)&scene.pixels[i];
            if (p[1] == 255 && p[0] == 0 && p[2] == 0)
                green++;
        }

        if (green == 0 || green > (uint32_t)(WIDTH * HEIGHT / 2)) {
            fprintf(stderr, "wireframe draw covered %u pixels\n", green);
            goto done;
        }
        printf("wireframe: %u pixels\n", green);
    }

    /* 5: depth test plus force store / force load of the depth surface. */
    assert(sceGxmDepthStencilSurfaceInit(&scene.depth_surface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32,
                                         SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, WIDTH,
                                         scene.depth_values, NULL) == 0);
    if (sceGxmDepthStencilSurfaceGetFormat(&scene.depth_surface) !=
            SCE_GXM_DEPTH_STENCIL_FORMAT_DF32 ||
        sceGxmDepthStencilSurfaceGetStrideInSamples(&scene.depth_surface) != WIDTH ||
        !sceGxmDepthStencilSurfaceIsEnabled(&scene.depth_surface)) {
        fprintf(stderr, "depth surface queries failed\n");
        goto done;
    }

    sceGxmDepthStencilSurfaceSetForceStoreMode(&scene.depth_surface,
                                               SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    sceGxmDepthStencilSurfaceSetForceLoadMode(&scene.depth_surface,
                                              SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);

    set_solid_color(solid_pixels, 255, 0, 0, 255);
    begin_frame(&scene);
    draw_quad_at_z(&scene, &solid_texture, 0.25f);
    set_solid_color(solid_pixels, 0, 255, 0, 255);
    draw_quad_at_z(&scene, &solid_texture, 0.75f);
    end_frame(&scene);

    if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 255, 0, 0, 255)) {
        fprintf(stderr, "depth test did not keep the near fragment\n");
        goto done;
    }
    if (!(scene.depth_values[0] > 0.60f && scene.depth_values[0] < 0.65f)) {
        fprintf(stderr, "stored depth is %f, expected ~0.625\n", scene.depth_values[0]);
        goto done;
    }
    printf("depth test / store: ok\n");

    /* 5b: force load makes the far fragment fail against the stored depth. */
    sceGxmDepthStencilSurfaceSetForceLoadMode(&scene.depth_surface,
                                              SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
    set_solid_color(solid_pixels, 0, 255, 0, 255);
    begin_frame(&scene);
    draw_quad_at_z(&scene, &solid_texture, 0.75f);
    end_frame(&scene);

    if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 255, 0, 0, 255)) {
        fprintf(stderr, "force-loaded depth did not reject the far fragment\n");
        goto done;
    }
    printf("depth force load: ok\n");

    /* 5c: without force load the depth surface starts from the background. */
    sceGxmDepthStencilSurfaceSetForceLoadMode(&scene.depth_surface,
                                              SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
    begin_frame(&scene);
    draw_quad_at_z(&scene, &solid_texture, 0.75f);
    end_frame(&scene);

    if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 0, 255, 0, 255)) {
        fprintf(stderr, "background depth clear left the old fragment\n");
        goto done;
    }
    printf("depth background clear: ok\n");

    /* 6: packed S8D24 surface stores both depth and stencil. */
    memset(scene.packed_depth_stencil, 0, (size_t)WIDTH * HEIGHT * sizeof(uint32_t));
    assert(sceGxmDepthStencilSurfaceInit(&scene.depth_surface,
                                         SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                         SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, WIDTH,
                                         scene.packed_depth_stencil, NULL) == 0);
    if (sceGxmDepthStencilSurfaceGetFormat(&scene.depth_surface) !=
        SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24) {
        fprintf(stderr, "S8D24 format query failed (0x%08x)\n",
                sceGxmDepthStencilSurfaceGetFormat(&scene.depth_surface));
        goto done;
    }
    sceGxmDepthStencilSurfaceSetForceStoreMode(&scene.depth_surface,
                                               SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
    sceGxmSetFrontStencilFunc(scene.context, SCE_GXM_STENCIL_FUNC_ALWAYS,
                              SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_REPLACE, 0xFF, 0xFF);
    sceGxmSetFrontStencilRef(scene.context, 7);

    set_solid_color(solid_pixels, 0, 0, 255, 255);
    begin_frame(&scene);
    draw_quad_at_z(&scene, &solid_texture, 0.5f);
    end_frame(&scene);

    if ((scene.packed_depth_stencil[0] & 0xFFu) != 7u) {
        fprintf(stderr, "stencil store returned %u, expected 7\n",
                scene.packed_depth_stencil[0] & 0xFFu);
        goto done;
    }
    if ((scene.packed_depth_stencil[0] >> 8) < 0x800000u) {
        fprintf(stderr, "packed depth store returned 0x%06x\n",
                scene.packed_depth_stencil[0] >> 8);
        goto done;
    }
    printf("packed S8D24 depth/stencil: ok\n");

    /* 6b: tiled colour surface round-trips through a tiled texture. */
    {
        SceGxmColorSurface tiled_surface;
        SceGxmTexture tiled_texture;
        uint8_t *tiled_pixels = low_alloc(WIDTH * HEIGHT * 4);

        assert(tiled_pixels);
        assert(sceGxmColorSurfaceInit(&tiled_surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                      SCE_GXM_COLOR_SURFACE_TILED,
                                      SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                      SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, WIDTH, HEIGHT, WIDTH,
                                      tiled_pixels) == 0);

        set_solid_color(solid_pixels, 12, 34, 56, 78);
        begin_frame_with_surface(&scene, &tiled_surface);
        draw_quad(&scene, &solid_texture);
        end_frame(&scene);

        assert(sceGxmTextureInitTiled(&tiled_texture, tiled_pixels,
                                      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, WIDTH, HEIGHT, 0) == 0);
        sceGxmTextureSetMinFilter(&tiled_texture, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&tiled_texture, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&tiled_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&tiled_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);

        begin_frame(&scene);
        draw_quad(&scene, &tiled_texture);
        end_frame(&scene);

        if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 12, 34, 56, 78)) {
            const uint8_t *p = (const uint8_t *)&scene.pixels[(HEIGHT / 2) * WIDTH + WIDTH / 2];
            fprintf(stderr, "tiled surface round-trip gave %02x %02x %02x %02x\n", p[0], p[1],
                    p[2], p[3]);
            goto done;
        }
        printf("tiled colour surface: ok\n");
    }

    /* 6b2: swizzled colour surface round-trips through a swizzled texture. */
    {
        SceGxmColorSurface swizzled_surface;
        SceGxmTexture swizzled_texture;
        uint8_t *swizzled_pixels = low_alloc(WIDTH * HEIGHT * 4);

        assert(swizzled_pixels);
        assert(sceGxmColorSurfaceInit(&swizzled_surface, SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                      SCE_GXM_COLOR_SURFACE_SWIZZLED,
                                      SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                      SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, WIDTH, HEIGHT, WIDTH,
                                      swizzled_pixels) == 0);

        set_solid_color(solid_pixels, 5, 200, 77, 255);
        begin_frame_with_surface(&scene, &swizzled_surface);
        draw_quad(&scene, &solid_texture);
        end_frame(&scene);

        assert(sceGxmTextureInitSwizzled(&swizzled_texture, swizzled_pixels,
                                         SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, WIDTH, HEIGHT,
                                         0) == 0);
        sceGxmTextureSetMinFilter(&swizzled_texture, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetMagFilter(&swizzled_texture, SCE_GXM_TEXTURE_FILTER_POINT);
        sceGxmTextureSetUAddrMode(&swizzled_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&swizzled_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);

        begin_frame(&scene);
        draw_quad(&scene, &swizzled_texture);
        end_frame(&scene);

        if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 5, 200, 77, 255)) {
            const uint8_t *p = (const uint8_t *)&scene.pixels[(HEIGHT / 2) * WIDTH + WIDTH / 2];
            fprintf(stderr, "swizzled surface round-trip gave %02x %02x %02x %02x\n", p[0], p[1],
                    p[2], p[3]);
            goto done;
        }
        printf("swizzled colour surface: ok\n");
    }

    /* 6c: 4x multisampled render target resolves into the colour surface. */
    {
        SceGxmRenderTarget *msaa_target = NULL;
        SceGxmRenderTarget *saved_target = scene.target;
        SceGxmRenderTargetParams msaa_params = target_params;

        msaa_params.multisampleMode = SCE_GXM_MULTISAMPLE_4X;
        if (sceGxmCreateRenderTarget(&msaa_params, &msaa_target) != 0) {
            fprintf(stderr, "could not create a multisampled render target\n");
            goto done;
        }

        scene.target = msaa_target;
        set_solid_color(solid_pixels, 90, 180, 30, 255);
        begin_frame(&scene);
        draw_quad(&scene, &solid_texture);
        end_frame(&scene);
        scene.target = saved_target;
        sceGxmDestroyRenderTarget(msaa_target);

        if (!pixel_is(scene.pixels, WIDTH / 2, HEIGHT / 2, 90, 180, 30, 255)) {
            fprintf(stderr, "multisampled resolve did not reach the colour surface\n");
            goto done;
        }
        printf("4x multisample target: ok\n");
    }

    /* 7: a deferred context records a draw that the immediate context replays. */
    {
        SceGxmDeferredContextParams deferred_params;
        void *uniform = NULL;
        const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        memset(&deferred_params, 0, sizeof(deferred_params));
        memset(&command_list, 0, sizeof(command_list));
        deferred_mem = calloc(1, 1 << 20);
        assert(deferred_mem);
        deferred_params.hostMem = deferred_mem;
        deferred_params.hostMemSize = 1 << 20;
        deferred_params.vdmRingBufferMem = deferred_mem;
        deferred_params.vdmRingBufferMemSize = 1 << 16;
        assert(sceGxmCreateDeferredContext(&deferred_params, &deferred) == 0);

        /* Draws outside a command list must be rejected. */
        if (sceGxmDraw(deferred, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                       quad_indices, 6) != SCE_GXM_ERROR_NOT_WITHIN_COMMAND_LIST) {
            fprintf(stderr, "deferred draw outside a command list was accepted\n");
            goto done;
        }

        assert(sceGxmBeginCommandList(deferred) == 0);
        sceGxmSetVertexProgram(deferred, scene.vertex_program);
        sceGxmSetFragmentProgram(deferred, scene.fragment_program);
        assert(sceGxmSetFragmentTexture(deferred, 0, &solid_texture) == 0);
        assert(sceGxmSetVertexStream(deferred, 0, quad_vertices) == 0);
        assert(sceGxmSetVertexStream(deferred, 1, quad_uvs) == 0);
        assert(sceGxmReserveVertexDefaultUniformBuffer(deferred, &uniform) == 0);
        assert(sceGxmSetUniformDataF(uniform, scene.wvp, 0, 16, identity) == 0);
        sceGxmSetFrontPolygonMode(deferred, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
        assert(sceGxmDraw(deferred, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                          quad_indices, 6) == 0);
        assert(sceGxmEndCommandList(deferred, &command_list) == 0);

        /* Executing a command list on a deferred context is invalid. */
        if (sceGxmExecuteCommandList(deferred, &command_list) != SCE_GXM_ERROR_INVALID_VALUE) {
            fprintf(stderr, "deferred ExecuteCommandList was accepted\n");
            goto done;
        }

        set_solid_color(solid_pixels, 11, 22, 33, 255);
        begin_frame(&scene);
        /* The recorded state must win over the immediate context's line mode. */
        sceGxmSetFrontPolygonMode(scene.context, SCE_GXM_POLYGON_MODE_LINE);
        assert(sceGxmExecuteCommandList(scene.context, &command_list) == 0);
        end_frame(&scene);

        {
            uint32_t matched = 0;

            for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                    if (pixel_is(scene.pixels, x, y, 11, 22, 33, 255))
                        matched++;
                }
            }

            if (matched != (uint32_t)(WIDTH * HEIGHT)) {
                fprintf(stderr, "replayed command list covered %u/%u pixels\n", matched,
                        WIDTH * HEIGHT);
                goto done;
            }
        }
        printf("deferred command list: ok\n");
    }

    printf("gxm_draw_smoke: ok\n");
    result = 0;

done:
    if (scene.pixels)
        free(scene.pixels);
    free(scene.depth_values);
    free(scene.packed_depth_stencil);
    free(scene.stencil_values);
    if (deferred)
        sceGxmDestroyDeferredContext(deferred);
    free(deferred_mem);
    if (scene.target)
        sceGxmDestroyRenderTarget(scene.target);
    if (scene.fragment_program)
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, scene.fragment_program);
    if (scene.vertex_program)
        sceGxmShaderPatcherReleaseVertexProgram(patcher, scene.vertex_program);
    if (patcher) {
        if (scene.vertex_id)
            sceGxmShaderPatcherUnregisterProgram(patcher, scene.vertex_id);
        if (scene.fragment_id)
            sceGxmShaderPatcherUnregisterProgram(patcher, scene.fragment_id);
        sceGxmShaderPatcherDestroy(patcher);
    }
    if (scene.context)
        sceGxmDestroyContext(scene.context);
    free(scene.vertex_gxp);
    free(scene.fragment_gxp);
    free(context_mem);
    sceGxmTerminate();
    host_gpu_destroy(device);
    return result;
}
