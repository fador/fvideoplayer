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

# --- subtitle parsing --------------------------------------------------------
add_library(player_subtitle STATIC
    src/subtitle/Subtitle.cpp)
target_include_directories(player_subtitle PUBLIC src)
target_compile_definitions(player_subtitle PRIVATE _CRT_SECURE_NO_WARNINGS)

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

# --- player audio (PCM helpers + from-scratch AAC decoder, no GUI deps) ------
add_library(player_audio STATIC
    src/audio/AudioBuffer.cpp
    src/audio/PcmGain.cpp
    src/audio/Simd.cpp
    src/audio/SimdAvx2.cpp
    src/audio/aac/AacConfig.cpp
    src/audio/aac/AacDecoder.cpp
    src/audio/aac/AacFilterbank.cpp
    src/audio/aac/AacIcs.cpp
    src/audio/aac/AacTables.cpp
    src/audio/aac/Huffman.cpp
    src/audio/aac/Mdct.cpp
    src/audio/aac/Window.cpp
    src/audio/opus/CeltAlloc.cpp
    src/audio/opus/CeltEnergy.cpp
    src/audio/opus/CeltPvq.cpp
    src/audio/opus/CeltBands.cpp
    src/audio/opus/CeltFilter.cpp
    src/audio/opus/CeltPost.cpp
    src/audio/opus/CeltStaticTables.cpp
    src/audio/opus/CeltVq.cpp
    src/audio/opus/CeltFrame.cpp
    src/audio/opus/CeltLpc.cpp
    src/audio/opus/KissFft.cpp
    src/audio/opus/Mdct.cpp
    src/audio/opus/SilkCore.cpp
    src/audio/opus/SilkDecoder.cpp
    src/audio/opus/SilkExcitation.cpp
    src/audio/opus/SilkGain.cpp
    src/audio/opus/SilkIndices.cpp
    src/audio/opus/SilkLpc.cpp
    src/audio/opus/SilkNlsf.cpp
    src/audio/opus/SilkParameters.cpp
    src/audio/opus/SilkPitch.cpp
    src/audio/opus/SilkResampler.cpp
    src/audio/opus/SilkStereo.cpp
    src/audio/opus/SilkTables.cpp
    src/audio/opus/OpusDecoder.cpp
    src/audio/opus/OpusHead.cpp
    src/audio/opus/OpusPacket.cpp
    src/audio/opus/OpusTables.cpp
    src/audio/opus/RangeDecoder.cpp)
target_include_directories(player_audio PUBLIC src)

# AVX2 kernels live in their own translation unit so it can be compiled with
# the instruction set enabled while the rest stays baseline; they are only
# called after simd::hasAvx2() (CPUID + XGETBV).
if(MSVC)
    set_source_files_properties(src/audio/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/audio/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()

# --- SDL3 audio output driver (kept separate so tests can link it) ----------
add_library(player_audio_sdl STATIC
    src/audio/SdlAudioSink.cpp)
target_include_directories(player_audio_sdl PUBLIC src)
target_link_libraries(player_audio_sdl PUBLIC player_audio player_util SDL3::SDL3)

# --- player playback (pipeline, no GUI dependencies) ------------------------
add_library(player_playback STATIC
    src/playback/PlaybackEngine.cpp
    src/stats/StatsCollector.cpp)
target_include_directories(player_playback PUBLIC src)
target_link_libraries(player_playback PUBLIC player_audio player_decode player_demux player_util Threads::Threads)

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
    player_subtitle
    player_util
    player_audio
    player_audio_sdl
    kvzdec
    imgui
    implot
    SDL3::SDL3
    OpenGL::GL
    Threads::Threads)

if(MSVC)
    target_compile_options(player_audio PRIVATE /W4 /permissive- /Zc:__cplusplus)
    target_compile_options(player_audio_sdl PRIVATE /W4 /permissive- /Zc:__cplusplus)
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
