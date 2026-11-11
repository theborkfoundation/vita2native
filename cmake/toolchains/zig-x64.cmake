# 64-bit x86 Linux build using zig, for compile checks and running the smoke
# tests where a 32-bit ARM userland is not available. It cannot execute Vita
# code, so it always needs VITA2NATIVE_ALLOW_64BIT.
#
# Generate the wrappers this file points at with:
#   tools/zig-toolchain.sh x64
# then configure with `cmake --preset x64-syntax`.

if(NOT DEFINED VITA2NATIVE_ZIG_DIR)
    set(VITA2NATIVE_ZIG_DIR "${CMAKE_SOURCE_DIR}/build")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-x64-cc")
set(CMAKE_CXX_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-x64-cxx")
set(CMAKE_AR "${VITA2NATIVE_ZIG_DIR}/zig-x64-ar")
set(CMAKE_RANLIB "${VITA2NATIVE_ZIG_DIR}/zig-x64-ranlib")

set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
