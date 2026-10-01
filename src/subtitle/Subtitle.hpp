#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace player::subtitle {

struct Cue {
    double start = 0.0;  // seconds
    double end = 0.0;    // seconds
    std::string text;    // plain text, '\n' between lines
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
    // ASS/SSA layout hints for font scaling (0 when unknown).
    int play_res_y = 0;
    int default_font_size = 0;
    std::vector<Cue> cues;
};

// --- parsers ----------------------------------------------------------------

bool parseSrt(const std::string& data, std::vector<Cue>& out, std::string* error);
bool parseAss(const std::string& data, std::vector<Cue>& out, std::string* error);
bool parseWebVtt(const std::string& data, std::vector<Cue>& out, std::string* error);
bool parseMicroDvd(const std::string& data, std::vector<Cue>& out, std::string* error);

// Parses the payloads of a text subtitle track stored in a container.
// codec is a Matroska codec id ("S_TEXT/UTF8", "S_TEXT/ASS", "S_TEXT/SSA")
// or "tx3g" for MP4 timed text. codec_private is the Matroska CodecPrivate
// (ASS header) when present.
bool parseEmbeddedTrack(const std::string& codec, const std::string& codec_private,
                        const std::vector<SubtitleBlock>& blocks, std::vector<Cue>& out,
                        std::string* error);

// Detects the format from the file extension/content and loads a subtitle file.
bool loadSubtitleFile(const std::filesystem::path& path, Track& out, std::string* error);

// Sorts cues and fills in missing end times from the following cue.
void finalizeCues(std::vector<Cue>& cues);

// Returns the cue active at the given time, or nullptr.
const Cue* findCue(const std::vector<Cue>& cues, double time);

}  // namespace player::subtitle
