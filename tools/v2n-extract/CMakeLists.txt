# v2n-extract - host-side PKG/PFS/SELF extractor.
#
# Host build only: it decrypts packages with OpenSSL and the psvpfstools engine,
# neither of which belongs in the ARM target. Build with tools/v2n-extract/build.sh
# (or `cmake -S tools/v2n-extract -B build/v2n-extract-host && cmake --build`).

cmake_minimum_required(VERSION 3.16)
project(v2n-extract LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

set(VITA3K "${CMAKE_CURRENT_SOURCE_DIR}/../../external/Vita3K")
if(NOT EXISTS "${VITA3K}/vita3k/packages/src/sce_utils.cpp")
    message(FATAL_ERROR "Vita3K submodule missing; run: git submodule update --init")
endif()

# The PFS layer (psvpfstools): zRIF -> klicensee, then file-by-file decryption.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${VITA3K}/external/psvpfstools" "${CMAKE_BINARY_DIR}/psvpfstools" EXCLUDE_FROM_ALL)

# The SELF layer: Vita3K's decrypt_fself() plus its built-in key store.
#
# compat/ shadows <util/log.h> with a version that does not need spdlog, which
# is the only reason this translation unit would drag in the emulator.
add_library(v2n_engines STATIC
    "${VITA3K}/vita3k/packages/src/sce_utils.cpp"
    "${VITA3K}/vita3k/util/src/string_utils.cpp"
    "${VITA3K}/external/miniz/miniz.c"
)
target_include_directories(v2n_engines PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${VITA3K}/vita3k/packages/include"
    "${VITA3K}/vita3k/util/include"
    "${VITA3K}/external/vita-toolchain/src"
    "${VITA3K}/external/miniz"
    "${VITA3K}/external/libfat16/include"
    "${VITA3K}/external/fmt/include"
)
target_compile_definitions(v2n_engines PUBLIC FMT_HEADER_ONLY=1)
target_compile_options(v2n_engines PRIVATE -w)  # third-party code
target_link_libraries(v2n_engines PUBLIC OpenSSL::Crypto ZLIB::ZLIB psvpfsparser)
# extract_fat() in this TU is unused here; do not require libfat16 at link time.
if(APPLE)
    target_link_options(v2n_engines PUBLIC -Wl,-dead_strip)
else()
    target_compile_options(v2n_engines PRIVATE -ffunction-sections)
    target_link_options(v2n_engines PUBLIC -Wl,--gc-sections)
endif()

add_executable(v2n-extract
    src/main.cpp
    src/pkg_reader.c
)
target_include_directories(v2n-extract PRIVATE src)
target_link_libraries(v2n-extract PRIVATE v2n_engines)
target_compile_options(v2n-extract PRIVATE -Wall -Wextra -Wno-unused-parameter)
