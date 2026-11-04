# Third-party libraries compiled into the Linux/ARM32 target.
#
# miniz is an INTERFACE library so CMake never invokes the host archiver on
# cross-compiled ELF objects; miniz.c is compiled directly into vita2native.

add_library(miniz INTERFACE)
target_include_directories(miniz INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Vita3K/external
    ${CMAKE_CURRENT_SOURCE_DIR}/Vita3K/external/miniz
)

add_library(mlib INTERFACE)
target_include_directories(mlib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/mlib)
