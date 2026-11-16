# Shared Linux cross-build pieces for the zig wrapper toolchains.
#
# Included by zig-arm.cmake and zig-x64.cmake after they have set
# VITA2NATIVE_DEBIAN_ARCH and VITA2NATIVE_MULTIARCH and selected their compiler
# wrappers. Everything a target needs from its packages lives here, so the two
# architectures cannot drift apart in how they find them.
#
# The sysroot is a derived artifact: tools/debian-sysroot.py writes one per
# Debian architecture under build/, and tools/sysroots/<arch>.lock records the
# exact package versions it resolved. Nothing in it is source.

get_filename_component(_v2n_source_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# An explicitly set sysroot always wins; the default only fills in an empty one,
# because a plain CACHE set would not replace a cache entry from an earlier
# configure.
if(NOT VITA2NATIVE_SYSROOT)
    set(VITA2NATIVE_SYSROOT "${_v2n_source_dir}/build/sysroot-${VITA2NATIVE_DEBIAN_ARCH}"
        CACHE PATH "Cross sysroot with the target's Debian libraries and headers" FORCE)
endif()

if(NOT EXISTS "${VITA2NATIVE_SYSROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "No sysroot for ${VITA2NATIVE_DEBIAN_ARCH} at ${VITA2NATIVE_SYSROOT}.\n"
        "Populate it with:\n"
        "    tools/debian-sysroot.py --arch ${VITA2NATIVE_DEBIAN_ARCH}\n"
        "(or --all for every architecture the presets use)")
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
