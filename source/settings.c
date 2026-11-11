/*
 * Every VITA2NATIVE_* environment knob, one cached lookup, and a startup check
 * for misspelt names. See include/settings.h for the contract.
 */

#include "settings.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

#define FLAG(name, text) {name, SETTING_FLAG, text}
#define INT(name, text) {name, SETTING_INT, text}
#define FLOAT(name, text) {name, SETTING_FLOAT, text}
#define STR(name, text) {name, SETTING_STRING, text}

static const Setting g_settings[] = {
    /* Crash handling and guest-memory dumps. */
    FLAG("VITA2NATIVE_CRASH_CORE", "write a core file when the guest faults (needs ulimit -c)"),
    STR("VITA2NATIVE_CRASH_CODE_DUMP", "write 4 MB of the faulting guest mapping to this file"),
    STR("VITA2NATIVE_CRASH_DUMP", "image offsets (comma-separated) to dump as words at a signal"),
    INT("VITA2NATIVE_CRASH_DUMP_ARRAY", "stack offset of a guest array pointer to print at a signal"),
    INT("VITA2NATIVE_CRASH_DUMP_VEC", "stack offset of a guest vector pointer to print at a signal"),
    FLAG("VITA2NATIVE_DUMP_MAPS", "log /proc/self/maps at startup and at a guest signal"),
    STR("VITA2NATIVE_DUMP_MAIN_CODE", "write the relocated main image to this path"),
    STR("VITA2NATIVE_DUMP_MAIN_DATA", "write the relocated main data image to this path"),
    STR("VITA2NATIVE_DUMP_MODULE_CODE", "directory for module images, written as they load"),
    FLAG("VITA2NATIVE_DEBUG_SAMPLE", "let kill -USR1 <tid> sample a thread's PC and stack"),
    STR("VITA2NATIVE_PATCH_BKPT", "image offset to plant udf #0 in (comma-separated)"),
    STR("VITA2NATIVE_PATCH", "instruction patches, <offset>=<hex16>,..."),

    /* Loader and module manager. */
    FLAG("VITA2NATIVE_GUEST_ARM", "force ARM mode for guest code (default: from relocations)"),
    FLAG("VITA2NATIVE_TRACE_IMPORTS", "log every import resolution (very noisy)"),
    FLAG("VITA2NATIVE_PREFER_GUEST_EXPORTS", "resolve a title's own exports before the host's"),
    FLAG("VITA2NATIVE_NO_MODULE_AUTOLOAD", "do not load the modules a title imports"),
    STR("VITA2NATIVE_SKIP_MODULES", "comma-separated modules not to load at all"),
    STR("VITA2NATIVE_SKIP_MODULE_START",
        "comma-separated modules to load without calling module_start"),
    STR("VITA2NATIVE_MODULE_BKPT", "per-module udf patches, <module>:<offset>,..."),
    STR("VITA2NATIVE_MODULE_PATCH", "per-module instruction patches, <module>:<offset>=<hex16>"),
    FLAG("VITA2NATIVE_TRACE_MODULE_LOOKUPS", "log module export lookups"),

    /* GXM translation. */
    FLAG("VITA2NATIVE_BATCH_DEBUG", "log draw-batch merging decisions (first 24)"),
    INT("VITA2NATIVE_BATCH_LIMIT", "maximum draws merged into one GL draw"),
    FLAG("VITA2NATIVE_BLEND_TRACE", "log blend state changes"),
    STR("VITA2NATIVE_CLEAR_COLOR", "paint the clear colour as hex RRGGBB"),
    FLAG("VITA2NATIVE_DEBUG_PROGRAM_COLORS", "draw each shader program in a distinct colour"),
    FLAG("VITA2NATIVE_DEBUG_TEXCOLOR", "shade with texture coordinates instead of texels"),
    INT("VITA2NATIVE_DEPTH_ALIAS", "export depth attachments as textures (default 1)"),
    FLAG("VITA2NATIVE_DRAW_COLOR_TRACE", "log the colour each draw writes (implies FORCE_COLOR)"),
    FLAG("VITA2NATIVE_DS_DEBUG", "log depth/stencil load, store and skip decisions"),
    STR("VITA2NATIVE_DUMP_TEXTURES", "directory for sampled textures"),
    STR("VITA2NATIVE_DUMP_TEXTURE_MIPS", "directory for sampled mip levels"),
    FLAG("VITA2NATIVE_FLOAT_TEXTURES", "upload float texture formats without conversion"),
    INT("VITA2NATIVE_FORCE_BLEND", "force a blend mode by number (0 = off)"),
    FLAG("VITA2NATIVE_FORCE_COLOR", "ignore textures and draw from the vertex colour"),
    FLAG("VITA2NATIVE_FORCE_SHADOW_LIT", "treat the shadow map as lit when forcing level 0"),
    FLAG("VITA2NATIVE_GL_CHECK", "enable the per-draw glGetError checks (slow)"),
    FLAG("VITA2NATIVE_GXM_DRAW_ERRORS", "log draws the GL rejects (first 32)"),
    FLAG("VITA2NATIVE_GXM_FLUSH_PIXEL", "read back one pixel per flushed batch"),
    INT("VITA2NATIVE_GXM_FLUSH_TRACE", "log every batch flushed from this frame on"),
    INT("VITA2NATIVE_GXM_FRAME_DUMP", "write the render target of this frame to a file"),
    STR("VITA2NATIVE_GXM_FRAME_DUMP_DIR", "directory for VITA2NATIVE_GXM_FRAME_DUMP"),
    FLAG("VITA2NATIVE_GXM_FRAME_TRACE", "per-frame report of surfaces, draws and clears"),
    FLAG("VITA2NATIVE_GXM_PROGRAM_TRACE", "log shader translation and link results"),
    STR("VITA2NATIVE_GXM_SHADER_DUMP", "directory for translated GLSL"),
    FLAG("VITA2NATIVE_GXM_STATE_TRACE", "log the state applied at each flush"),
    STR("VITA2NATIVE_GXM_UNIFORM_TRACE", "log uniform blocks (value = shader hash filter)"),
    INT("VITA2NATIVE_GXM_VERT_TRACE", "log vertex data from this frame on"),
    INT("VITA2NATIVE_MASK_REPLICATE", "replicate texture channel masks (diagnostic)"),
    FLAG("VITA2NATIVE_NO_BATCH", "never merge draws"),
    FLAG("VITA2NATIVE_NO_CULL", "disable culling"),
    FLAG("VITA2NATIVE_NO_DEPTH", "disable the depth test and write"),
    FLAG("VITA2NATIVE_NO_DRAW_UNIFORMS", "disable the per-draw uniform path"),
    FLAG("VITA2NATIVE_NO_FB_FETCH", "disable framebuffer fetch and read the colour back"),
    FLAG("VITA2NATIVE_NO_MSAA", "render without multisampling"),
    FLAG("VITA2NATIVE_NO_RGBA4", "keep RGBA4444 textures on the RGBA8 path"),
    FLAG("VITA2NATIVE_RGBA4", "force RGBA4444 textures onto the GL_RGBA4 path"),
    FLAG("VITA2NATIVE_POISON_UNIFORMS", "fill fresh uniform blocks with 0xCD"),
    INT("VITA2NATIVE_PROBE_AFTER", "seconds before the draw probe starts"),
    FLAG("VITA2NATIVE_PROBE_DUMP", "dump the render target of probed draws"),
    STR("VITA2NATIVE_PROBE_PIXEL", "probe sample point as x,y (default 50,60)"),
    FLAG("VITA2NATIVE_PROBE_TEXDUMP", "dump the textures bound to probed draws"),
    FLAG("VITA2NATIVE_PROBE_VERTS", "dump the vertex data of probed draws"),
    FLAG("VITA2NATIVE_SAMPLER_TRACE", "log sampler state and the unit a texture lands in"),
    STR("VITA2NATIVE_SKIP_PROGRAM", "program hashes not to draw (vertex:fragment,...)"),
    FLAG("VITA2NATIVE_SYNC_DRAWS", "glFinish after every draw"),
    INT("VITA2NATIVE_TEXTURE_CACHE_MB", "texture cache budget in MB (default 256)"),
    INT("VITA2NATIVE_TEXTURE_HASH_INTERVAL", "re-hash every texture every N frames (diagnostic)"),
    FLAG("VITA2NATIVE_TEXTURE_LEVEL0", "upload only mip level 0"),
    FLAG("VITA2NATIVE_NO_PVRTC_FAST", "use the reference PVRTC decoder instead of the fast path"),
    FLAG("VITA2NATIVE_TRACE_BATCH", "log each merged batch"),
    FLAG("VITA2NATIVE_TRACE_DRAWS", "log the scene and draw call sequence"),
    FLAG("VITA2NATIVE_TRACE_F32M", "log F32M texture uploads and reads"),
    FLAG("VITA2NATIVE_TRACE_PARAMS", "log program parameter lookups"),
    FLAG("VITA2NATIVE_TRACE_TEXTURES", "log texture uploads"),
    FLAG("VITA2NATIVE_TRACE_VIEWPORT", "log the first 16 viewport transforms"),
    STR("VITA2NATIVE_UNIFORM_DUMP", "dump the uniforms of the program with this hash"),
    FLAG("VITA2NATIVE_UNIFORM_NAMES", "annotate uniform traces with the GXP parameter names"),
    INT("VITA2NATIVE_UNIFORM_PADDING", "extra bytes between per-draw uniform blocks"),
    FLAG("VITA2NATIVE_PROFILE", "print the GXM cost breakdown"),
    FLAG("VITA2NATIVE_PROFILE_FRAME", "print the frame split (present, vblank, GXM, guest)"),

    /* Display and presentation. */
    STR("VITA2NATIVE_DUMP_FRAME", "write guest framebuffer BMPs here (%u = frame number)"),
    STR("VITA2NATIVE_DUMP_FRAME_AT", "comma-separated swap indices to capture"),
    FLAG("VITA2NATIVE_FINISH_PER_FRAME", "glFinish each frame is on by default; set to disable"),
    FLAG("VITA2NATIVE_FPS", "log the frame rate once a second"),
    FLAG("VITA2NATIVE_FPS_OVERLAY", "draw the FPS graph in the corner of the frame"),
    STR("VITA2NATIVE_METRICS_FILE", "append <ms>,<fps>,<frames> per second for fps_chart.py"),
    FLAG("VITA2NATIVE_KMS_PRESENT", "use the direct KMS/DRM present path"),
    FLOAT("VITA2NATIVE_RENDER_SCALE", "scale the 3D scene, upscaled again on present"),
    STR("VITA2NATIVE_SDL_SIZE", "SDL window size, WxH (default: the display geometry)"),
    FLAG("VITA2NATIVE_SDL_WINDOWED", "keep an SDL window instead of going fullscreen"),
    FLAG("VITA2NATIVE_STRETCH", "stretch the frame to fill the display"),
    FLAG("VITA2NATIVE_SKIP_READBACK", "skip the colour readback (measure a frame without it)"),
    FLAG("VITA2NATIVE_NO_PRESENT_OVERLAP", "the present overlap is on by default; set to disable"),
    FLAG("VITA2NATIVE_GL_INFO", "log the GL/EGL vendor, renderer and version"),
    FLAG("VITA2NATIVE_DIRECT_PRESENT_DEBUG", "log direct-present attach and frame handoff"),

    /* Input. */
    STR("VITA2NATIVE_CTRL_SCRIPT", "frame-keyed pad script, e.g. 30:down,32:,38:cross"),
    FLAG("VITA2NATIVE_CTRL_SCRIPT_DEBUG", "log every scripted input change"),
    FLAG("VITA2NATIVE_CTRL_SWAP_AB", "swap cross and circle on every input source"),
    FLAG("VITA2NATIVE_INPUT_DEBUG", "log the input mapping and the SDL controller choice"),
    STR("VITA2NATIVE_TOUCH_SCRIPT", "frame-keyed touch script, same syntax as CTRL_SCRIPT"),
    FLAG("VITA2NATIVE_TOUCH_SCRIPT_DEBUG", "log every scripted touch change"),

    /* Audio. */
    FLAG("VITA2NATIVE_AUDIO_DEBUG", "SceAudioOut/SceAudioIn queueing and timing"),
    FLAG("VITA2NATIVE_NGS_DEBUG", "SceNgs graph, parameter and peak tracing"),

    /* Kernel, memory and file I/O. */
    INT("VITA2NATIVE_FREE_MEM", "override the free-memory size reported to titles"),
    FLAG("VITA2NATIVE_MSPACE_DEBUG", "log mspace allocation calls"),
    FLAG("VITA2NATIVE_TRACE_ALLOC", "log guest malloc/free calls"),
    FLAG("VITA2NATIVE_TRACE_SYNC", "log mutex and semaphore operations"),
    FLAG("VITA2NATIVE_TRACE_THREADID", "log the guest/host thread id mapping"),
    FLAG("VITA2NATIVE_TRACE_THREAD_STACK", "log each thread's stack allocation"),
    FLAG("VITA2NATIVE_TRACE_FILEIO", "log guest file opens and I/O"),
    FLAG("VITA2NATIVE_TRACE_FIOS", "log SceFios2 operations"),
    FLAG("VITA2NATIVE_TRACE_ABORT", "print guest callers in sceKernelCallAbortHandler"),
    FLAG("VITA2NATIVE_TRACE_PROCESS_PARAM", "log the process parameter read at boot"),
    STR("VITA2NATIVE_TRACE_TARGET", "vita addresses to trace call arguments for"),

    /* Everything else. */
    FLAG("VITA2NATIVE_NETWORK", "report the host network state (offline is the default)"),
    FLAG("VITA2NATIVE_ENV_DUMP", "print every setting with its value and exit"),
};

