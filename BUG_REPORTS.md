# Bug reports

## VPK titles share save data

Directly launched VPKs are all extracted to `vita2native/app0`, and the save
namespace is derived from that directory name. Different VPK titles therefore
share `savedata/app0` and can read or overwrite each other's saves. Derive the
namespace from a stable per-title identity instead. See `prepare_app_dir` in
`source/main.c` and `host_fs_init` in `source/host/host_fs.c`.

## File handles can be freed during concurrent I/O

`get_opened_file_for_fd` releases the dictionary lock before returning its
pointer. A concurrent `sceIoClose` can close and free that object while another
thread uses it; concurrent closes can also operate on the same object. Protect
the handle's lifetime throughout each operation. See `source/modules/SceLibKernel.c`.

## Async result slots leak on synchronous close

The 128-slot async result table forgets entries on async close, but not on
ordinary `sceIoClose`. After enough distinct file UIDs have been used for async
I/O and closed synchronously, subsequent results are silently discarded and
`sceIoComplete` returns zero. Release results on every close and handle a full
table. See `source/modules/SceIoAsync.c`.

## File read errors look like EOF

`sceIoRead` returns the count from `fread` without checking `ferror`. A host
read error can therefore be reported as EOF or a successful short read. Map
stream errors to a Vita error as appropriate. See `source/modules/SceLibKernel.c`.

## Texture LOD query lowering returns incorrect values

`lower_texture_query_lod` in `source/spirv_cross_glsl_es.cpp` replaces queries
unconditionally, even when the driver supports the extension. Its second result
component is a texture coordinate instead of the LOD lambda. Its first component
also ignores sampler LOD limits and available mip levels, and its cube-map
calculation does not project onto the selected face. Shaders using these queries
can select the wrong mip or compute incorrect filter coefficients. Prefer the
native extension where available and explicitly account for its semantics in a
fallback.

## Wireframe draws use uninitialized draw indices

For the rewritten per-draw-uniform vertex shader, the wireframe path in
`source/modules/SceGxm.c` uploads only one zero draw index but enables it as a
per-vertex array. Indices beyond vertex zero read uninitialized buffer contents,
so vertices can select the wrong uniform slot. Use a constant vertex attribute
or supply a zero for each addressed vertex.

## Shader cache saturation causes repeated compilation and leaked shaders

The in-memory shader cache holds 128 entries. Once full, a newly compiled shader
is returned without being cached or given another owner; program release assumes
the cache owns it. Recreated guest programs or changing texture hints can then
compile that same shader repeatedly, leaking GL shader objects and causing
stutter. See `gxm_translate_shader` and shader patcher release in
`source/modules/SceGxm.c`.

## Unused texture bindings produce redundant shader variants

The shader hint signature includes every texture unit, regardless of which
units the program samples. Changing an unused binding can therefore produce a
new shader cache key and trigger translation, compilation and linking of an
equivalent shader. Limit the signature to dependencies actually used by the
program, then benchmark the effect. See `gxm_hint_signature` and
`gxm_ensure_shaders` in `source/modules/SceGxm.c`.

# Further rendering performance candidates

These are costs established by code inspection; their impact on a particular
title and device still needs profiling. Preserve guest-visible ordering and
memory updates when evaluating alternatives.

## Unused texture units are processed on every state flush

`gxm_upload_textures` visits all fragment and vertex units even when the
current shaders do not sample them. A still-bound unused texture can be hashed,
decoded and uploaded on a cache miss or change, consuming CPU, memory and GL
bandwidth without affecting the draw. Use the linked programs' active sampler
dependencies to filter this work; test shaders with dynamic sampler use and
bindings changing across draws. See `source/modules/SceGxm.c`.

## Sparse indexed draws copy the entire vertex prefix

Every draw scans all indices for the maximum, then `gxm_batch_append` copies
vertices from zero through that maximum for every stream and fills the same
number of draw-index values. A small draw referencing a high index thus copies
and uploads far more data than it uses, repeatedly if several draws share a
vertex buffer. Measure copied bytes versus referenced indices; consider a
minimum/maximum range with rebased indices, or safe reuse when guest data has
not changed. See `gxm_draw_common` and `gxm_batch_append` in
`source/modules/SceGxm.c`.

## Texture decoding creates and joins threads per large texture

`gxm_decode_in_bands` calls `pthread_create` and `pthread_join` for every
eligible decode. Asset-heavy scenes and frequently updated large textures pay
thread startup and synchronization costs repeatedly and can oversubscribe
concurrent render threads. Compare a bounded persistent worker pool against
the current path on both handhelds, including small workloads and simultaneous
decodes; retain a single-thread fallback. See `include/gxm/decode_bands.h`.

## Shader hint and program lookup work repeats on every draw

`gxm_ensure_shaders` builds format arrays and hashes all units for both stages
on every draw, and `gxm_get_linked_program` linearly scans up to 128 entries,
even when the same program and texture state remain bound. Cache signatures or
linked-program lookups behind appropriate state changes and measure the draw
setup cost, especially for titles issuing many small draws. See
`source/modules/SceGxm.c`.

## Batching cannot merge across blend states

The batcher breaks on every draw when a title alternates blend states per
sprite (`ADD` with `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`, then blending off), which
leaves roughly one GL draw per guest draw. That makes such a title CPU-bound on
draw submission rather than GPU fill: its frame rate is flat in the render
scale.

Applying the blend equation in the fragment shader is not enough on its own. A
version that read the destination via `gl_LastFragColorARM` and covered all 12
factors and 6 functions produced a byte-identical frame but no speed win,
because the factors were plain uniforms: a merged draw would apply one blend to
every vertex, so the batch state still split on blend. It was removed rather
than kept as dead code.

To merge, the parameters have to ride in the per-draw channel the vertex shader
already has (`v2nDrawIndex`) — a flat varying into the fragment stage and
parameters in a std140 block indexed by it — and `blend_info` has to leave the
batch-state comparison. The subtlety that defeated the first attempt: the vertex
and fragment stages are translated independently, so the varying must be gated
on the *pair* (the vertex program's `draw_uniform_location >= 0`), otherwise a
fragment that reads it can be linked with a vertex shader that does not write
it. See the shader patcher and batch-state comparison in
`source/modules/SceGxm.c`.

**Considered and rejected:** scaling only those render targets at least as wide
as the panel, so small UI targets stay native (a 384x216 HUD becomes 256x144 and
is unreadable at 0.6667). It trades image quality per target for frame rate
instead of removing work; the right answer for a slow title is to make the frame
cheaper. Do not re-propose it.

## Profiling clocks run even when profiling is disabled

The draw and flush paths call `clock_gettime` and update profiling counters
unconditionally, including several timing intervals inside each state flush.
This overhead scales with draw and batch counts in normal gameplay. Gate the
profiling-only measurements when neither relevant profiler is enabled, while
preserving timing required by diagnostics, and benchmark the change. See
`gxm_draw_common`, `gxm_flush_state`, and `gxm_batch_flush` in
`source/modules/SceGxm.c`.
