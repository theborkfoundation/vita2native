# Third-party dependencies, backends and vendored source lists.
#
# Included from the top-level CMakeLists.txt after the project() call. Nothing
# here creates our own targets apart from the vendored static libraries.

include(GNUInstallDirs)

# --- Platform backends -------------------------------------------------------
#
# One backend pair for every target: SceDisplay hands the GPU's render target to
# the host video layer, which picks the scanout (KMS) or window (EGL) surface,
# and SceCtrl takes its input from that same layer, which samples SDL pads and
# keyboards on the video thread.
#
# SDL is dlopen'd (source/host/host_video_sdl.c), not linked, so the build needs
# neither its headers nor its library; that is also what lets the emulator start
# on a target without SDL installed.

set(VITA2NATIVE_DISPLAY_SOURCE source/modules/SceDisplay.c)
set(VITA2NATIVE_CTRL_SOURCE source/modules/SceCtrl_sdl.c)
set(VITA2NATIVE_AUDIO_SOURCE source/modules/SceAudio_sdl.c)

# --- FAAD2 (GPLv2) -----------------------------------------------------------
#
# Provides the AAC decoder. Bundled as a static library and treated as
# third-party code for warning and sanitizer purposes.

file(GLOB FAAD2_SOURCES external/faad2/libfaad/*.c)
add_library(faad STATIC ${FAAD2_SOURCES})
target_include_directories(faad PUBLIC external/faad2/include PRIVATE external/faad2/libfaad)
target_compile_definitions(faad PRIVATE
    APPLY_DRC
    HAVE_INTTYPES_H=1
    HAVE_MEMCPY=1
    HAVE_STRING_H=1
    HAVE_STRINGS_H=1
    HAVE_SYS_STAT_H=1
    HAVE_SYS_TYPES_H=1
    STDC_HEADERS=1
    PACKAGE_VERSION="2.11.1"
)
target_compile_options(faad PRIVATE -w)
target_link_libraries(faad PUBLIC m)
set_source_files_properties(${FAAD2_SOURCES} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined")

# --- Vendored translation units ---------------------------------------------
#
# Third-party code with its own warning hygiene: the sqlite amalgamation, the
# dlmalloc copy and stb_image_write (which shifts a signed accumulator into the
# sign bit; well-defined on the target compilers in release builds, but Zig's
# debug instrumentation traps it).

set_source_files_properties(external/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(source/mspace.c PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(
    source/stb_image_impl.c
    source/stb_image_write_impl.c
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined"
)

# --- Vendored source lists ---------------------------------------------------
#
# Shared by the emulator target and the smoke tests that exercise them, so the
# file list exists once. LibAtrac9 alone was repeated in three targets.

set(VITA2NATIVE_ATRAC9_SOURCES
    external/Vita3K/external/LibAtrac9/C/src/band_extension.c
    external/Vita3K/external/LibAtrac9/C/src/bit_allocation.c
    external/Vita3K/external/LibAtrac9/C/src/bit_reader.c
    external/Vita3K/external/LibAtrac9/C/src/decinit.c
    external/Vita3K/external/LibAtrac9/C/src/decoder.c
    external/Vita3K/external/LibAtrac9/C/src/huffCodes.c
    external/Vita3K/external/LibAtrac9/C/src/imdct.c
    external/Vita3K/external/LibAtrac9/C/src/libatrac9.c
    external/Vita3K/external/LibAtrac9/C/src/quantization.c
    external/Vita3K/external/LibAtrac9/C/src/scale_factors.c
    external/Vita3K/external/LibAtrac9/C/src/tables.c
    external/Vita3K/external/LibAtrac9/C/src/unpack.c
    external/Vita3K/external/LibAtrac9/C/src/utility.c
)

set(VITA2NATIVE_GXM_TEXTURE_SOURCES
    source/gxm_texture.c
    source/gxm_texture_pvrtc.cpp
    source/gxm_texture_pvrtc_fast.c
    external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
)

set(VITA2NATIVE_HOST_GFX_SOURCES
    source/frame_profile.c
    source/host/host_gfx.c
    source/host/host_gfx_gl.c
    source/host/host_gfx_null.c
)
