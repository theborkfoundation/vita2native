cmake_minimum_required(VERSION 3.18)

# Keep the launcher self-contained: it links libcrypto into the binary rather
# than requiring a separate extractor or a target-side OpenSSL installation.
set(OPENSSL_USE_STATIC_LIBS TRUE)
# The Zig sysroot uses Debian multiarch paths that FindZLIB cannot infer from
# the arm compiler; point it at the locked static archive explicitly.
if(VITA2NATIVE_SYSROOT)
    set(ZLIB_INCLUDE_DIR "${VITA2NATIVE_SYSROOT}/usr/include" CACHE PATH "" FORCE)
    set(ZLIB_LIBRARY_RELEASE "${VITA2NATIVE_SYSROOT}/usr/lib/${VITA2NATIVE_MULTIARCH}/libz.a" CACHE FILEPATH "" FORCE)
endif()
find_package(ZLIB REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

set(VITA3K "${CMAKE_CURRENT_SOURCE_DIR}/../../external/Vita3K")
if(NOT EXISTS "${VITA3K}/vita3k/packages/src/sce_utils.cpp")
    message(FATAL_ERROR "Vita3K submodule missing; run: git submodule update --init")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${VITA3K}/external/psvpfstools" "${CMAKE_CURRENT_BINARY_DIR}/psvpfstools" EXCLUDE_FROM_ALL)
# The vendored target uses zlib but does not declare its link dependency.
target_link_libraries(libzRIF PUBLIC ZLIB::ZLIB)

# Vita3K's SELF decryptor and its built-in key store. compat/ replaces its
# logging header so this does not pull in the rest of Vita3K.
add_library(v2n_extract_engines STATIC
    "${VITA3K}/vita3k/packages/src/sce_utils.cpp"
    "${VITA3K}/vita3k/util/src/string_utils.cpp"
    "${VITA3K}/external/miniz/miniz.c")
target_include_directories(v2n_extract_engines PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${VITA3K}/vita3k/packages/include"
    "${VITA3K}/vita3k/util/include"
    "${VITA3K}/external/vita-toolchain/src"
    "${VITA3K}/external/miniz"
    "${VITA3K}/external/libfat16/include"
    "${VITA3K}/external/fmt/include")
target_compile_definitions(v2n_extract_engines PUBLIC FMT_HEADER_ONLY=1)
target_compile_options(v2n_extract_engines PRIVATE -w)
target_link_libraries(v2n_extract_engines PUBLIC OpenSSL::Crypto ZLIB::ZLIB psvpfsparser)

# extract_fat() in sce_utils.cpp is unused; discard it so libfat16 is not needed.
if(APPLE)
    target_link_options(v2n_extract_engines PUBLIC -Wl,-dead_strip)
else()
    target_compile_options(v2n_extract_engines PRIVATE -ffunction-sections)
    target_link_options(v2n_extract_engines PUBLIC -Wl,--gc-sections)
endif()

add_library(v2n_extract_core STATIC extract.cpp pkg_reader.c)
target_compile_features(v2n_extract_core PUBLIC cxx_std_17)
target_include_directories(v2n_extract_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(v2n_extract_core PUBLIC v2n_extract_engines)
target_compile_options(v2n_extract_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
