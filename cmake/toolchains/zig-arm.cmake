# 32-bit ARM (armhf) Linux cross build using zig's bundled clang and libc.
#
# Generate the wrappers this file points at with:
#   tools/zig-toolchain.sh arm
# then configure with `cmake --preset arm-rel` (or arm-debug, arm-minsize).
#
# VITA2NATIVE_ZIG_DIR defaults to <source>/build, which is where the helper
# script writes by default.

if(NOT DEFINED VITA2NATIVE_ZIG_DIR)
    set(VITA2NATIVE_ZIG_DIR "${CMAKE_SOURCE_DIR}/build")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cc")
set(CMAKE_CXX_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cxx")
set(CMAKE_AR "${VITA2NATIVE_ZIG_DIR}/zig-arm-ar")
set(CMAKE_RANLIB "${VITA2NATIVE_ZIG_DIR}/zig-arm-ranlib")

# Optional cross sysroot: Debian armhf libraries and headers, populated by
# tools/debian-sysroot.py. Without it the build uses Zig's bundled libc, which
# is enough for the headless backend but cannot resolve -lSDL2.
set(VITA2NATIVE_SYSROOT "" CACHE PATH "Cross sysroot with the target's system libraries")
if(VITA2NATIVE_SYSROOT)
    set(CMAKE_SYSROOT "${VITA2NATIVE_SYSROOT}")
    # zig's clang keeps its own C and C++ library headers, so --sysroot does not
    # put the target's /usr/include on the search path; the headers the sysroot
    # exists for (X11, Wayland, SDL's generated config) have to be named. They
    # go in as -isystem, after zig's libc++ wrappers: -I here would put glibc's
    # <ctype.h> ahead of libc++'s, which then refuses to compile.
    if(EXISTS "${VITA2NATIVE_SYSROOT}/usr/include")
        string(APPEND CMAKE_C_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include")
        string(APPEND CMAKE_CXX_FLAGS_INIT " -isystem ${VITA2NATIVE_SYSROOT}/usr/include")
    endif()
    if(EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/arm-linux-gnueabihf")
        string(APPEND CMAKE_C_FLAGS_INIT
               " -isystem ${VITA2NATIVE_SYSROOT}/usr/include/arm-linux-gnueabihf")
        string(APPEND CMAKE_CXX_FLAGS_INIT
               " -isystem ${VITA2NATIVE_SYSROOT}/usr/include/arm-linux-gnueabihf")
    endif()
endif()

set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
