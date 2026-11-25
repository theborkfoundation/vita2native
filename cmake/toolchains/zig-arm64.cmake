# ARM64 Linux launcher cross-build using mrexodia/zig-cross and a Debian sysroot.
# The runtime itself is ARM32; configure source/launcher as a standalone project.
get_filename_component(_v2n_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_v2n_zig_cross "${_v2n_root}/external/zig-cross/cmake/zig-toolchain.cmake")
if(NOT EXISTS "${_v2n_zig_cross}")
    message(FATAL_ERROR "zig-cross is missing; run git submodule update --init --recursive")
endif()

# Bookworm's libraries target glibc 2.36, supported by the ARM64 test beds.
set(ZIG_TARGET "aarch64-linux-gnu.2.36" CACHE STRING "Zig target triple")
include("${_v2n_zig_cross}")

set(VITA2NATIVE_MULTIARCH "aarch64-linux-gnu" CACHE STRING "Debian multiarch triplet")
set(VITA2NATIVE_SYSROOT "${_v2n_root}/build/sysroot-arm64" CACHE PATH "ARM64 sysroot")
if(NOT EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR "No ARM64 sysroot at ${VITA2NATIVE_SYSROOT}; run tools/debian-sysroot.py --arch arm64")
endif()
set(CMAKE_SYSROOT "${VITA2NATIVE_SYSROOT}")
# Zig resolves `-L` relative to `--sysroot`, so CMake's absolute library
# directories would be doubled at link time. Keep the sysroot for compiling and
# for find_root_path, but let the linker see plain absolute paths.
set(CMAKE_SYSROOT_LINK "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Zig keeps its own libc++ headers; target C headers must come after those.
string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")
string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")

# CMake's host pkg-config must see target .pc files and rewrite their paths.
set(ENV{PKG_CONFIG_LIBDIR} "${VITA2NATIVE_SYSROOT}/usr/lib/${VITA2NATIVE_MULTIARCH}/pkgconfig:${VITA2NATIVE_SYSROOT}/usr/lib/pkgconfig:${VITA2NATIVE_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${VITA2NATIVE_SYSROOT}")
