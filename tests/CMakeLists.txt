add_executable(player_tests
    test_ac3.cpp
    test_audio.cpp
    test_flac.cpp
    test_mp2.cpp
    test_mp3.cpp
    test_celt_bands.cpp
    test_celt_filter.cpp
    test_celt_frame.cpp
    test_celt_plc.cpp
    test_silk_plc.cpp
    test_celt_post.cpp
    test_celt_vq.cpp
    test_mdct.cpp
    test_opus_silk.cpp
    test_silk_core.cpp
    test_silk_indices.cpp
    test_silk_lpc.cpp
    test_silk_nlsf.cpp
    test_silk_params.cpp
    test_silk_parameters.cpp
    test_silk_stereo_resampler.cpp
    test_avi_demuxer.cpp
    test_filters.cpp
    test_flv1.cpp
    test_flv_demuxer.cpp
    test_hvcc.cpp
    test_log.cpp
    test_mkv_demuxer.cpp
    test_mp4_demuxer.cpp
    test_mpeg12.cpp
    test_mpeg12_demuxer.cpp
    test_mpeg2.cpp
    test_mpeg_ps_demuxer.cpp
    test_nal_units.cpp
    test_raw_hevc_demuxer.cpp
    test_subtitle.cpp)
target_link_libraries(player_tests PRIVATE
    player_audio
    player_audio_sdl
    player_demux
    player_decode
    player_filters
    player_playback
    player_subtitle
    player_util
    GTest::gtest_main)

if(MSVC)
    target_compile_options(player_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

include(GoogleTest)
gtest_discover_tests(player_tests)


