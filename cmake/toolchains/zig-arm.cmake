# 32-bit ARM (armhf) Linux cross build using zig's bundled clang.
#
# Generate the wrappers this file points at with:
#   tools/zig-toolchain.sh arm
# then configure with `cmake --preset arm-rel` (or arm-debug, arm-minsize).
# The target's system libraries come from build/sysroot-armhf, populated by
# tools/debian-sysroot.py.

if(NOT DEFINED VITA2NATIVE_ZIG_DIR)
    set(VITA2NATIVE_ZIG_DIR "${CMAKE_SOURCE_DIR}/build")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cc")
set(CMAKE_CXX_COMPILER "${VITA2NATIVE_ZIG_DIR}/zig-arm-cxx")
set(CMAKE_AR "${VITA2NATIVE_ZIG_DIR}/zig-arm-ar")
set(CMAKE_RANLIB "${VITA2NATIVE_ZIG_DIR}/zig-arm-ranlib")

set(VITA2NATIVE_DEBIAN_ARCH armhf)
set(VITA2NATIVE_MULTIARCH arm-linux-gnueabihf)
include("${CMAKE_CURRENT_LIST_DIR}/zig-linux.cmake")

set(VITA2NATIVE_ARM_ARCH_FLAGS "" CACHE STRING "" FORCE)
