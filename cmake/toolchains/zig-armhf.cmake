# 32-bit ARM (armhf) Linux cross build using mrexodia/zig-cross and a Debian
# sysroot. This is the runtime target; the launcher's primary target is ARM64.
# The target's system libraries come from build/sysroot-armhf, populated by
# tools/debian-sysroot.py and pinned by tools/sysroots/armhf.lock.
get_filename_component(_v2n_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_v2n_zig_cross "${_v2n_root}/external/zig-cross/cmake/zig-toolchain.cmake")
if(NOT EXISTS "${_v2n_zig_cross}")
    message(FATAL_ERROR "zig-cross is missing; run git submodule update --init --recursive")
endif()

# arm-linux-gnueabihf already selects ARMv7, NEON and the hard-float ABI, and
# the sysroot supplies the glibc the devices ship, so the triple pins no version.
set(ZIG_TARGET "arm-linux-gnueabihf" CACHE STRING "Zig target triple")
include("${_v2n_zig_cross}")

# Debian's multiarch triplet: where armhf packages put their libraries and some
# of their headers inside the sysroot (SDL2's .pc file is only in
# usr/lib/arm-linux-gnueabihf/pkgconfig).
set(VITA2NATIVE_MULTIARCH "arm-linux-gnueabihf" CACHE STRING "Debian multiarch triplet")

# An explicitly set sysroot always wins; the default only fills in an empty one,
# because a plain CACHE set would not replace an entry from an earlier configure.
if(NOT VITA2NATIVE_SYSROOT)
    set(VITA2NATIVE_SYSROOT "${_v2n_root}/build/sysroot-armhf"
        CACHE PATH "Cross sysroot with armhf libraries and headers" FORCE)
endif()

if(NOT EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "No armhf sysroot at ${VITA2NATIVE_SYSROOT}.\n"
        "Populate it with:\n"
        "    tools/debian-sysroot.py")
endif()

set(CMAKE_SYSROOT "${VITA2NATIVE_SYSROOT}")

# Unlike the ARM64 launcher, this build hands the linker sysroot-relative -L
# directories: deps.cmake leaves PKG_CONFIG_SYSROOT_DIR unset on purpose, so zig
# prefixes them itself. CMAKE_SYSROOT_LINK therefore stays set; clearing it would
# turn those into host paths.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Zig keeps its own libc++ headers, so --sysroot alone does not put the target's
# /usr/include on the search path; the headers the sysroot exists for (SDL, X11,
# Wayland, generated config headers) have to be named. They go in as -isystem,
# after zig's libc++ wrappers: -I here would put glibc's <ctype.h> ahead of
# libc++'s, which then refuses to compile.
string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")
string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")

# CMake's host pkg-config must see target .pc files. PKG_CONFIG_SYSROOT_DIR is
# deliberately left unset: zig applies --sysroot to absolute -L paths itself, so
# pre-prefixing them here would search inside the sysroot twice.
set(ENV{PKG_CONFIG_LIBDIR} "${VITA2NATIVE_SYSROOT}/usr/lib/${VITA2NATIVE_MULTIARCH}/pkgconfig:${VITA2NATIVE_SYSROOT}/usr/lib/pkgconfig:${VITA2NATIVE_SYSROOT}/usr/share/pkgconfig")

# Clears any value an earlier configure left in the cache; the top-level set()
# cannot replace a cache entry.
set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)
