cmake_minimum_required(VERSION 3.13)

set(VITA2NATIVE_MAJOR 0)
set(VITA2NATIVE_MINOR 4)
set(VITA2NATIVE_PATCH 0)
execute_process(COMMAND
    git describe --dirty --always --exclude '*'
    OUTPUT_VARIABLE VITA2NATIVE_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

project(
    vita2native
    VERSION ${VITA2NATIVE_MAJOR}.${VITA2NATIVE_MINOR}.${VITA2NATIVE_PATCH}
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VITA2NATIVE_HEADLESS
    "Build without SDL2 (null display/input; useful for local testing and QEMU)" OFF)

if(VITA2NATIVE_HEADLESS)
    set(VITA2NATIVE_DISPLAY_SOURCE source/modules/SceDisplay_null.c)
    set(VITA2NATIVE_CTRL_SOURCE source/modules/SceCtrl_null.c)
    set(VITA2NATIVE_AUDIO_SOURCE source/modules/SceAudio_null.c)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(VITA2NATIVE_DISPLAY_SOURCE source/modules/SceDisplay_linux.c)
    set(VITA2NATIVE_CTRL_SOURCE source/modules/SceCtrl_linux.c)
    set(VITA2NATIVE_AUDIO_SOURCE source/modules/SceAudio_sdl.c)
endif()

option(VITA2NATIVE_ALLOW_64BIT
    "Allow 64-bit builds for syntax checks (cannot execute Vita code)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT VITA2NATIVE_ALLOW_64BIT)
    message(FATAL_ERROR
        "vita2native requires a 32-bit ARM target. "
        "Pass -DVITA2NATIVE_ALLOW_64BIT=ON only for syntax checks.")
endif()

add_subdirectory(external)

add_executable(vita2native
    source/host/host_fs.c
    source/host/host_gfx.c
    source/host/host_gfx_gl.c
    source/host/host_gfx_null.c
    source/host/host_jit.c
    source/host/host_thread.c
    external/Vita3K/external/miniz/miniz.c
    source/gxm_texture.c
    source/gxm_texture_pvrtc.cpp
    external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
    source/load.c
    source/log.c
    source/main.c
    source/module.c
    source/netlog.c
    source/spirv_cross_glsl_es.cpp
    source/util.c
    source/vita3k_shader_recompiler_iface_c.cpp
    source/vpk.c

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
    ${VITA2NATIVE_CTRL_SOURCE}
    source/modules/SceCtrl_common.c
    ${VITA2NATIVE_DISPLAY_SOURCE}
    ${VITA2NATIVE_AUDIO_SOURCE}
    source/modules/SceAudio.c
    source/modules/SceGxm.c
    source/modules/SceAppMgr.c
    source/modules/SceAtrac.c
    source/modules/SceAudiodec.c
    source/modules/SceAppUtil.c
    source/modules/SceCommonDialog.c
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
    source/modules/ScePgf.c
    source/modules/SceSqlite.c
    source/modules/SceIoAsync.c
    source/modules/SceLibClib.c
    source/modules/SceNet.c
    source/minimp3_impl.c
    source/modules/SceNgs.c
    source/modules/ScePower.c
    source/modules/SceRtc.c
    source/modules/SceSharedFb.c
    source/modules/SceShellUtil.c
    source/modules/SceSysmodule.c
    source/modules/SceSysmem.c
    source/modules/SceTouch_linux.c
)

target_include_directories(vita2native PRIVATE
    include
    source/host
    external/khronos/include
    external/minimp3
    external/font8x8
    external/Vita3K/external/LibAtrac9/C/src
    external/vita-headers/include
    external/Vita3K/vita3k/renderer/include
    ${SDL2_INCLUDE_DIRS}
)

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

set(VITA2NATIVE_ARM_ARCH_FLAGS
    "-march=armv7-a;-mfpu=neon;-mfloat-abi=hard;-mthumb"
    CACHE STRING "Extra ARM32 code-generation flags")

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
    ${SDL2_LIBRARIES}
    pthread
    dl
    m
)

# Keep symbols in the executable so the guest crash handler can print a useful
# backtrace.
target_link_options(vita2native PRIVATE -rdynamic)
set_target_properties(vita2native PROPERTIES ENABLE_EXPORTS ON)

# Do not use --gc-sections here: the export registry relies on
# __start_/__stop_ symbols for linker sections, and lld may garbage-collect
# them unless -z nostart-stop-gc is used.

include(CTest)
if(BUILD_TESTING)
    add_executable(host_smoke
        tests/host_smoke.c
        source/host/host_fs.c
        source/host/host_thread.c
    )
    target_include_directories(host_smoke PRIVATE include)
    target_compile_definitions(host_smoke PRIVATE
        _GNU_SOURCE=1
        _FILE_OFFSET_BITS=64
    )
    target_link_libraries(host_smoke PRIVATE pthread)
    add_test(NAME host_smoke COMMAND host_smoke)

    add_executable(gfx_smoke
        tests/gfx_smoke.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(gfx_smoke PRIVATE include external/khronos/include)
    target_compile_definitions(gfx_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gfx_smoke PRIVATE ${CMAKE_DL_LIBS} m)
    add_test(NAME gfx_smoke COMMAND gfx_smoke)

    add_executable(gl_thread_smoke
        tests/gl_thread_smoke.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(gl_thread_smoke PRIVATE include source/host external/khronos/include)
    target_compile_definitions(gl_thread_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gl_thread_smoke PRIVATE pthread ${CMAKE_DL_LIBS} m)
    add_test(NAME gl_thread_smoke COMMAND gl_thread_smoke)

    add_executable(audio_smoke
        tests/audio_smoke.c
        source/modules/SceAudio.c
        ${VITA2NATIVE_AUDIO_SOURCE}
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    )
    target_include_directories(audio_smoke PRIVATE include source/modules
        external/vita-headers/include)
    target_compile_definitions(audio_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_directories(audio_smoke PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(audio_smoke PRIVATE pthread m ${SDL2_LIBRARIES})
    add_test(NAME audio_smoke COMMAND audio_smoke)

    add_executable(sync_smoke
        tests/sync_smoke.c
        source/modules/SceKernelSync.c
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    )
    target_include_directories(sync_smoke PRIVATE include source/modules
        external/vita-headers/include external/mlib)
    target_compile_definitions(sync_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(sync_smoke PRIVATE pthread m)
    add_test(NAME sync_smoke COMMAND sync_smoke)

    add_executable(threadmgr_smoke
        tests/threadmgr_smoke.c
        source/modules/SceKernelThreadMgr.c
        source/modules/SceKernelSync.c
        source/modules/SceKernelLocks.c
        source/modules/SceKernelEvent.c
        source/host/host_thread.c
        source/log.c
        source/netlog.c
    )
    target_include_directories(threadmgr_smoke PRIVATE include source/modules
        external/vita-headers/include external/mlib)
    target_compile_definitions(threadmgr_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(threadmgr_smoke PRIVATE pthread m)
    add_test(NAME threadmgr_smoke COMMAND threadmgr_smoke)

    add_executable(audiodec_smoke
        tests/audiodec_smoke.c
        source/modules/SceAudiodec.c
        source/minimp3_impl.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
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
    target_include_directories(audiodec_smoke PRIVATE include source/modules
        external/vita-headers/include external/minimp3 external/Vita3K/external/LibAtrac9/C/src
        external/mlib)
    target_compile_definitions(audiodec_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(audiodec_smoke PRIVATE pthread m)
    add_test(NAME audiodec_smoke COMMAND audiodec_smoke)

    add_executable(atrac_smoke
        tests/atrac_smoke.c
        source/modules/SceAtrac.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
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
    target_include_directories(atrac_smoke PRIVATE include source/modules
        external/vita-headers/include external/Vita3K/external/LibAtrac9/C/src external/mlib)
    target_compile_definitions(atrac_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(atrac_smoke PRIVATE pthread m)
    add_test(NAME atrac_smoke COMMAND atrac_smoke)

    add_executable(apputil_smoke
        tests/apputil_smoke.c
        source/modules/SceAppUtil.c
        source/host/host_fs.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    )
    target_include_directories(apputil_smoke PRIVATE include source/modules
        external/vita-headers/include)
    target_compile_definitions(apputil_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(apputil_smoke PRIVATE pthread m)
    add_test(NAME apputil_smoke COMMAND apputil_smoke)

    add_executable(fios2_smoke
        tests/fios2_smoke.c
        source/modules/SceFios2.c
        source/host/host_fs.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    )
    target_include_directories(fios2_smoke PRIVATE include source/modules
        external/vita-headers/include)
    target_compile_definitions(fios2_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(fios2_smoke PRIVATE pthread m)
    add_test(NAME fios2_smoke COMMAND fios2_smoke)

    add_executable(pgf_smoke
        tests/pgf_smoke.c
        source/modules/ScePgf.c
    )
    target_include_directories(pgf_smoke PRIVATE include external/font8x8
        external/vita-headers/include)
    target_compile_definitions(pgf_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(pgf_smoke PRIVATE m)
    add_test(NAME pgf_smoke COMMAND pgf_smoke)

    add_executable(appmgr_smoke
        tests/appmgr_smoke.c
        source/modules/SceAppMgr.c
        source/host/host_fs.c
    )
    target_include_directories(appmgr_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(appmgr_smoke PRIVATE _GNU_SOURCE=1)
    add_test(NAME appmgr_smoke COMMAND appmgr_smoke)

    add_executable(aeabi_smoke
        tests/aeabi_smoke.c
        source/modules/SceLibKernelAeabi.c
    )
    target_include_directories(aeabi_smoke PRIVATE include)
    target_compile_definitions(aeabi_smoke PRIVATE _GNU_SOURCE=1)
    add_test(NAME aeabi_smoke COMMAND aeabi_smoke)

    add_executable(ngs_smoke
        tests/ngs_smoke.c
        source/modules/SceNgs.c
    )
    target_include_directories(ngs_smoke PRIVATE include)
    target_compile_definitions(ngs_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(ngs_smoke PRIVATE pthread)
    add_test(NAME ngs_smoke COMMAND ngs_smoke)

    add_executable(egl_window_smoke
        tests/egl_window_smoke.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(egl_window_smoke PRIVATE include source/host external/khronos/include)
    target_compile_definitions(egl_window_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(egl_window_smoke PRIVATE ${CMAKE_DL_LIBS} m)
    add_test(NAME egl_window_smoke COMMAND egl_window_smoke)

    if(NOT VITA2NATIVE_HEADLESS)
        add_executable(display_egl_smoke
            tests/display_egl_smoke.c
            source/modules/SceDisplay_linux.c
            source/host/host_gfx.c
            source/host/host_gfx_gl.c
            source/host/host_gfx_null.c
            source/host/host_thread.c
            source/log.c
            source/netlog.c
        )
        target_include_directories(display_egl_smoke PRIVATE
            include
            source/host
            external/khronos/include
    external/Vita3K/external/LibAtrac9/C/src
            external/vita-headers/include
            ${SDL2_INCLUDE_DIRS}
        )
        target_compile_definitions(display_egl_smoke PRIVATE
            _GNU_SOURCE=1
            _FILE_OFFSET_BITS=64
        )
        if(SDL2_LIBRARY_DIRS)
            target_link_directories(display_egl_smoke PRIVATE ${SDL2_LIBRARY_DIRS})
        endif()
        target_link_libraries(display_egl_smoke PRIVATE
            ${SDL2_LIBRARIES}
            ${CMAKE_DL_LIBS}
            m
            pthread
        )
        add_test(NAME display_egl_smoke COMMAND display_egl_smoke)
    endif()

    add_executable(shader_smoke
        tests/shader_smoke.c
        source/spirv_cross_glsl_es.cpp
        source/vita3k_shader_recompiler_iface_c.cpp
    )
    target_include_directories(shader_smoke PRIVATE
        include
        external/vita-headers/include
    )
    target_compile_definitions(shader_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(shader_smoke PRIVATE
        shader
        glslang
        spirv-cross-glsl
        spirv-cross-core
        fmt-header-only
        m
    )
    add_test(NAME shader_smoke_clear
        COMMAND shader_smoke
            ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders/clear_v.gxp)
    add_test(NAME shader_smoke_texture
        COMMAND shader_smoke
            ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders/texture_f.gxp)

    add_executable(texture_smoke
        tests/texture_smoke.c
        source/gxm_texture.c
        source/gxm_texture_pvrtc.cpp
        external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
    )
    target_include_directories(texture_smoke PRIVATE
        include
        external/vita-headers/include
        external/Vita3K/vita3k/renderer/include
    )
    target_compile_definitions(texture_smoke PRIVATE _GNU_SOURCE=1)
    add_test(NAME texture_smoke COMMAND texture_smoke)

    add_executable(gxm_draw_smoke
        tests/gxm_draw_smoke.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
        source/host/host_thread.c
        source/host/host_fs.c
        source/gxm_texture.c
        source/gxm_texture_pvrtc.cpp
        external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
        source/modules/SceGxm.c
        source/spirv_cross_glsl_es.cpp
        source/vita3k_shader_recompiler_iface_c.cpp
    )
    target_include_directories(gxm_draw_smoke PRIVATE
        include
        source/host
        external/khronos/include
    external/Vita3K/external/LibAtrac9/C/src
        external/vita-headers/include
        external/Vita3K/vita3k/renderer/include
    )
    target_compile_definitions(gxm_draw_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gxm_draw_smoke PRIVATE
        shader
        glslang
        spirv-cross-glsl
        spirv-cross-core
        fmt-header-only
        m
        pthread
        ${CMAKE_DL_LIBS}
    )
    add_test(NAME gxm_draw_smoke
        COMMAND gxm_draw_smoke
            ${CMAKE_SOURCE_DIR}/external/Vita3K/tools/native-tool/src/shaders)
endif()

install(TARGETS vita2native RUNTIME DESTINATION bin)
