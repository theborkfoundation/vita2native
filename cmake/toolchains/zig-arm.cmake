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

set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
