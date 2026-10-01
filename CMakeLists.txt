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
if(NOT BUILD_SHARED_LIBS)
    # kvzdec's public header needs this to avoid __declspec(dllimport) on Windows.
    target_compile_definitions(kvzdec INTERFACE KVZDEC_STATIC_LIB)
endif()

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

# --- logging utilities -------------------------------------------------------
add_library(player_util STATIC
    src/utils/Log.cpp)
target_include_directories(player_util PUBLIC src)

# --- player demux (no GUI dependencies) --------------------------------------
add_library(player_demux STATIC
    src/demux/AviDemuxer.cpp
    src/demux/Demuxer.cpp
    src/demux/HvcC.cpp
    src/demux/MkvDemuxer.cpp
    src/demux/Mp4Demuxer.cpp
    src/demux/RawHevcDemuxer.cpp)
target_include_directories(player_demux PUBLIC src)
target_compile_definitions(player_demux PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- player decode (kvzdec wrapper, no GUI dependencies) --------------------
add_library(player_decode STATIC
    src/decode/DecodeDump.cpp
    src/decode/KvzdecDecoder.cpp
    src/decode/NalUnits.cpp)
target_include_directories(player_decode PUBLIC src)
target_link_libraries(player_decode PUBLIC player_demux kvzdec)
target_compile_definitions(player_decode PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- player playback (pipeline, no GUI dependencies) ------------------------
add_library(player_playback STATIC
    src/playback/PlaybackEngine.cpp
    src/stats/StatsCollector.cpp)
target_include_directories(player_playback PUBLIC src)
target_link_libraries(player_playback PUBLIC player_decode player_demux player_util Threads::Threads)

# --- player -----------------------------------------------------------------
add_library(player_core STATIC
    src/app/Application.cpp
    src/render/GLFunctions.cpp
    src/render/VideoRenderer.cpp)
target_include_directories(player_core PUBLIC src)
target_compile_definitions(player_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(player_core PUBLIC
    player_playback
    player_decode
    player_demux
    player_util
    kvzdec
    imgui
    implot
    SDL3::SDL3
    OpenGL::GL
    Threads::Threads)

if(MSVC)
    target_compile_options(player_demux PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(player_decode PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(player_playback PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(player_core PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

add_executable(hevc_player src/main.cpp)
target_link_libraries(hevc_player PRIVATE player_core)

# --- tests -------------------------------------------------------------------
if(PLAYER_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/googletest EXCLUDE_FROM_ALL)
    enable_testing()
    add_subdirectory(tests)
endif()
