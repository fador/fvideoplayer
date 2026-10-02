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

TEST(SubtitleAss, ParsesKaraokeStylesAndPosition) {
    const std::string data =
        "[Script Info]\n"
        "PlayResX: 1280\n"
        "PlayResY: 720\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Kara,SomeFont,48,&H00FFFFFF,&H0000FFFF,&H00123456,&H00000000,0,0,0,0,100,100,"
        "0,0,1,2,0,2,10,10,10,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:05.00,Kara,,0,0,0,,"
        "{\\pos(640,600)\\an5}{\\k50}ka{\\k30}ra{\\k20}o{\\k40}ke\n";

    player::subtitle::Track track;
    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, &error, &track)) << error;
    EXPECT_EQ(track.play_res_x, 1280);
    EXPECT_EQ(track.play_res_y, 720);
    ASSERT_EQ(cues.size(), 1u);

    const Cue& cue = cues[0];
    EXPECT_EQ(cue.text, "karaoke");
    EXPECT_TRUE(cue.has_position);
    EXPECT_FLOAT_EQ(cue.pos_x, 640.0f);
    EXPECT_FLOAT_EQ(cue.pos_y, 600.0f);
    EXPECT_EQ(cue.alignment, 5);  // \an5 overrides the style's 2
    EXPECT_FLOAT_EQ(cue.state.font_size, 48.0f);
    EXPECT_EQ(cue.state.primary, 0xFFFFFFu);
    EXPECT_EQ(cue.state.secondary, 0xFFFF00u);  // &H0000FFFF is BGR
    EXPECT_EQ(cue.state.outline, 0x563412u);

    // The karaoke runs carry the timing of each syllable.
    ASSERT_EQ(cue.karaoke.size(), 4u);
    EXPECT_EQ(cue.karaoke[0].text, "ka");
    EXPECT_DOUBLE_EQ(cue.karaoke[0].start, 0.0);
    EXPECT_DOUBLE_EQ(cue.karaoke[0].end, 0.5);
    EXPECT_EQ(cue.karaoke[1].text, "ra");
    EXPECT_DOUBLE_EQ(cue.karaoke[1].start, 0.5);
    EXPECT_DOUBLE_EQ(cue.karaoke[1].end, 0.8);
    EXPECT_EQ(cue.karaoke[2].text, "o");
    EXPECT_DOUBLE_EQ(cue.karaoke[3].start, 1.0);
    EXPECT_DOUBLE_EQ(cue.karaoke[3].end, 1.4);
}

TEST(SubtitleAss, SkipsCommentLines) {
    // Aegisub keeps its karaoke templates in Comment lines; they are disabled
    // and must not show up as subtitles.
    const std::string data =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Comment: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\k50}hidden\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,shown\n";
    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 1u);
    EXPECT_EQ(cues[0].text, "shown");
}

TEST(SubtitleLookup, FindsEveryOverlappingCue) {
    // Karaoke typesetting stacks several lines on top of each other, so the
    // lookup has to return all of them, not just the newest.
    std::vector<Cue> cues(3);
    cues[0].start = 0.0;
    cues[0].end = 10.0;
    cues[0].text = "long";
    cues[1].start = 1.0;
    cues[1].end = 2.0;
    cues[1].text = "short";
    cues[2].start = 1.5;
    cues[2].end = 3.0;
    cues[2].text = "overlap";

    std::vector<const Cue*> active = player::subtitle::findCues(cues, 1.6);
    ASSERT_EQ(active.size(), 3u);
    EXPECT_EQ(active[0]->text, "long");
    EXPECT_EQ(active[1]->text, "short");
    EXPECT_EQ(active[2]->text, "overlap");

    active = player::subtitle::findCues(cues, 2.5);
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0]->text, "long");
    EXPECT_EQ(active[1]->text, "overlap");

    active = player::subtitle::findCues(cues, 0.5);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0]->text, "long");

    EXPECT_TRUE(player::subtitle::findCues(cues, 10.0).empty());
}

TEST(SubtitleAss, KeepsStyleMarginsAndAlignment) {
    // A line without \pos is placed by its alignment and margins, so the
    // renderer needs both.
    const std::string data =
        "[Script Info]\n"
        "PlayResX: 1920\n"
        "PlayResY: 1080\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Eng,SomeFont,45,&H00FFFFFF,&H40999999,&H00863C32,&H00863C32,-1,0,0,0,100,100,"
        "0,0,1,2.5,0,1,64,48,24,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,Eng,,0,0,0,,Bottom left\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Eng,,10,0,0,,Margin override\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 2u);
    EXPECT_EQ(cues[0].alignment, 1);
    EXPECT_FLOAT_EQ(cues[0].margin_l, 64.0f);
    EXPECT_FLOAT_EQ(cues[0].margin_r, 48.0f);
    EXPECT_FLOAT_EQ(cues[0].margin_v, 24.0f);
    EXPECT_FALSE(cues[0].has_position);

    // A non-zero event margin wins over the style's; zero keeps the style's.
    EXPECT_FLOAT_EQ(cues[1].margin_l, 10.0f);
    EXPECT_FLOAT_EQ(cues[1].margin_r, 48.0f);
}