#undef FLAG
#undef INT
#undef FLOAT
#undef STR

/*
 * Name-based cache for getenv(). The keys are string literals, so only the
 * value has to be stored; a miss is cached as NULL so a knob that is not set is
 * not looked up again either.
 */
#define SETTINGS_CACHE_SLOTS 256

typedef struct {
    const char *name;
    const char *value;
    bool cached;
} CacheEntry;

static CacheEntry g_cache[SETTINGS_CACHE_SLOTS];
static size_t g_cache_used;

static uint32_t hash_name(const char *name)
{
    uint32_t hash = 2166136261u;

    while (*name) {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }
    return hash;
}

/* Returns the slot for `name`, inserting it when `create` is set. NULL means
   the cache is full, and the caller should fall back to getenv(). */
static CacheEntry *cache_slot(const char *name, bool create)
{
    uint32_t index = hash_name(name) % SETTINGS_CACHE_SLOTS;

    for (size_t probe = 0; probe < SETTINGS_CACHE_SLOTS; probe++) {
        CacheEntry *entry = &g_cache[index];

        if (entry->name && strcmp(entry->name, name) == 0)
            return entry;
        if (!entry->name) {
            if (!create)
                return NULL;
            entry->name = name;
            g_cache_used++;
            return entry;
        }
        index = (index + 1) % SETTINGS_CACHE_SLOTS;
    }
    return NULL;
}

