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
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(VITA2NATIVE_DISPLAY_SOURCE source/modules/SceDisplay_linux.c)
    set(VITA2NATIVE_CTRL_SOURCE source/modules/SceCtrl_linux.c)
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
    ${VITA2NATIVE_CTRL_SOURCE}
    ${VITA2NATIVE_DISPLAY_SOURCE}
    source/modules/SceGxm.c
    source/modules/SceAppMgr.c
    source/modules/SceCommonDialog.c
    source/modules/SceKernelThreadMgr.c
    source/modules/SceLibKernel.c
    source/modules/SceNet.c
    source/modules/SceRtc.c
    source/modules/SceSharedFb.c
    source/modules/SceSysmodule.c
    source/modules/SceSysmem.c
    source/modules/SceTouch_linux.c
)

target_include_directories(vita2native PRIVATE
    include
    source/host
    external/khronos/include
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
