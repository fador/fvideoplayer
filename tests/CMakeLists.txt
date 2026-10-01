add_executable(player_tests
    test_audio.cpp
    test_celt_bands.cpp
    test_celt_filter.cpp
    test_celt_frame.cpp
    test_celt_post.cpp
    test_celt_vq.cpp
    test_mdct.cpp
    test_silk_nlsf.cpp
    test_avi_demuxer.cpp
    test_hvcc.cpp
    test_log.cpp
    test_mkv_demuxer.cpp
    test_mp4_demuxer.cpp
    test_nal_units.cpp
    test_raw_hevc_demuxer.cpp
    test_subtitle.cpp)
target_link_libraries(player_tests PRIVATE
    player_audio
    player_audio_sdl
    player_demux
    player_decode
    player_subtitle
    player_util
    GTest::gtest_main)

if(MSVC)
    target_compile_options(player_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

include(GoogleTest)
gtest_discover_tests(player_tests)
