cmake_minimum_required(VERSION 3.13)

set(VITA2NATIVE_MAJOR 0)
set(VITA2NATIVE_MINOR 0)
set(VITA2NATIVE_PATCH 1)
execute_process(COMMAND
    git describe --dirty --always --exclude '*'
    OUTPUT_VARIABLE VITA2NATIVE_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# A tarball or a non-git tree leaves this empty, which would print as a bare
# trailing dash in --version.
if(NOT VITA2NATIVE_HASH)
    set(VITA2NATIVE_HASH "unknown")
endif()

project(
    vita2native
    VERSION ${VITA2NATIVE_MAJOR}.${VITA2NATIVE_MINOR}.${VITA2NATIVE_PATCH}
    LANGUAGES C CXX
)

# Delivered as compile definitions rather than a configured header so the
# version in the binary is always the one the build read. Global because
# source/cli.c reads them and is also linked into cli_smoke.
add_compile_definitions(
    VITA2NATIVE_MAJOR="${VITA2NATIVE_MAJOR}"
    VITA2NATIVE_MINOR="${VITA2NATIVE_MINOR}"
    VITA2NATIVE_PATCH="${VITA2NATIVE_PATCH}"
    VITA2NATIVE_HASH="${VITA2NATIVE_HASH}"
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VITA2NATIVE_NO_BACKTRACE_SYMBOLS
    "Strip dynamic symbols from the executable (smaller binary, but crash backtraces lose names)" OFF)

add_subdirectory(external)

# The export registry relies on __start_/__stop_ symbols for linker sections.
# LLD's start-stop-gc keeps the section only if it is otherwise referenced, so
# optimizing builds (which enable --gc-sections) would drop it; disable section
# GC for every target. This also covers Zig's optimizing configurations.
add_link_options("-Wl,--no-gc-sections")

set(VITA2NATIVE_ARM_ARCH_FLAGS
    "-march=armv7-a;-mfpu=neon;-mfloat-abi=hard;-mthumb"
    CACHE STRING "Extra ARM32 code-generation flags")

# Backends, vendored libraries and the vendored source lists.
include(cmake/deps.cmake)

# The emulator target.
include(cmake/sources.cmake)

# Smoke tests (skipped when BUILD_TESTING is off).
include(cmake/tests.cmake)

include(cmake/install.cmake)