const Setting *vita2native_settings_table(size_t *count)
{
    if (count)
        *count = COUNT_OF(g_settings);
    return g_settings;
}

const Setting *vita2native_settings_find(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < COUNT_OF(g_settings); i++) {
        if (strcmp(g_settings[i].name, name) == 0)
            return &g_settings[i];
    }
    return NULL;
}

/* Edit distance, capped: anything above the cap is all the caller needs. */
static size_t edit_distance(const char *a, const char *b, size_t cap)
{
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    size_t previous[96];
    size_t current[96];

    if (len_a >= COUNT_OF(previous) || len_b >= COUNT_OF(previous))
        return cap;
    if ((len_a > len_b ? len_a - len_b : len_b - len_a) >= cap)
        return cap;

    for (size_t j = 0; j <= len_b; j++)
        previous[j] = j;

    for (size_t i = 1; i <= len_a; i++) {
        current[0] = i;
        for (size_t j = 1; j <= len_b; j++) {
            size_t substitution = previous[j - 1] + (a[i - 1] != b[j - 1]);
            size_t deletion = previous[j] + 1;
            size_t insertion = current[j - 1] + 1;
            size_t best = substitution < deletion ? substitution : deletion;

            current[j] = best < insertion ? best : insertion;
        }
        memcpy(previous, current, (len_b + 1) * sizeof(previous[0]));
    }
    return previous[len_b];
}

