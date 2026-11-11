# Smoke tests.
#
# Included from the top-level CMakeLists.txt after sources.cmake. Every target
# here is a standalone binary: it compiles the module it exercises straight from
# source, so it also works in the x64 syntax-check preset.

include(CTest)

if(NOT BUILD_TESTING)
    return()
endif()

# The smoke tests drive their setup from assert(), so NDEBUG must not remove
# those calls in optimizing builds.
add_compile_options(-UNDEBUG)

# vita2native_add_smoke(<name>
#     SOURCES <source>...
#     [LIBS <library>...]        link libraries
#     [INCLUDES <directory>...]  extra include directories
#     [DEFINES <define>...]      extra compile definitions
#     [ARGUMENTS <argument>...]  ctest arguments
#     [NO_TEST])                 build only, do not register with ctest
#
# `include` and `external/vita-headers/include` as well as `_GNU_SOURCE=1` are
# implied: every test needs them and 31 copies of the same block was the reason
# this helper exists.
function(vita2native_add_smoke name)
    cmake_parse_arguments(SMOKE "NO_TEST" "" "SOURCES;LIBS;INCLUDES;DEFINES;ARGUMENTS" ${ARGN})

    add_executable(${name} ${SMOKE_SOURCES})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/vita-headers/include
        ${SMOKE_INCLUDES}
    )
    target_compile_definitions(${name} PRIVATE _GNU_SOURCE=1 ${SMOKE_DEFINES})

    if(SDL2_LIBRARY_DIRS)
        target_link_directories(${name} PRIVATE ${SDL2_LIBRARY_DIRS})
    endif()

    if(SMOKE_LIBS)
        target_link_libraries(${name} PRIVATE ${SMOKE_LIBS})
    endif()

    if(NOT SMOKE_NO_TEST)
        add_test(NAME ${name} COMMAND ${name} ${SMOKE_ARGUMENTS})
    endif()
endfunction()

vita2native_add_smoke(host_smoke
    SOURCES
        tests/host_smoke.c
        source/host/host_fs.c
        source/host/host_thread.c
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread)

vita2native_add_smoke(mspace_smoke
    SOURCES tests/mspace_smoke.c source/mspace.c)

vita2native_add_smoke(ctrl_script_smoke
    SOURCES
        tests/ctrl_script_smoke.c
        source/modules/SceCtrl_null.c
        source/modules/SceCtrl_common.c
        source/modules/SceCtrl_script.c
        source/host/host_video_sdl.c
    LIBS pthread ${CMAKE_DL_LIBS})

vita2native_add_smoke(regmgr_smoke
    SOURCES tests/regmgr_smoke.c source/modules/SceRegistryMgr.c)

vita2native_add_smoke(extra_imports_smoke
    SOURCES
        tests/extra_imports_smoke.c
        source/module.c
        source/modules/SceAdhoc.c
        source/modules/SceMediaExport.c
        source/modules/ScePromoterUtil.c
        source/modules/SceUsbMtp.c
    LIBS pthread)

vita2native_add_smoke(jpeg_smoke
    SOURCES
        tests/jpeg_smoke.c
        source/module.c
        source/modules/SceJpeg.c
        source/stb_image_impl.c
        source/stb_image_write_impl.c
    INCLUDES external/Vita3K/external/stb
    LIBS pthread m)

vita2native_add_smoke(camera_smoke
    SOURCES tests/camera_smoke.c source/module.c source/modules/SceCamera.c
    LIBS pthread)

vita2native_add_smoke(gfx_smoke
    SOURCES tests/gfx_smoke.c ${VITA2NATIVE_HOST_GFX_SOURCES}
    INCLUDES external/khronos/include
    LIBS ${CMAKE_DL_LIBS} m)

vita2native_add_smoke(gpu_bench
    SOURCES tests/gpu_bench.c ${VITA2NATIVE_HOST_GFX_SOURCES}
    INCLUDES source/host external/khronos/include
    LIBS ${CMAKE_DL_LIBS} m
    NO_TEST)

vita2native_add_smoke(gl_thread_smoke
    SOURCES tests/gl_thread_smoke.c ${VITA2NATIVE_HOST_GFX_SOURCES}
    INCLUDES source/host external/khronos/include
    LIBS pthread ${CMAKE_DL_LIBS} m)

