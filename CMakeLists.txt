cmake_minimum_required(VERSION 3.13)

set(VITA2HOS_MAJOR 0)
set(VITA2HOS_MINOR 4)
set(VITA2HOS_PATCH 0)
execute_process(COMMAND
    git describe --dirty --always --exclude '*'
    OUTPUT_VARIABLE VITA2HOS_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

project(
    vita2hos
    VERSION ${VITA2HOS_MAJOR}.${VITA2HOS_MINOR}.${VITA2HOS_PATCH}
    LANGUAGES C CXX
)

if(NOT DEFINED VITA2NATIVE_LINUX)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(VITA2NATIVE_LINUX ON CACHE BOOL "Build the Linux/ARM32 host")
    else()
        set(VITA2NATIVE_LINUX OFF CACHE BOOL "Build the Linux/ARM32 host")
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(VITA2NATIVE_LINUX)

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
        source/load.c
        source/log.c
        source/main.c
        source/module.c
        source/netlog.c
        source/util.c
        source/vpk.c
        ${VITA2NATIVE_CTRL_SOURCE}
        ${VITA2NATIVE_DISPLAY_SOURCE}
        source/modules/SceGxm_linux.c
        source/modules/SceKernelThreadMgr.c
        source/modules/SceLibKernel.c
        source/modules/SceSysmem.c
        source/modules/SceTouch_linux.c
    )

    target_include_directories(vita2native PRIVATE
        include
        external/khronos/include
        external/vita-headers/include
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
        VITA2NATIVE_LINUX=1
        _GNU_SOURCE=1
        _FILE_OFFSET_BITS=64
        VITA2HOS_MAJOR="${VITA2HOS_MAJOR}"
        VITA2HOS_MINOR="${VITA2HOS_MINOR}"
        VITA2HOS_PATCH="${VITA2HOS_PATCH}"
        VITA2HOS_HASH="${VITA2HOS_HASH}"
    )

    if(SDL2_LIBRARY_DIRS)
        target_link_directories(vita2native PRIVATE ${SDL2_LIBRARY_DIRS})
    endif()

    target_link_libraries(vita2native PRIVATE
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
            VITA2NATIVE_LINUX=1
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
        target_compile_definitions(gfx_smoke PRIVATE
            VITA2NATIVE_LINUX=1
            _GNU_SOURCE=1
        )
        target_link_libraries(gfx_smoke PRIVATE ${CMAKE_DL_LIBS} m)
        add_test(NAME gfx_smoke COMMAND gfx_smoke)
    endif()

    install(TARGETS vita2native RUNTIME DESTINATION bin)

else()

    # Overwrite libnx location
    set(NX_ROOT ${DEVKITPRO}/libnx32)

    include(Platform/NintendoSwitch)

    set(VITA_HEADERS ${CMAKE_SOURCE_DIR}/external/vita-headers)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${NX_ARCH_SETTINGS} ${NX_COMMON_FLAGS} -idirafter ${VITA_HEADERS}/include")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${NX_ARCH_SETTINGS} ${NX_COMMON_FLAGS} -idirafter ${VITA_HEADERS}/include")

    add_compile_options(
        -Wall
        -Wimplicit-fallthrough=3
        -Wdouble-promotion
        -fstack-usage
        -fdata-sections
        -ffunction-sections
    )

    add_subdirectory(external)

    add_executable(vita2hos
        source/deko_utils.c
        source/load.c
        source/log.c
        source/main.c
        source/module.c
        source/netlog.c
        source/uam_compiler_iface_c.cpp
        source/util.c
        source/vita3k_shader_recompiler_iface_c.cpp
        source/modules/SceCtrl.c
        source/modules/SceDisplay.c
        source/modules/SceGxm.c
        source/modules/SceKernelThreadMgr.c
        source/modules/SceLibKernel.c
        source/modules/SceSysmem.c
        source/modules/SceTouch.c
    )

    target_include_directories(vita2hos PRIVATE
        include
        ${NX_ROOT}/include/uam/mesa-imported
    )

    target_compile_options(vita2hos PRIVATE
        -Wextra
        -Wno-unused-parameter
    )

    target_compile_definitions(vita2hos PRIVATE
        VITA2HOS_MAJOR="${VITA2HOS_MAJOR}"
        VITA2HOS_MINOR="${VITA2HOS_MINOR}"
        VITA2HOS_PATCH="${VITA2HOS_PATCH}"
        VITA2HOS_HASH="${VITA2HOS_HASH}"
    )

    target_link_libraries(vita2hos PRIVATE
        shader
        spirv-cross-core
        fmt
        uam
        miniz
        mlib
        $<$<CONFIG:Debug>:deko3dd>
        $<$<CONFIG:Release>:deko3d>
    )

    target_link_directories(vita2hos PRIVATE
        ${NX_ROOT}/lib
    )

    target_link_options(vita2hos PRIVATE
        -specs=${NX_ROOT}/switch32.specs
        -Wl,-Map,${CMAKE_PROJECT_NAME}.map
    )

    find_program(NX_ELF2NSO_EXE NAMES elf2nso HINTS "${DEVKITPRO}/tools/bin")
    find_program(NX_NPDMTOOL_EXE NAMES npdmtool HINTS "${DEVKITPRO}/tools/bin")
    find_program(NX_BUILD_PFS0_EXE NAMES build_pfs0 HINTS "${DEVKITPRO}/tools/bin")

    nx_create_exefs(${CMAKE_PROJECT_NAME}
        CONFIG ${CMAKE_PROJECT_NAME}.json
    )

    add_custom_target(send
        COMMAND curl -T ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.nsp --user switch:switch ftp://$ENV{SWITCHIP}:5000/vita2hos/vita2hos.nsp
        DEPENDS ${CMAKE_PROJECT_NAME}_nsp
    )

endif()
