add_executable(player_tests
    test_avi_demuxer.cpp
    test_hvcc.cpp
    test_mkv_demuxer.cpp
    test_mp4_demuxer.cpp
    test_raw_hevc_demuxer.cpp)
target_link_libraries(player_tests PRIVATE player_demux GTest::gtest_main)

if(MSVC)
    target_compile_options(player_tests PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

include(GoogleTest)
gtest_discover_tests(player_tests)
