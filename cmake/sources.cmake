# The emulator target: sources, flags and link line.
#
# Included from the top-level CMakeLists.txt after deps.cmake (which defines the
# backend sources and the vendored source lists).

add_executable(vita2native
    source/host/host_video_sdl.c
    source/host/host_fs.c
    source/host/host_psarc.c
    source/host/host_jit.c
    source/host/host_thread.c
    ${VITA2NATIVE_HOST_GFX_SOURCES}
    external/Vita3K/external/miniz/miniz.c
    external/sqlite/sqlite3.c
    ${VITA2NATIVE_GXM_TEXTURE_SOURCES}
    source/stb_image_impl.c
    source/stb_image_write_impl.c
    source/load.c
    source/log.c
    source/main.c
    source/module.c
    source/netlog.c
    source/settings.c
    source/spirv_cross_glsl_es.cpp
    source/util.c
    source/vita3k_shader_recompiler_iface_c.cpp
    source/vpk.c
    ${VITA2NATIVE_ATRAC9_SOURCES}

    ${VITA2NATIVE_CTRL_SOURCE}
    source/modules/SceCtrl_common.c
    source/modules/SceCtrl_script.c
    ${VITA2NATIVE_DISPLAY_SOURCE}
    ${VITA2NATIVE_AUDIO_SOURCE}

    source/modules/SceGxm.c
    source/modules/SceAppMgr.c
    source/modules/SceCamera.c
    source/modules/SceAtrac.c
    source/modules/SceAudiodec.c
    source/modules/SceJpeg.c
    source/modules/SceAvPlayer.c
    source/modules/SceAppUtil.c
    source/modules/SceCommonDialog.c
    source/modules/common_dialog_overlay.c
    source/modules/fps_overlay.c
    source/modules/SceKernelThreadMgr.c
    source/modules/SceKernelRuntime.c
    source/modules/SceKernelLocks.c
    source/modules/SceKernelTimer.c
    source/modules/SceKernelEvent.c
    source/modules/SceModulemgr.c
    source/modules/SceMotion.c
    source/modules/SceKernelSync.c
    source/modules/SceLibKernel.c
    source/modules/SceLibKernelAeabi.c
    source/modules/SceFios2.c
    source/modules/SceFiber.c
    source/modules/SceHttp.c
    source/modules/SceSsl.c
    source/modules/SceNpDrm.c
    source/modules/SceNp.c
    source/modules/ScePgf.c
    source/modules/SceProcessmgr.c
    source/modules/SceRegistryMgr.c
    source/modules/SceAdhoc.c
    source/modules/SceMediaExport.c
    source/modules/ScePromoterUtil.c
    source/modules/SceUsbMtp.c
    source/modules/SceVshBridge.c
    source/modules/SceSqlite.c
    source/modules/SceIoAsync.c
    source/modules/SceLibClib.c
    source/modules/SceLibc.c
    source/mspace.c
    source/modules/SceNet.c
    source/modules/SceNetCtl.c
    source/minimp3_impl.c
    source/modules/SceNgs.c
    source/modules/SceNgs_render.c
    source/modules/ScePower.c
    source/modules/SceRtc.c
    source/modules/SceSharedFb.c
    source/modules/SceShellUtil.c
    source/modules/SceSysmodule.c
    source/modules/SceSysmem.c
    source/modules/SceSystemGesture.c
    source/modules/SceTouch_linux.c
    source/modules/SceAudio.c
)

target_include_directories(vita2native PRIVATE
    include
    source/host
    external/khronos/include
    external/minimp3
    external/font8x8
    external/sqlite
    external/Vita3K/external/LibAtrac9/C/src
    external/vita-headers/include
    external/faad2/include
    external/Vita3K/external/stb
    external/Vita3K/vita3k/renderer/include
    ${SDL2_INCLUDE_DIRS}
)

if(VITA2NATIVE_HEADLESS)
    # Presentation is the headless display backend's job (SDL); the GLES
    # context must stay offscreen even when DISPLAY is set, otherwise an X11
    # DISPLAY moves EGL to the windowing platform and every GL call round-trips
    # through the X server.
    target_compile_definitions(vita2native PRIVATE VITA2NATIVE_HEADLESS_BUILD=1)
endif()

target_compile_options(vita2native PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wimplicit-fallthrough
    -Wdouble-promotion
    -fstack-usage
    -fdata-sections
    -ffunction-sections
    -fno-strict-aliasing
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^arm"
   AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(vita2native PRIVATE ${VITA2NATIVE_ARM_ARCH_FLAGS})
endif()

target_compile_definitions(vita2native PRIVATE
    _GNU_SOURCE=1
    _FILE_OFFSET_BITS=64
    VITA2NATIVE_MAJOR="${VITA2NATIVE_MAJOR}"
    VITA2NATIVE_MINOR="${VITA2NATIVE_MINOR}"
    VITA2NATIVE_PATCH="${VITA2NATIVE_PATCH}"
    VITA2NATIVE_HASH="${VITA2NATIVE_HASH}"
)

if(SDL2_LIBRARY_DIRS)
    target_link_directories(vita2native PRIVATE ${SDL2_LIBRARY_DIRS})
endif()

target_link_libraries(vita2native PRIVATE
    shader
    glslang
    spirv-cross-glsl
    spirv-cross-core
    fmt-header-only
    miniz
    mlib
    faad
    ${SDL2_LIBRARIES}
    pthread
    dl
    m
)

# Keep symbols in the executable so the guest crash handler can print a useful
# backtrace. MinSize builds can opt out to save the dynamic symbol table.
if(NOT VITA2NATIVE_NO_BACKTRACE_SYMBOLS)
    target_link_options(vita2native PRIVATE -rdynamic)
    set_target_properties(vita2native PROPERTIES ENABLE_EXPORTS ON)
endif()
