# Third-party libraries compiled into the Linux/ARM32 target.

# Some vendored projects still declare cmake_minimum_required(VERSION 3.x)
# with x < 3.5; CMake 4 rejects that unless the policy floor is raised.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
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

# ---------------------------------------------------------------------------
# Vita3K shader recompiler (GXP -> SPIR-V) and SPIRV-Cross (SPIR-V -> GLSL ES)
# ---------------------------------------------------------------------------

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(Vita3K/external/fmt EXCLUDE_FROM_ALL)

# glslang provides SPIRV/SpvBuilder, used by Vita3K's recompiler.
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(Vita3K/external/glslang EXCLUDE_FROM_ALL)

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
add_subdirectory(Vita3K/external/SPIRV-Cross EXCLUDE_FROM_ALL)

# Headers that adapt Vita3K's utility layer to this project.
add_library(vita3k_shim INTERFACE)
target_include_directories(vita3k_shim INTERFACE
    ${PROJECT_SOURCE_DIR}/include/vita3k
    ${PROJECT_SOURCE_DIR}/external/vita-headers/include
)

add_library(features INTERFACE)
target_link_libraries(features INTERFACE vita3k_shim)
target_include_directories(features INTERFACE Vita3K/vita3k/features/include)

add_library(util INTERFACE)
target_link_libraries(util INTERFACE vita3k_shim fmt-header-only)
target_include_directories(util INTERFACE Vita3K/vita3k/util/include)

add_subdirectory(Vita3K/vita3k/gxm EXCLUDE_FROM_ALL)

# Apply local patches to the Vita3K submodule. Idempotent: if the patch is
# already applied, the reverse check succeeds and nothing happens.
set(VITA3K_PATCH_DIR ${PROJECT_SOURCE_DIR}/patches)
foreach(patch vita3k-arm32-alignment.patch)
    if(EXISTS ${VITA3K_PATCH_DIR}/${patch})
        execute_process(
            COMMAND git apply --reverse --check ${VITA3K_PATCH_DIR}/${patch}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Vita3K
            RESULT_VARIABLE VITA3K_PATCH_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT VITA3K_PATCH_APPLIED EQUAL 0)
            execute_process(
                COMMAND git apply ${VITA3K_PATCH_DIR}/${patch}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Vita3K
                RESULT_VARIABLE VITA3K_PATCH_RESULT
                ERROR_VARIABLE VITA3K_PATCH_ERROR)
            if(NOT VITA3K_PATCH_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to apply ${patch}: ${VITA3K_PATCH_ERROR}")
            endif()
            message(STATUS "Applied Vita3K patch: ${patch}")
        endif()
    endif()
endforeach()

# LibAtrac9 is its own repository inside the Vita3K submodule, so its patch is
# applied from that directory.
set(LIBATRAC9_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Vita3K/external/LibAtrac9)
foreach(patch libatrac9-bitreverse.patch)
    if(EXISTS ${VITA3K_PATCH_DIR}/${patch})
        execute_process(
            COMMAND git apply --reverse --check ${VITA3K_PATCH_DIR}/${patch}
            WORKING_DIRECTORY ${LIBATRAC9_DIR}
            RESULT_VARIABLE LIBATRAC9_PATCH_APPLIED
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT LIBATRAC9_PATCH_APPLIED EQUAL 0)
            execute_process(
                COMMAND git apply ${VITA3K_PATCH_DIR}/${patch}
                WORKING_DIRECTORY ${LIBATRAC9_DIR}
                RESULT_VARIABLE LIBATRAC9_PATCH_RESULT
                ERROR_VARIABLE LIBATRAC9_PATCH_ERROR)
            if(NOT LIBATRAC9_PATCH_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to apply ${patch}: ${LIBATRAC9_PATCH_ERROR}")
            endif()
            message(STATUS "Applied LibAtrac9 patch: ${patch}")
        endif()
    endif()
endforeach()

add_subdirectory(Vita3K/vita3k/shader EXCLUDE_FROM_ALL)

# The shim headers must win over Vita3K's own util headers (log/fs/string utils).
target_include_directories(gxm BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/include/vita3k)
target_include_directories(shader BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/include/vita3k)
