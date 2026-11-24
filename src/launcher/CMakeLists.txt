cmake_minimum_required(VERSION 3.18)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(v2n-launcher LANGUAGES C CXX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()
# Pinned submodules keep native and cross builds reproducible and offline.
set(TOMLPLUSPLUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/tomlplusplus")
set(LUNASVG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/lunasvg")
if(NOT EXISTS "${TOMLPLUSPLUS_DIR}/include/toml++/toml.hpp" OR
   NOT EXISTS "${LUNASVG_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Launcher dependencies missing; run git submodule update --init --recursive")
endif()
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${LUNASVG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lunasvg" EXCLUDE_FROM_ALL)
# Link the shared package extraction pipeline into the launcher.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../package"
                 "${CMAKE_CURRENT_BINARY_DIR}/package")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
set(NANOVG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/nanovg/src")
if(NOT EXISTS "${NANOVG_DIR}/nanovg.c")
    message(FATAL_ERROR "NanoVG submodule missing; run git submodule update --init --recursive")
endif()
function(embed_asset file name)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/${name}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_asset.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/${file}" "${header}" "${name}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${file}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_asset.py" VERBATIM)
    set(EMBEDDED_ASSETS ${EMBEDDED_ASSETS} "${header}" PARENT_SCOPE)
endfunction()
embed_asset(Inter-Regular.ttf inter_font)
embed_asset(Inter-SemiBold.ttf inter_bold)
foreach(i RANGE 0 3)
    embed_asset(cover-${i}.jpg cover_${i})
endforeach()
add_executable(v2n-launcher
    main.cpp
    config.cpp
    logo.cpp
    state.cpp
    input.cpp
    import.cpp
    ui.cpp
    nanovg_backend.cpp
    "${NANOVG_DIR}/nanovg.c"
    ${EMBEDDED_ASSETS})
target_compile_features(v2n-launcher PRIVATE cxx_std_17)
target_include_directories(v2n-launcher PRIVATE ${SDL2_INCLUDE_DIRS} "${CMAKE_CURRENT_BINARY_DIR}" "${NANOVG_DIR}")
target_include_directories(v2n-launcher SYSTEM PRIVATE "${TOMLPLUSPLUS_DIR}/include")
target_link_directories(v2n-launcher PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(v2n-launcher PRIVATE ${SDL2_LIBRARIES} v2n_extract_core Threads::Threads lunasvg::lunasvg)
if(APPLE)
    target_link_libraries(v2n-launcher PRIVATE "-framework OpenGL")
else()
    target_link_libraries(v2n-launcher PRIVATE GLESv2 m)
endif()
target_compile_options(v2n-launcher PRIVATE -Wall -Wextra -Wno-unused-function)
