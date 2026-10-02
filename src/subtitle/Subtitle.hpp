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

// Animatable ASS override state. Sizes are in script units and alphas run from
// 0 (opaque) to 255 (transparent), the way the format stores them.
struct AssState {
    float font_size = 0.0f;  // 0 keeps the style's size
    float scale_x = 100.0f;
    float scale_y = 100.0f;
    float border = 0.0f;  // outline width
    float shadow = 0.0f;
    float blur = 0.0f;
    int alpha1 = 0;  // text
    int alpha2 = 0;  // karaoke
    int alpha3 = 0;  // outline
    int alpha4 = 0;  // shadow
    uint32_t primary = 0xFFFFFF;
    uint32_t secondary = 0x999999;
    uint32_t outline = 0x000000;
    uint32_t back = 0x000000;
};

// Which fields a transition changes.
enum AssField : uint32_t {
    kAssFontSize = 1u << 0,
    kAssScaleX = 1u << 1,
    kAssScaleY = 1u << 2,
    kAssBorder = 1u << 3,
    kAssShadow = 1u << 4,
    kAssBlur = 1u << 5,
    kAssAlpha1 = 1u << 6,
    kAssAlpha2 = 1u << 7,
    kAssAlpha3 = 1u << 8,
    kAssAlpha4 = 1u << 9,
    kAssPrimary = 1u << 10,
    kAssSecondary = 1u << 11,
    kAssOutline = 1u << 12,
    kAssBack = 1u << 13,
};

// One \t block: the masked fields move from their current value to the target
// over [t1, t2], both in seconds from the cue start.
struct AssTransition {
    double t1 = 0.0;
    double t2 = 0.0;
    uint32_t mask = 0;
    AssState target;
};

// \fad(a,b) and \fade(...) resolved to four alpha stops. Alphas are 0
// (opaque) to 255 (transparent), times are seconds from the cue start.
struct AssFade {
    bool present = false;
    int a1 = 0;
    int a2 = 0;
    int a3 = 0;
    int a4 = 0;
    double t1 = 0.0;
    double t2 = 0.0;
    double t3 = 0.0;
    double t4 = 0.0;
};

// The evaluated state of a cue at one instant, which is all the renderer needs.
struct AssRenderState {
    float font_size = 0.0f;
    float scale_x = 100.0f;
    float scale_y = 100.0f;
    float border = 0.0f;
    float shadow = 0.0f;
    float blur = 0.0f;
    float text_opacity = 1.0f;     // 0..1, fades included
    float outline_opacity = 1.0f;
    float shadow_opacity = 1.0f;
    uint32_t primary = 0xFFFFFF;
    uint32_t secondary = 0x999999;
    uint32_t outline = 0x000000;
    uint32_t back = 0x000000;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    bool has_position = false;
    int alignment = 2;
};

struct Cue {
    double start = 0.0;  // seconds
    double end = 0.0;    // seconds
    std::string text;    // plain text, '\n' between lines
    // ASS layout and animation data. Positions and sizes are in script
    // coordinates and are scaled with Track::play_res_x/play_res_y.
    bool has_position = false;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    int alignment = 2;      // ASS \an numpad alignment
    std::string font_name;  // style font family, empty when unknown
    float margin_l = 0.0f;  // script units, for unpositioned lines
    float margin_r = 0.0f;
    float margin_v = 0.0f;
    std::vector<KaraokeSyllable> karaoke;
    AssState state;                    // style plus the static overrides
    std::vector<AssTransition> transitions;
    AssFade fade;
    bool has_move = false;
    float move_x1 = 0.0f;
    float move_y1 = 0.0f;
    float move_x2 = 0.0f;
    float move_y2 = 0.0f;
    double move_t1 = 0.0;
    double move_t2 = 0.0;
};

// Applies the transitions and fades of a cue at the given time.
AssRenderState evaluateAssCue(const Cue& cue, double time);

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
