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

# The SDL audio backend dlopens SDL, so it works for both builds: the ARM
# headless build has no SDL at link time but the target has it at runtime.
set(VITA2NATIVE_AUDIO_SOURCE source/modules/SceAudio_sdl.c)

option(VITA2NATIVE_ALLOW_64BIT
    "Allow 64-bit builds for syntax checks (cannot execute Vita code)" OFF)

option(VITA2NATIVE_NO_BACKTRACE_SYMBOLS
    "Strip dynamic symbols from the executable (smaller binary, but crash backtraces lose names)" OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT VITA2NATIVE_ALLOW_64BIT)
    message(FATAL_ERROR
        "vita2native requires a 32-bit ARM target. "
        "Pass -DVITA2NATIVE_ALLOW_64BIT=ON only for syntax checks.")
endif()

add_subdirectory(external)

# The export registry relies on __start_/__stop_ symbols for linker sections.
# LLD's start-stop-gc keeps the section only if it is otherwise referenced, so
# optimizing builds (which enable --gc-sections) would drop it; disable section
# GC for every target. This also covers Zig's optimizing configurations.
add_link_options("-Wl,--no-gc-sections")

add_executable(vita2native
    source/host/host_video_sdl.c
    source/host/host_fs.c
    source/host/host_psarc.c
    source/host/host_gfx.c
    source/host/host_gfx_gl.c
    source/host/host_gfx_null.c
    source/host/host_jit.c
    source/host/host_thread.c
    external/Vita3K/external/miniz/miniz.c
    external/sqlite/sqlite3.c
    source/gxm_texture.c
    source/gxm_texture_pvrtc.cpp
    source/gxm_texture_pvrtc_fast.c
    source/stb_image_impl.c
    source/stb_image_write_impl.c
    external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
    source/frame_profile.c
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
    source/modules/SceCtrl_script.c
    ${VITA2NATIVE_DISPLAY_SOURCE}
    ${VITA2NATIVE_AUDIO_SOURCE}
    source/modules/SceAudio.c
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
    source/modules/SceTouch_linux.c
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

# The amalgamation is third-party code with its own warning hygiene.
set_source_files_properties(external/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "-w")

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

include(GNUInstallDirs)

# FAAD2 (GPLv2) provides the AAC decoder. It is bundled as a static library and
# treated as third-party code for warning/sanitizer purposes.
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
install(TARGETS vita2native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/vita2native.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/x-vita-vpk.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES README.md licenses/COPYING.txt DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Do not use --gc-sections: the export registry relies on the linker sections
# above and that is handled once, for every target, near the top of the file.

# stb_image_write shifts a signed accumulator into the sign bit (well-defined
# on the target compilers in release builds, but Zig's debug instrumentation
# traps it). Treat the vendored stb translation units as third-party code.
set_source_files_properties(
    source/stb_image_impl.c
    source/stb_image_write_impl.c
    PROPERTIES COMPILE_OPTIONS "-fno-sanitize=undefined"
)

include(CTest)
if(BUILD_TESTING)
    # The smoke tests drive their setup from assert(), so NDEBUG must not
    # remove those calls in optimizing builds.
    add_compile_options(-UNDEBUG)

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

    # The vendored dlmalloc is third-party code; keep our warning flags out of it.
    set_source_files_properties(source/mspace.c PROPERTIES COMPILE_OPTIONS "-w")

    add_executable(mspace_smoke
        tests/mspace_smoke.c
        source/mspace.c
    )
    target_include_directories(mspace_smoke PRIVATE include)
    target_compile_definitions(mspace_smoke PRIVATE _GNU_SOURCE=1)
    add_test(NAME mspace_smoke COMMAND mspace_smoke)

    add_executable(ctrl_script_smoke
        tests/ctrl_script_smoke.c
        source/modules/SceCtrl_null.c
        source/modules/SceCtrl_common.c
        source/modules/SceCtrl_script.c
        source/host/host_video_sdl.c
    )
    target_include_directories(ctrl_script_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(ctrl_script_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(ctrl_script_smoke PRIVATE pthread ${CMAKE_DL_LIBS})
    add_test(NAME ctrl_script_smoke COMMAND ctrl_script_smoke)

    add_executable(regmgr_smoke
        tests/regmgr_smoke.c
        source/modules/SceRegistryMgr.c
    )
    target_include_directories(regmgr_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(regmgr_smoke PRIVATE _GNU_SOURCE=1)
    add_test(NAME regmgr_smoke COMMAND regmgr_smoke)

    add_executable(extra_imports_smoke
        tests/extra_imports_smoke.c
        source/module.c
        source/modules/SceAdhoc.c
        source/modules/SceMediaExport.c
        source/modules/ScePromoterUtil.c
        source/modules/SceUsbMtp.c
    )
    target_include_directories(extra_imports_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(extra_imports_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(extra_imports_smoke PRIVATE pthread)
    add_test(NAME extra_imports_smoke COMMAND extra_imports_smoke)

    add_executable(jpeg_smoke
        tests/jpeg_smoke.c
        source/module.c
        source/modules/SceJpeg.c
        source/stb_image_impl.c
        source/stb_image_write_impl.c
    )
    target_include_directories(jpeg_smoke PRIVATE
        include
        external/vita-headers/include
        external/Vita3K/external/stb
    )
    target_compile_definitions(jpeg_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(jpeg_smoke PRIVATE pthread m)
    add_test(NAME jpeg_smoke COMMAND jpeg_smoke)

    add_executable(camera_smoke
        tests/camera_smoke.c
        source/module.c
        source/modules/SceCamera.c
    )
    target_include_directories(camera_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(camera_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(camera_smoke PRIVATE pthread)
    add_test(NAME camera_smoke COMMAND camera_smoke)

    add_executable(gfx_smoke
        tests/gfx_smoke.c
        source/frame_profile.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(gfx_smoke PRIVATE include external/khronos/include)
    target_compile_definitions(gfx_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gfx_smoke PRIVATE ${CMAKE_DL_LIBS} m)
    add_test(NAME gfx_smoke COMMAND gfx_smoke)

    add_executable(gpu_bench
        tests/gpu_bench.c
        source/frame_profile.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(gpu_bench PRIVATE include source/host external/khronos/include)
    target_compile_definitions(gpu_bench PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gpu_bench PRIVATE ${CMAKE_DL_LIBS} m)

    add_executable(gl_thread_smoke
        tests/gl_thread_smoke.c
        source/frame_profile.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
    )
    target_include_directories(gl_thread_smoke PRIVATE include source/host external/khronos/include)
    target_compile_definitions(gl_thread_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(gl_thread_smoke PRIVATE pthread ${CMAKE_DL_LIBS} m)
    add_test(NAME gl_thread_smoke COMMAND gl_thread_smoke)

    # Interactive display check: presents a moving pattern through DRM/KMS or
    # fbdev. Not a ctest entry because it needs a real panel and runs until the
    # requested duration expires.
    add_executable(present_probe
        tests/present_probe.c
        source/host/host_video_sdl.c
    )
    target_include_directories(present_probe PRIVATE include external/vita-headers/include)
    target_compile_definitions(present_probe PRIVATE
        _GNU_SOURCE=1
        _FILE_OFFSET_BITS=64
    )

    # Virtual gamepad used to validate the evdev input path without touching
    # the hardware (needs /dev/uinput and usually root).
    add_executable(uinput_pad tests/uinput_pad.c)
    target_compile_definitions(uinput_pad PRIVATE _GNU_SOURCE=1)

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
        external/mlib external/faad2/include)
    target_compile_definitions(audiodec_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(audiodec_smoke PRIVATE pthread m faad)
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

    add_executable(fiber_smoke
        tests/fiber_smoke.c
        source/modules/SceFiber.c
        source/log.c
        source/netlog.c
        source/host/host_thread.c
    )
    target_include_directories(fiber_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(fiber_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(fiber_smoke PRIVATE pthread)
    add_test(NAME fiber_smoke COMMAND fiber_smoke)

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

    add_executable(sqlite_smoke
        tests/sqlite_smoke.c
        source/modules/SceSqlite.c
        external/sqlite/sqlite3.c
        source/host/host_fs.c
        source/host/host_thread.c
    )
    target_include_directories(sqlite_smoke PRIVATE include source/modules external/sqlite
        external/vita-headers/include)
    target_compile_definitions(sqlite_smoke PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
    target_link_libraries(sqlite_smoke PRIVATE pthread m)
    add_test(NAME sqlite_smoke COMMAND sqlite_smoke)

    add_executable(net_smoke
        tests/net_smoke.c
        source/modules/SceNet.c
        source/modules/SceNetCtl.c
        source/host/host_thread.c
    )
    target_include_directories(net_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(net_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(net_smoke PRIVATE pthread m)
    add_test(NAME net_smoke COMMAND net_smoke)

    add_executable(processmgr_smoke
        tests/processmgr_smoke.c
        source/module.c
        source/modules/SceProcessmgr.c
        source/host/host_thread.c
    )
    target_include_directories(processmgr_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(processmgr_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(processmgr_smoke PRIVATE pthread m)
    add_test(NAME processmgr_smoke COMMAND processmgr_smoke)

    add_executable(dialog_smoke
        tests/dialog_smoke.c
        source/modules/SceCommonDialog.c
        source/host/host_thread.c
    )
    target_include_directories(dialog_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(dialog_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(dialog_smoke PRIVATE pthread)
    add_test(NAME dialog_smoke COMMAND dialog_smoke)

    add_executable(dialog_overlay_smoke
        tests/dialog_overlay_smoke.c
        source/module.c
        source/modules/SceCommonDialog.c
        source/modules/common_dialog_overlay.c
        source/host/host_thread.c
    )
    target_include_directories(dialog_overlay_smoke PRIVATE
        include
        external/vita-headers/include
        external/font8x8
    )
    target_compile_definitions(dialog_overlay_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(dialog_overlay_smoke PRIVATE pthread)
    add_test(NAME dialog_overlay_smoke COMMAND dialog_overlay_smoke)

    add_executable(avplayer_smoke
        tests/avplayer_smoke.c
        source/module.c
        source/modules/SceAvPlayer.c
        source/host/host_thread.c
    )
    target_include_directories(avplayer_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(avplayer_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(avplayer_smoke PRIVATE pthread)
    add_test(NAME avplayer_smoke COMMAND avplayer_smoke)

    add_executable(touch_smoke
        tests/touch_smoke.c
        source/modules/SceTouch_linux.c
        source/host/host_thread.c
        source/host/host_video_sdl.c
    )
    target_include_directories(touch_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(touch_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(touch_smoke PRIVATE pthread m ${CMAKE_DL_LIBS})
    add_test(NAME touch_smoke COMMAND touch_smoke)

    add_executable(http_smoke
        tests/http_smoke.c
        source/modules/SceHttp.c
        source/modules/SceSsl.c
    )
    target_include_directories(http_smoke PRIVATE include external/vita-headers/include)
    target_compile_definitions(http_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(http_smoke PRIVATE pthread dl m)
    add_test(NAME http_smoke COMMAND http_smoke)

    add_executable(ngs_smoke
        tests/ngs_smoke.c
        source/modules/SceNgs.c
        source/modules/SceNgs_render.c
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
    target_include_directories(ngs_smoke PRIVATE include
        external/Vita3K/external/LibAtrac9/C/src)
    target_compile_definitions(ngs_smoke PRIVATE _GNU_SOURCE=1)
    target_link_libraries(ngs_smoke PRIVATE pthread)
    add_test(NAME ngs_smoke COMMAND ngs_smoke)

    add_executable(egl_window_smoke
        tests/egl_window_smoke.c
        source/frame_profile.c
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
            source/frame_profile.c
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
        source/gxm_texture_pvrtc_fast.c
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
        source/frame_profile.c
        source/host/host_gfx.c
        source/host/host_gfx_gl.c
        source/host/host_gfx_null.c
        source/host/host_thread.c
        source/host/host_fs.c
        source/gxm_texture.c
        source/gxm_texture_pvrtc.cpp
        source/gxm_texture_pvrtc_fast.c
        external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp
        source/modules/SceGxm.c
        source/spirv_cross_glsl_es.cpp
        source/vita3k_shader_recompiler_iface_c.cpp
        source/stb_image_write_impl.c
    )
    target_include_directories(gxm_draw_smoke PRIVATE
        include
        source/host
        external/khronos/include
        external/Vita3K/external/stb
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
