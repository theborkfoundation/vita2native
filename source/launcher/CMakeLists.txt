cmake_minimum_required(VERSION 3.18)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(v2n-launcher LANGUAGES C CXX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
endif()
# Pinned submodules keep native and cross builds reproducible and offline.
set(TOMLPLUSPLUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/tomlplusplus")
set(LUNASVG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/lunasvg")
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/lvgl")
set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/Vita3K/external/stb")
if(NOT EXISTS "${TOMLPLUSPLUS_DIR}/include/toml++/toml.hpp" OR
   NOT EXISTS "${LUNASVG_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${LVGL_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${STB_DIR}/stb_image.h")
    message(FATAL_ERROR "Launcher dependencies missing; run git submodule update --init --recursive")
endif()
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${LUNASVG_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lunasvg" EXCLUDE_FROM_ALL)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(lvgl PRIVATE -w)
endif()

# Link the shared package extraction pipeline into the launcher.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../package"
                 "${CMAKE_CURRENT_BINARY_DIR}/package")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
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
embed_asset(lucide.ttf lucide_font)
embed_asset(web/index.html web_index_html)
embed_asset(web/style.css web_style_css)
embed_asset(web/app.js web_app_js)
if(APPLE)
    enable_language(OBJCXX)
endif()
add_executable(v2n-launcher
    main.cpp
    config.cpp
    library.cpp
    logo.cpp
    state.cpp
    input.cpp
    import.cpp
    web_server.cpp
    ui.cpp
    stb_impl.c
    ${EMBEDDED_ASSETS})
if(APPLE)
    target_sources(v2n-launcher PRIVATE macos_activate.mm)
    target_link_libraries(v2n-launcher PRIVATE "-framework Cocoa")
endif()
set_source_files_properties(stb_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_compile_features(v2n-launcher PRIVATE cxx_std_17)
target_include_directories(v2n-launcher PRIVATE ${SDL2_INCLUDE_DIRS} "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(v2n-launcher SYSTEM PRIVATE "${TOMLPLUSPLUS_DIR}/include" "${STB_DIR}")
target_link_directories(v2n-launcher PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(v2n-launcher PRIVATE ${SDL2_LIBRARIES} v2n_extract_core Threads::Threads lunasvg::lunasvg lvgl::lvgl)
target_compile_options(v2n-launcher PRIVATE -Wall -Wextra -Wno-unused-function)
