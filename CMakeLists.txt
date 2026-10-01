cmake_minimum_required(VERSION 3.25)

project(hevc_player
    VERSION 0.1.0
    DESCRIPTION "HEVC video player built on the kvzdec decoder"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PLAYER_BUILD_TESTS "Build player unit tests" ON)

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)

# --- kvzdec: static library only -------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_KVZDEC_BINARY OFF CACHE BOOL "" FORCE)
set(GIT_SUBMODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(external/kvzdec EXCLUDE_FROM_ALL)

# --- SDL3: static, no tests/examples/install -------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

# --- Dear ImGui (core + SDL3/OpenGL3 backends) -----------------------------
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_sdl3.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# --- ImPlot -----------------------------------------------------------------
add_library(implot STATIC
    external/implot/implot.cpp
    external/implot/implot_items.cpp
    external/implot/implot_demo.cpp)
target_include_directories(implot PUBLIC external/implot)
target_link_libraries(implot PUBLIC imgui)

# --- player -----------------------------------------------------------------
add_library(player_core STATIC
    src/app/Application.cpp)
target_include_directories(player_core PUBLIC src)
target_link_libraries(player_core PUBLIC kvzdec imgui implot SDL3::SDL3 OpenGL::GL Threads::Threads)

if(MSVC)
    target_compile_options(player_core PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(player_core PUBLIC $<$<CONFIG:Release>:/O2>)
endif()

add_executable(hevc_player src/main.cpp)
target_link_libraries(hevc_player PRIVATE player_core)

if(PLAYER_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
