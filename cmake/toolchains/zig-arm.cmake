# 32-bit ARM (armhf) Linux cross build using zig's bundled clang.
#
# Generate the wrappers this file points at with:
#   tools/zig-toolchain.sh
# then configure with `cmake --preset release` (or debug, minsize).
# The target's system libraries come from build/sysroot-armhf, populated by
# tools/debian-sysroot.py.

if(NOT DEFINED VITA2NATIVE_ZIG_DIR)
    set(VITA2NATIVE_ZIG_DIR "${CMAKE_SOURCE_DIR}/build")
endif()

# A toolchain file is re-read inside try_compile(), where CMAKE_SOURCE_DIR points
# at the scratch project rather than this repository, so derive the root from the
# file's own location.
get_filename_component(_v2n_source_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cc")
set(CMAKE_CXX_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cxx")
set(CMAKE_AR "${VITA2NATIVE_ZIG_DIR}/zig-arm-ar")
set(CMAKE_RANLIB "${VITA2NATIVE_ZIG_DIR}/zig-arm-ranlib")

# Debian's multiarch triplet: where armhf packages put their libraries and some
# of their headers inside the sysroot (SDL2's .pc file is only in
# usr/lib/arm-linux-gnueabihf/pkgconfig). The sysroot itself is a derived
# artifact - tools/debian-sysroot.py writes it under build/, and
# tools/sysroots/armhf.lock records the exact versions it resolved.
set(VITA2NATIVE_MULTIARCH arm-linux-gnueabihf)

# An explicitly set sysroot always wins; the default only fills in an empty one,
# because a plain CACHE set would not replace a cache entry from an earlier
# configure.
if(NOT VITA2NATIVE_SYSROOT)
    set(VITA2NATIVE_SYSROOT "${_v2n_source_dir}/build/sysroot-armhf"
        CACHE PATH "Cross sysroot with armhf libraries and headers" FORCE)
endif()

if(NOT EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "No armhf sysroot at ${VITA2NATIVE_SYSROOT}.\n"
        "Populate it with:\n"
        "    tools/debian-sysroot.py")
endif()

set(CMAKE_SYSROOT "${VITA2NATIVE_SYSROOT}")

# zig's clang keeps its own C and C++ library headers, so --sysroot does not put
# the target's /usr/include on the search path; the headers the sysroot exists
# for (SDL, X11, Wayland, generated config headers) have to be named. They go in
# as -isystem, after zig's libc++ wrappers: -I here would put glibc's <ctype.h>
# ahead of libc++'s, which then refuses to compile.
if(EXISTS "${VITA2NATIVE_SYSROOT}/usr/include")
    string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include")
    string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include")
endif()
if(EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")
    string(APPEND CMAKE_C_FLAGS_INIT
           " -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")
    string(APPEND CMAKE_CXX_FLAGS_INIT
           " -isystem ${VITA2NATIVE_SYSROOT}/usr/include/${VITA2NATIVE_MULTIARCH}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Clears any value an earlier configure left in the cache; the top-level set()
# cannot replace a cache entry.
set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)