const char *vita2native_settings_suggest(const char *name)
{
    const char *best = NULL;
    size_t best_distance = 3; /* accept at most two edits */

    if (!name)
        return NULL;
    for (size_t i = 0; i < COUNT_OF(g_settings); i++) {
        size_t distance = edit_distance(name, g_settings[i].name, best_distance);

        if (distance > 0 && distance < best_distance) {
            best_distance = distance;
            best = g_settings[i].name;
        }
    }
    return best;
}

const char *vita2native_env_str(const char *name)
{
    CacheEntry *entry;

    if (!name)
        return NULL;

    entry = cache_slot(name, true);
    if (!entry)
        return getenv(name);

    if (!entry->cached) {
        /* Copy: setenv() may reuse the environment block, so a pointer from
           getenv() does not stay valid for the life of the process. */
        const char *raw = getenv(name);

        entry->value = raw ? strdup(raw) : NULL;
        if (raw && !entry->value)
            entry->value = raw;
        entry->cached = true;
    }
    return entry->value;
}

bool vita2native_env_flag(const char *name)
{
    const char *value = vita2native_env_str(name);

    /* Presence, like the getenv(...) != NULL checks this replaces: an empty
       value counts as unset, but "0" is still set. */
    return value && value[0] != '\0';
}

long vita2native_env_int(const char *name, long fallback)
{
    const char *value = vita2native_env_str(name);
    char *end = NULL;
    long parsed;

    if (!value || !value[0])
        return fallback;
    parsed = strtol(value, &end, 0);
    return end && end != value ? parsed : fallback;
}