TEST(SubtitleAss, EvaluatesTransitionsAndFades) {
    // The pattern the karaoke track uses: the outline flashes white for 100 ms
    // and settles back, while the glyphs pop to 120% and shrink again. The line
    // also fades in over 500 ms.
    const std::string data =
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
        "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Kara,SomeFont,45,&H00FFFFFF,&H40999999,&H00863C32,&H00000000,0,0,0,0,100,100,"
        "0,0,1,2.5,0,5,64,64,32,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:10.00,0:00:12.00,Kara,,0,0,0,,"
        "{\\fad(500,0)\\blur3\\t(0,100,\\fscx120\\fscy120\\3c&HFFFFFF&)"
        "\\t(100,300,\\fscx100\\fscy100\\3c&H00863C32&)}ka\n";

    std::vector<Cue> cues;
    std::string error;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, &error)) << error;
    ASSERT_EQ(cues.size(), 1u);
    const Cue& cue = cues[0];
    EXPECT_TRUE(cue.fade.present);
    ASSERT_EQ(cue.transitions.size(), 2u);
    EXPECT_FLOAT_EQ(cue.state.border, 2.5f);
    EXPECT_FLOAT_EQ(cue.state.blur, 3.0f);

    // 100 ms in: the first transition just finished, so the outline is white
    // and the glyphs are at 120%; the fade is a fifth of the way through.
    const player::subtitle::AssRenderState peak = player::subtitle::evaluateAssCue(cue, 10.1);
    EXPECT_NEAR(peak.scale_x, 120.0f, 0.01f);
    EXPECT_EQ(peak.outline, 0xFFFFFFu);
    EXPECT_NEAR(peak.text_opacity, 0.2f, 0.01f);

    // 250 ms in: the second transition is three quarters of the way back.
    const player::subtitle::AssRenderState middle =
        player::subtitle::evaluateAssCue(cue, 10.25);
    EXPECT_NEAR(middle.scale_x, 105.0f, 0.01f);
    EXPECT_NEAR(middle.text_opacity, 0.5f, 0.01f);
    EXPECT_GT(middle.outline, 0x323C86u);
    EXPECT_LT(middle.outline, 0xFFFFFFu);

    // 600 ms in: transitions are done and the fade has finished.
    const player::subtitle::AssRenderState late = player::subtitle::evaluateAssCue(cue, 10.6);
    EXPECT_FLOAT_EQ(late.text_opacity, 1.0f);
    EXPECT_FLOAT_EQ(late.scale_x, 100.0f);
    EXPECT_EQ(late.outline, 0x323C86u);
    EXPECT_FLOAT_EQ(late.outline_opacity, 1.0f);
}

TEST(SubtitleAss, MovesPositionAndFadesOut) {
    const std::string data =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,"
        "{\\move(100,200,300,400,0,1000)\\fad(0,500)}text\n";

    std::vector<Cue> cues;
    ASSERT_TRUE(player::subtitle::parseAss(data, cues, nullptr));
    ASSERT_EQ(cues.size(), 1u);
    const Cue& cue = cues[0];
    EXPECT_TRUE(cue.has_move);

    const player::subtitle::AssRenderState start = player::subtitle::evaluateAssCue(cue, 1.0);
    EXPECT_FLOAT_EQ(start.pos_x, 100.0f);
    EXPECT_FLOAT_EQ(start.pos_y, 200.0f);

    const player::subtitle::AssRenderState middle = player::subtitle::evaluateAssCue(cue, 1.5);
    EXPECT_FLOAT_EQ(middle.pos_x, 200.0f);
    EXPECT_FLOAT_EQ(middle.pos_y, 300.0f);

    const player::subtitle::AssRenderState end = player::subtitle::evaluateAssCue(cue, 2.9);
    EXPECT_FLOAT_EQ(end.pos_x, 300.0f);
    EXPECT_FLOAT_EQ(end.pos_y, 400.0f);
    // \fad(0,500) fades out over the last half second.
    EXPECT_NEAR(end.text_opacity, 0.2f, 0.01f);
}

