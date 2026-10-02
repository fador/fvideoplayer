#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace player::subtitle {

// One karaoke run and the time window it is sung in, relative to the cue
// start. The ASS karaoke tags (\k, \K, \kf, \ko) define these.
struct KaraokeSyllable {
    std::string text;
    double start = 0.0;  // seconds from the cue start
    double end = 0.0;
};

struct Cue {
    double start = 0.0;  // seconds
    double end = 0.0;    // seconds
    std::string text;    // plain text, '\n' between lines
    // ASS layout and karaoke data. Positions and sizes are in script
    // coordinates and are scaled with Track::play_res_x/play_res_y.
    bool has_position = false;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    int alignment = 2;                    // ASS \an numpad alignment
    float font_size = 0.0f;               // script units, 0 when unknown
    uint32_t primary_color = 0xFFFFFF;    // sung text
    uint32_t secondary_color = 0x999999;  // unsung text
    uint32_t outline_color = 0x000000;
    std::vector<KaraokeSyllable> karaoke;
};

// A raw subtitle payload as stored in a container.
struct SubtitleBlock {
    std::vector<uint8_t> data;
    int64_t pts_ms = 0;
    int64_t duration_ms = 0;  // 0 when unknown
};

struct Track {
    std::string name;      // display name
    std::string language;  // may be empty
    std::string codec;     // "srt", "ass", "ssa", "vtt", "sub", "S_TEXT/ASS", ...
    bool embedded = false;
    // ASS/SSA script resolution for scaling positions and font sizes
    // (0 when unknown).
    int play_res_x = 0;
    int play_res_y = 0;
    std::vector<Cue> cues;
};

// --- parsers ----------------------------------------------------------------

bool parseSrt(const std::string& data, std::vector<Cue>& out, std::string* error);
// track, when given, receives the script resolution.
bool parseAss(const std::string& data, std::vector<Cue>& out, std::string* error,
              Track* track = nullptr);
bool parseWebVtt(const std::string& data, std::vector<Cue>& out, std::string* error);
bool parseMicroDvd(const std::string& data, std::vector<Cue>& out, std::string* error);

// Parses the payloads of a text subtitle track stored in a container.
// codec is a Matroska codec id ("S_TEXT/UTF8", "S_TEXT/ASS", "S_TEXT/SSA")
// or "tx3g" for MP4 timed text. codec_private is the Matroska CodecPrivate
// (ASS header) when present.
bool parseEmbeddedTrack(const std::string& codec, const std::string& codec_private,
                        const std::vector<SubtitleBlock>& blocks, std::vector<Cue>& out,
                        std::string* error, Track* track = nullptr);

// Detects the format from the file extension/content and loads a subtitle file.
bool loadSubtitleFile(const std::filesystem::path& path, Track& out, std::string* error);

// Sorts cues and fills in missing end times from the following cue.
void finalizeCues(std::vector<Cue>& cues);

// Returns the cue active at the given time, or nullptr.
const Cue* findCue(const std::vector<Cue>& cues, double time);

// Every cue active at the given time, in start order. Overlapping ASS lines,
// such as karaoke syllables, are meant to be drawn together.
std::vector<const Cue*> findCues(const std::vector<Cue>& cues, double time);

}  // namespace player::subtitle