double vita2native_env_double(const char *name, double fallback)
{
    const char *value = vita2native_env_str(name);
    char *end = NULL;
    double parsed;

    if (!value || !value[0])
        return fallback;
    parsed = strtod(value, &end);
    return end && end != value ? parsed : fallback;
}

void vita2native_settings_check(void)
{
    char name[96];

    if (!environ)
        return;

    for (char **entry = environ; *entry; entry++) {
        const char *equals;
        size_t length;
        const char *hint;

        if (strncmp(*entry, "VITA2NATIVE_", 12) != 0)
            continue;

        equals = strchr(*entry, '=');
        length = equals ? (size_t)(equals - *entry) : strlen(*entry);
        if (length >= sizeof(name))
            continue;
        memcpy(name, *entry, length);
        name[length] = '\0';

        if (vita2native_settings_find(name))
            continue;

        hint = vita2native_settings_suggest(name);
        if (hint)
            fprintf(stderr, "vita2native: unknown setting %s (did you mean %s?)\n", name, hint);
        else
            fprintf(stderr,
                    "vita2native: unknown setting %s (VITA2NATIVE_ENV_DUMP=1 lists every one)\n",
                    name);
    }
    fflush(stderr);
}

static const char *kind_name(SettingKind kind)
{
    switch (kind) {
    case SETTING_FLAG:
        return "flag";
    case SETTING_INT:
        return "int";
    case SETTING_FLOAT:
        return "float";
    case SETTING_STRING:
        return "string";
    }
    return "?";
}

void vita2native_settings_dump(void)
{
    size_t count;
    const Setting *table = vita2native_settings_table(&count);

    printf("vita2native: %zu settings\n", count);
    for (size_t i = 0; i < count; i++) {
        const char *value = vita2native_env_str(table[i].name);

        printf("  %-6s %-34s %-20s %s\n", kind_name(table[i].kind), table[i].name,
               value ? value : "(unset)", table[i].description);
    }
    fflush(stdout);
}