# Interactive display check: presents a moving pattern through DRM/KMS or
# fbdev. Not a ctest entry because it needs a real panel and runs until the
# requested duration expires.
vita2native_add_smoke(present_probe
    SOURCES tests/present_probe.c source/host/host_video_sdl.c
    DEFINES _FILE_OFFSET_BITS=64
    NO_TEST)

# Virtual gamepad used to validate the evdev input path without touching the
# hardware (needs /dev/uinput and usually root).
vita2native_add_smoke(uinput_pad
    SOURCES tests/uinput_pad.c
    NO_TEST)

vita2native_add_smoke(audio_smoke
    SOURCES
        tests/audio_smoke.c
        source/modules/SceAudio.c
        ${VITA2NATIVE_AUDIO_SOURCE}
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    INCLUDES source/modules
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m ${SDL2_LIBRARIES})

vita2native_add_smoke(sync_smoke
    SOURCES
        tests/sync_smoke.c
        source/modules/SceKernelSync.c
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    INCLUDES source/modules external/mlib
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(threadmgr_smoke
    SOURCES
        tests/threadmgr_smoke.c
        source/modules/SceKernelThreadMgr.c
        source/modules/SceKernelSync.c
        source/modules/SceKernelLocks.c
        source/modules/SceKernelEvent.c
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    INCLUDES source/modules external/mlib
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(audiodec_smoke
    SOURCES
        tests/audiodec_smoke.c
        source/modules/SceAudiodec.c
        source/minimp3_impl.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
        ${VITA2NATIVE_ATRAC9_SOURCES}
    INCLUDES
        source/modules external/minimp3
        external/Vita3K/external/LibAtrac9/C/src
        external/mlib external/faad2/include
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m faad)

vita2native_add_smoke(atrac_smoke
    SOURCES
        tests/atrac_smoke.c
        source/modules/SceAtrac.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
        ${VITA2NATIVE_ATRAC9_SOURCES}
    INCLUDES source/modules external/Vita3K/external/LibAtrac9/C/src external/mlib
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(apputil_smoke
    SOURCES
        tests/apputil_smoke.c
        source/modules/SceAppUtil.c
        source/host/host_fs.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    INCLUDES source/modules
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(libc_format_smoke
    SOURCES
        tests/libc_format_smoke.c
        source/modules/SceLibc.c
        source/module.c
        source/host/host_fs.c
        source/host/host_psarc.c
        source/host/host_thread.c
        source/mspace.c
        source/log.c
        source/netlog.c
        external/Vita3K/external/miniz/miniz.c
    INCLUDES source/modules external/Vita3K/external
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(fios2_smoke
    SOURCES
        tests/fios2_smoke.c
        source/modules/SceFios2.c
        source/host/host_fs.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    INCLUDES source/modules
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(fiber_smoke
    SOURCES
        tests/fiber_smoke.c
        source/modules/SceFiber.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    LIBS pthread)

vita2native_add_smoke(pgf_smoke
    SOURCES tests/pgf_smoke.c source/modules/ScePgf.c
    INCLUDES external/font8x8
    LIBS m)

vita2native_add_smoke(appmgr_smoke
    SOURCES tests/appmgr_smoke.c source/modules/SceAppMgr.c source/host/host_fs.c)

vita2native_add_smoke(aeabi_smoke
    SOURCES tests/aeabi_smoke.c source/modules/SceLibKernelAeabi.c)

vita2native_add_smoke(sqlite_smoke
    SOURCES
        tests/sqlite_smoke.c
        source/modules/SceSqlite.c
        external/sqlite/sqlite3.c
        source/host/host_fs.c
        source/host/host_thread.c
    INCLUDES source/modules external/sqlite
    DEFINES _FILE_OFFSET_BITS=64
    LIBS pthread m)

vita2native_add_smoke(net_smoke
    SOURCES
        tests/net_smoke.c
        source/modules/SceNet.c
        source/modules/SceNetCtl.c
        source/host/host_thread.c
    LIBS pthread m)

vita2native_add_smoke(processmgr_smoke
    SOURCES
        tests/processmgr_smoke.c
        source/module.c
        source/modules/SceProcessmgr.c
        source/host/host_thread.c
    LIBS pthread m)

vita2native_add_smoke(dialog_smoke
    SOURCES
        tests/dialog_smoke.c
        tests/dialog_deps_stub.c
        source/modules/SceCommonDialog.c
        source/host/host_thread.c
    LIBS pthread)

vita2native_add_smoke(dialog_overlay_smoke
    SOURCES
        tests/dialog_overlay_smoke.c
        tests/dialog_deps_stub.c
        source/module.c
        source/modules/SceCommonDialog.c
        source/modules/common_dialog_overlay.c
        source/host/host_thread.c
    INCLUDES external/font8x8
    LIBS pthread)

vita2native_add_smoke(avplayer_smoke
    SOURCES
        tests/avplayer_smoke.c
        source/module.c
        source/modules/SceAvPlayer.c
        source/host/host_thread.c
    LIBS pthread)

vita2native_add_smoke(touch_smoke
    SOURCES
        tests/touch_smoke.c
        source/modules/SceTouch_linux.c
        source/host/host_thread.c
        source/host/host_video_sdl.c
    LIBS pthread m ${CMAKE_DL_LIBS})

vita2native_add_smoke(http_smoke
    SOURCES tests/http_smoke.c source/modules/SceHttp.c source/modules/SceSsl.c
    LIBS pthread dl m)

vita2native_add_smoke(ngs_smoke
    SOURCES
        tests/ngs_smoke.c
        source/modules/SceNgs.c
        source/modules/SceNgs_render.c
        ${VITA2NATIVE_ATRAC9_SOURCES}
    INCLUDES external/Vita3K/external/LibAtrac9/C/src
    LIBS pthread)

vita2native_add_smoke(egl_window_smoke
    SOURCES tests/egl_window_smoke.c ${VITA2NATIVE_HOST_GFX_SOURCES}
    INCLUDES source/host external/khronos/include
    LIBS ${CMAKE_DL_LIBS} m)

if(NOT VITA2NATIVE_HEADLESS)
    vita2native_add_smoke(display_egl_smoke
        SOURCES
            tests/display_egl_smoke.c
            source/modules/SceDisplay_linux.c
            ${VITA2NATIVE_HOST_GFX_SOURCES}
            source/host/host_thread.c
            source/log.c
            source/netlog.c
        INCLUDES
            source/host
            external/khronos/include
            external/Vita3K/external/LibAtrac9/C/src
            ${SDL2_INCLUDE_DIRS}
        DEFINES _FILE_OFFSET_BITS=64
        LIBS ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS} m pthread)
endif()

# shader_smoke takes a .gxp path, so it registers one ctest entry per fixture
# instead of through the helper's ARGUMENTS.
vita2native_add_smoke(shader_smoke
    SOURCES
        tests/shader_smoke.c
        source/spirv_cross_glsl_es.cpp
        source/vita3k_shader_recompiler_iface_c.cpp
    LIBS shader glslang spirv-cross-glsl spirv-cross-core fmt-header-only m
    NO_TEST)
add_test(NAME shader_smoke_clear
    COMMAND shader_smoke
        ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders/clear_v.gxp)
add_test(NAME shader_smoke_texture
    COMMAND shader_smoke
        ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders/texture_f.gxp)

vita2native_add_smoke(texture_smoke
    SOURCES tests/texture_smoke.c ${VITA2NATIVE_GXM_TEXTURE_SOURCES}
    INCLUDES external/Vita3K/vita3k/renderer/include)

vita2native_add_smoke(gxm_draw_smoke
    SOURCES
        tests/gxm_draw_smoke.c
        ${VITA2NATIVE_HOST_GFX_SOURCES}
        source/host/host_thread.c
        source/host/host_fs.c
        ${VITA2NATIVE_GXM_TEXTURE_SOURCES}
        source/modules/SceGxm.c
        source/spirv_cross_glsl_es.cpp
        source/vita3k_shader_recompiler_iface_c.cpp
        source/stb_image_write_impl.c
    INCLUDES
        source/host
        external/khronos/include
        external/Vita3K/external/stb
        external/Vita3K/external/LibAtrac9/C/src
        external/Vita3K/vita3k/renderer/include
    LIBS
        shader glslang spirv-cross-glsl spirv-cross-core fmt-header-only
        m pthread ${CMAKE_DL_LIBS}
    ARGUMENTS ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders)
