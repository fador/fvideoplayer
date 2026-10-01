#include "subtitle/Subtitle.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using player::subtitle::Cue;
using player::subtitle::SubtitleBlock;

TEST(SubtitleSrt, ParsesCuesAndStripsTags) {
    const std::string data =
        "\xEF\xBB\xBF"
        "1\r\n"
        "00:00:01,000 --> 00:00:04,500\r\n"
        "<i>Hello</i> &amp; welcome\r\n"
        "second line\r\n"
        "\r\n"
        "2\r\n"
        "00:00:05,000 --> 00:00:06,000\r\n"
        "Bye\r\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseSrt(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end, 4.5);
    EXPECT_EQ(cues[0].text, "Hello & welcome\nsecond line");
    EXPECT_DOUBLE_EQ(cues[1].start, 5.0);
    EXPECT_EQ(cues[1].text, "Bye");
}

TEST(SubtitleSrt, AcceptsDotMilliseconds) {
    const std::string data = "00:00:01.250 --> 00:00:02.750\nTest\n";
    std::vector<Cue> cues;
    ASSERT_TRUE(player::subtitle::parseSrt(data, cues, nullptr));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.25);
    EXPECT_DOUBLE_EQ(cues[0].end, 2.75);
}

TEST(SubtitleAss, ParsesDialogueWithCommasInText) {
    const std::string data =
        "[Script Info]\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize\n"
        "Style: Default,Arial,48\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:03.50,Default,,0,0,0,,{\\i1}Hello, world{\\i0}\\Nsecond\n"
        "Dialogue: 0,0:00:04.00,0:00:05.00,Default,,0,0,0,,Plain\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end, 3.5);
    EXPECT_EQ(cues[0].text, "Hello, world\nsecond");
    EXPECT_EQ(cues[1].text, "Plain");
}

TEST(SubtitleAss, ParsesSsaV4Format) {
    const std::string data =
        "[Events]\n"
        "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: Marked=0,0:00:02.00,0:00:04.00,Default,,0,0,0,,Ssa text\n";
    std::vector<Cue> cues;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, nullptr));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_DOUBLE_EQ(cues[0].start, 2.0);
    EXPECT_EQ(cues[0].text, "Ssa text");
}

TEST(SubtitleVtt, ParsesCuesAndSkipsNotes) {
    const std::string data =
        "WEBVTT\n"
        "\n"
        "NOTE this is a comment\n"
        "\n"
        "cue-1\n"
        "00:00:01.000 --> 00:00:03.000 align:middle\n"
        "<v Speaker>Hello</v>\n"
        "\n"
        "00:01:00.000 --> 00:01:02.000\n"
        "Later\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseWebVtt(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.0);
    EXPECT_EQ(cues[0].text, "Hello");
    EXPECT_DOUBLE_EQ(cues[1].start, 60.0);
    EXPECT_EQ(cues[1].text, "Later");
}

TEST(SubtitleMicroDvd, ParsesFramesWithFps) {
    const std::string data =
        "{1}{1}25.000\n"
        "{25}{50}Hello|world\n"
        "{75}{100}Bye\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseMicroDvd(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end, 2.0);
    EXPECT_EQ(cues[0].text, "Hello\nworld");
    EXPECT_DOUBLE_EQ(cues[1].start, 3.0);
}

TEST(SubtitleEmbedded, ParsesMatroskaAssBlocks) {
    SubtitleBlock first;
    const std::string text = "0,0,Default,,0,0,0,,Embedded line";
    first.data.assign(text.begin(), text.end());
    first.pts_ms = 1000;
    first.duration_ms = 2000;

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseEmbeddedTrack("S_TEXT/ASS", "", {first}, cues, &error))
        << error;
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_DOUBLE_EQ(cues[0].start, 1.0);
    EXPECT_DOUBLE_EQ(cues[0].end, 3.0);
    EXPECT_EQ(cues[0].text, "Embedded line");
}

TEST(SubtitleEmbedded, ParsesMatroskaUtf8Blocks) {
    SubtitleBlock first;
    const std::string text = "Plain subtitle";
    first.data.assign(text.begin(), text.end());
    first.pts_ms = 500;
    first.duration_ms = 1500;

    std::vector<Cue> cues;
    ASSERT_TRUE(player::subtitle::parseEmbeddedTrack("S_TEXT/UTF8", "", {first}, cues, nullptr));
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_DOUBLE_EQ(cues[0].start, 0.5);
    EXPECT_EQ(cues[0].text, "Plain subtitle");
}

TEST(SubtitleLookup, FindsActiveCue) {
    std::vector<Cue> cues = {
        {1.0, 2.0, "a"},
        {3.0, 4.0, "b"},
        {4.5, 6.0, "c"},
    };
    player::subtitle::finalizeCues(cues);
    EXPECT_EQ(player::subtitle::findCue(cues, 0.5), nullptr);
    ASSERT_NE(player::subtitle::findCue(cues, 1.5), nullptr);
    EXPECT_EQ(player::subtitle::findCue(cues, 1.5)->text, "a");
    EXPECT_EQ(player::subtitle::findCue(cues, 4.75)->text, "c");
    EXPECT_EQ(player::subtitle::findCue(cues, 10.0), nullptr);
}

TEST(SubtitleLookup, FillsMissingEndTimes) {
    std::vector<Cue> cues = {
        {0.0, 0.0, "first"},
        {2.0, 3.0, "second"},
    };
    player::subtitle::finalizeCues(cues);
    EXPECT_DOUBLE_EQ(cues[0].end, 2.0);
}

}  // namespace
