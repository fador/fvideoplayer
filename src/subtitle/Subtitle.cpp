#include "subtitle/Subtitle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fstream>
#include <sstream>

namespace player::subtitle {
namespace {

bool startsWith(const std::string& text, const char* prefix) {
    const size_t length = std::strlen(prefix);
    return text.size() >= length && std::memcmp(text.data(), prefix, length) == 0;
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && static_cast<unsigned char>(text[begin]) <= ' ') {
        ++begin;
    }
    while (end > begin && static_cast<unsigned char>(text[end - 1]) <= ' ') {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string stripTrailingNulls(std::string text) {
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

std::string stripTags(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_tag = false;
    for (char c : text) {
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
        } else if (!in_tag) {
            out += c;
        }
    }
    return out;
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string decodeEntities(std::string text) {
    replaceAll(text, "&amp;", "&");
    replaceAll(text, "&lt;", "<");
    replaceAll(text, "&gt;", ">");
    replaceAll(text, "&quot;", "\"");
    replaceAll(text, "&apos;", "'");
    replaceAll(text, "&#39;", "'");
    replaceAll(text, "&nbsp;", " ");
    return text;
}

// Removes ASS override blocks and converts line/space escapes.
std::string stripAssText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{') {
            while (i < text.size() && text[i] != '}') {
                ++i;
            }
            continue;
        }
        if (c == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'N' || next == 'n') {
                out += '\n';
            } else if (next == 'h') {
                out += ' ';
            }
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

// Converts the escapes that survive into plain text: \N and \n are line
// breaks, \h is a hard space.
std::string decodeAssEscapes(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'N' || next == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (next == 'h') {
                out += ' ';
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string normalizeNewlines(std::string text) {
    replaceAll(text, "\r\n", "\n");
    replaceAll(text, "\r", "\n");
    return text;
}

bool parseSrtTime(const std::string& text, double& out) {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int millis = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d,%d", &hours, &minutes, &seconds, &millis) != 4 &&
        std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &seconds, &millis) != 4) {
        return false;
    }
    out = hours * 3600.0 + minutes * 60.0 + seconds + millis / 1000.0;
    return true;
}

bool parseAssTime(const std::string& text, double& out) {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int centis = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &seconds, &centis) != 4) {
        return false;
    }
    out = hours * 3600.0 + minutes * 60.0 + seconds + centis / 100.0;
    return true;
}

bool parseVttTime(const std::string& text, double& out) {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int millis = 0;
    const int fields = std::sscanf(text.c_str(), "%d:%d:%d.%d", &hours, &minutes, &seconds, &millis);
    if (fields == 4) {
        out = hours * 3600.0 + minutes * 60.0 + seconds + millis / 1000.0;
        return true;
    }
    if (std::sscanf(text.c_str(), "%d:%d.%d", &minutes, &seconds, &millis) == 3) {
        out = minutes * 60.0 + seconds + millis / 1000.0;
        return true;
    }
    return false;
}

// Splits "a,b,c" into at most max_fields parts; the last part keeps the rest.
std::vector<std::string> splitLimited(const std::string& text, char separator, size_t max_fields) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (fields.size() + 1 < max_fields) {
        const size_t pos = text.find(separator, start);
        if (pos == std::string::npos) {
            break;
        }
        fields.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    fields.push_back(text.substr(start));
    return fields;
}

int findField(const std::vector<std::string>& names, const char* wanted);

struct AssStyle {
    float font_size = 0.0f;
    uint32_t primary = 0xFFFFFF;
    uint32_t secondary = 0x999999;
    uint32_t outline = 0x000000;
    int alignment = 2;
};

using AssStyles = std::map<std::string, AssStyle>;

// ASS colours are written as &HAABBGGRR; the renderer wants RGB.
uint32_t assColor(const std::string& value, uint32_t fallback) {
    const size_t start = value.find("&H");
    if (start == std::string::npos) {
        return fallback;
    }
    std::string digits;
    for (size_t i = start + 2; i < value.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            break;
        }
        digits += value[i];
    }
    if (digits.empty()) {
        return fallback;
    }
    const uint32_t bgr = static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 16)) & 0xFFFFFFu;
    return ((bgr & 0xFFu) << 16) | (bgr & 0xFF00u) | ((bgr >> 16) & 0xFFu);
}

// Reads [Script Info] and the style table from an ASS header. Matroska keeps
// this part in the track's CodecPrivate, so it is parsed on its own.
void parseAssHeader(const std::string& data, AssStyles& styles, int& play_res_x, int& play_res_y) {
    std::istringstream stream(normalizeNewlines(data));
    std::string line;
    bool in_styles = false;
    std::vector<std::string> style_format;
    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty() || stripped[0] == ';') {
            continue;
        }
        if (stripped[0] == '[') {
            in_styles = startsWith(stripped, "[V4+ Styles]") || startsWith(stripped, "[V4 Styles]");
            continue;
        }
        if (!in_styles) {
            if (startsWith(stripped, "PlayResX:")) {
                play_res_x = std::atoi(stripped.c_str() + 9);
            } else if (startsWith(stripped, "PlayResY:")) {
                play_res_y = std::atoi(stripped.c_str() + 9);
            }
            continue;
        }
        if (startsWith(stripped, "Format:")) {
            style_format = splitLimited(stripped.substr(7), ',', 32);
            continue;
        }
        if (!startsWith(stripped, "Style:")) {
            continue;
        }
        const std::vector<std::string> fields = splitLimited(stripped.substr(6), ',', 32);
        // The V4+ field order is the fallback when the file has no Format line.
        const auto field = [&](const char* name, int fallback) {
            const int index = style_format.empty() ? fallback : findField(style_format, name);
            return index >= 0 && static_cast<size_t>(index) < fields.size()
                       ? trim(fields[static_cast<size_t>(index)])
                       : std::string();
        };
        const std::string name = field("Name", 0);
        if (name.empty()) {
            continue;
        }
        AssStyle style;
        style.font_size = static_cast<float>(std::atof(field("Fontsize", 2).c_str()));
        style.primary = assColor(field("PrimaryColour", 3), style.primary);
        style.secondary = assColor(field("SecondaryColour", 4), style.secondary);
        style.outline = assColor(field("OutlineColour", 5), style.outline);
        const int alignment = std::atoi(field("Alignment", 18).c_str());
        if (alignment >= 1 && alignment <= 9) {
            style.alignment = alignment;
        }
        styles[name] = style;
    }
}

// Applies the layout overrides of one override block: position, alignment,
// font size and the three colours the renderer uses.
void parseAssOverrides(const std::string& block, Cue& cue) {
    const size_t pos = block.find("\\pos(");
    if (pos != std::string::npos) {
        float x = 0.0f;
        float y = 0.0f;
        if (std::sscanf(block.c_str() + pos + 5, "%f,%f", &x, &y) == 2) {
            cue.has_position = true;
            cue.pos_x = x;
            cue.pos_y = y;
        }
    }
    const size_t an = block.find("\\an");
    if (an != std::string::npos) {
        const int value = std::atoi(block.c_str() + an + 3);
        if (value >= 1 && value <= 9) {
            cue.alignment = value;
        }
    }
    const size_t fs = block.find("\\fs");
    if (fs != std::string::npos) {
        const float value = static_cast<float>(std::atof(block.c_str() + fs + 3));
        if (value > 0.0f) {
            cue.font_size = value;
        }
    }
    const auto color_override = [&block](const char* tag, uint32_t& target) {
        const size_t at = block.find(tag);
        if (at == std::string::npos) {
            return;
        }
        const std::string value = block.substr(at + std::strlen(tag));
        target = assColor(value, target);
    };
    color_override("\\1c", cue.primary_color);
    color_override("\\2c", cue.secondary_color);
    color_override("\\3c", cue.outline_color);
}

// The duration of a karaoke tag in centiseconds, or -1 when the block has none.
// \k, \K, \kf and \ko all carry the timing of the run that follows.
double parseAssKaraokeTag(const std::string& block) {
    for (size_t i = 0; i + 1 < block.size(); ++i) {
        if (block[i] != '\\' || (block[i + 1] != 'k' && block[i + 1] != 'K')) {
            continue;
        }
        size_t length = 2;  // "\k" or "\K", the digits follow the tag
        if (block[i + 1] == 'k' && i + 2 < block.size() &&
            (block[i + 2] == 'f' || block[i + 2] == 'o')) {
            length = 3;  // "\kf" and "\ko"
        }
        return std::atof(block.c_str() + i + length) / 100.0;
    }
    return -1.0;
}

// Turns one ASS event into a cue: the style provides the defaults, the override
// blocks refine them and the karaoke tags split the text into timed runs.
void applyAssLine(const std::string& style_name, const std::string& raw, const AssStyles& styles,
                  Cue& cue) {
    const auto style = styles.find(style_name);
    if (style != styles.end()) {
        cue.font_size = style->second.font_size;
        cue.primary_color = style->second.primary;
        cue.secondary_color = style->second.secondary;
        cue.outline_color = style->second.outline;
        cue.alignment = style->second.alignment;
    }

    std::string text;
    double elapsed = 0.0;
    double pending = -1.0;
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] == '{') {
            const size_t close = raw.find('}', i);
            if (close == std::string::npos) {
                break;
            }
            const std::string block = raw.substr(i + 1, close - i - 1);
            parseAssOverrides(block, cue);
            const double duration = parseAssKaraokeTag(block);
            if (duration >= 0.0) {
                pending = duration;
            }
            i = close + 1;
            continue;
        }
        size_t end = raw.find('{', i);
        if (end == std::string::npos) {
            end = raw.size();
        }
        const std::string run = decodeAssEscapes(raw.substr(i, end - i));
        if (pending >= 0.0) {
            KaraokeSyllable syllable;
            syllable.text = run;
            syllable.start = elapsed;
            syllable.end = elapsed + pending;
            elapsed += pending;
            pending = -1.0;
            cue.karaoke.push_back(std::move(syllable));
        }
        text += run;
        i = end;
    }
    cue.text = trim(text);
}



int findField(const std::vector<std::string>& names, const char* wanted) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (trim(names[i]) == wanted) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct AssFormat {
    int start = -1;
    int end = -1;
    int text = -1;
    int style = -1;
};

AssFormat parseAssFormat(const std::string& line) {
    AssFormat format;
    const std::vector<std::string> names = splitLimited(line, ',', 64);
    format.start = findField(names, "Start");
    format.end = findField(names, "End");
    format.text = findField(names, "Text");
    format.style = findField(names, "Style");
    return format;
}

bool decodeUtf16(const std::string& data, bool little_endian, std::string& out) {
    if (data.size() < 2) {
        return false;
    }
    out.clear();
    for (size_t i = 0; i + 1 < data.size(); i += 2) {
        const unsigned char lo = static_cast<unsigned char>(data[i]);
        const unsigned char hi = static_cast<unsigned char>(data[i + 1]);
        const uint32_t code = little_endian ? (lo | (hi << 8)) : ((lo << 8) | hi);
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return true;
}

std::string readTextFile(const std::filesystem::path& path, bool& ok) {
    ok = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ok = true;
    if (data.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(data[0]);
        const unsigned char b1 = static_cast<unsigned char>(data[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            std::string converted;
            if (decodeUtf16(data.substr(2), true, converted)) {
                data = std::move(converted);
            }
        } else if (b0 == 0xFE && b1 == 0xFF) {
            std::string converted;
            if (decodeUtf16(data.substr(2), false, converted)) {
                data = std::move(converted);
            }
        } else if (b0 == 0xEF && b1 == 0xBB && data.size() >= 3 &&
                   static_cast<unsigned char>(data[2]) == 0xBF) {
            data.erase(0, 3);
        }
    }
    return data;
}

}  // namespace

void finalizeCues(std::vector<Cue>& cues) {
    std::stable_sort(cues.begin(), cues.end(),
                     [](const Cue& a, const Cue& b) { return a.start < b.start; });
    for (size_t i = 0; i < cues.size(); ++i) {
        if (cues[i].end <= cues[i].start) {
            if (i + 1 < cues.size() && cues[i + 1].start > cues[i].start) {
                cues[i].end = cues[i + 1].start;
            } else {
                cues[i].end = cues[i].start + 5.0;
            }
        }
    }
}

const Cue* findCue(const std::vector<Cue>& cues, double time) {
    if (cues.empty()) {
        return nullptr;
    }
    auto it = std::upper_bound(cues.begin(), cues.end(), time,
                               [](double value, const Cue& cue) { return value < cue.start; });
    for (int i = 0; i < 16 && it != cues.begin(); ++i) {
        --it;
        if (it->start <= time && time < it->end) {
            return &*it;
        }
        if (it->end <= time) {
            break;
        }
    }
    return nullptr;
}

std::vector<const Cue*> findCues(const std::vector<Cue>& cues, double time) {
    std::vector<const Cue*> active;
    if (cues.empty()) {
        return active;
    }
    // Cues are sorted by start time, so every candidate lies before the first
    // one that starts later. Karaoke typesetting overlaps many short lines with
    // long ones, so an ended cue does not mean the lines before it ended too:
    // scan a generous window instead of stopping at the first gap.
    const auto it = std::upper_bound(cues.begin(), cues.end(), time,
                                     [](double value, const Cue& cue) { return value < cue.start; });
    constexpr size_t kMaxLookback = 1024;
    size_t looked = 0;
    for (auto back = it; back != cues.begin() && looked < kMaxLookback; ++looked) {
        --back;
        if (back->start <= time && time < back->end) {
            active.push_back(&*back);
        }
    }
    std::reverse(active.begin(), active.end());
    return active;
}

bool parseSrt(const std::string& data, std::vector<Cue>& out, std::string* error) {
    out.clear();
    std::istringstream stream(normalizeNewlines(data));
    std::string line;
    Cue current;
    bool have_timing = false;
    std::string text;

    const auto flush = [&] {
        if (have_timing) {
            current.text = stripTags(decodeEntities(trim(text)));
            out.push_back(current);
        }
        have_timing = false;
        text.clear();
    };

    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty()) {
            flush();
            continue;
        }
        if (stripped.find("-->") != std::string::npos) {
            flush();
            const size_t arrow = stripped.find("-->");
            const std::string left = trim(stripped.substr(0, arrow));
            std::string right = trim(stripped.substr(arrow + 3));
            const size_t space = right.find_first_of(" \t");
            if (space != std::string::npos) {
                right = right.substr(0, space);
            }
            if (!parseSrtTime(left, current.start) || !parseSrtTime(right, current.end)) {
                have_timing = false;
                continue;
            }
            have_timing = true;
            continue;
        }
        if (have_timing) {
            if (!text.empty()) {
                text += '\n';
            }
            text += stripped;
        }
    }
    flush();
    finalizeCues(out);
    if (out.empty()) {
        if (error) {
            *error = "no SRT cues found";
        }
        return false;
    }
    return true;
}

bool parseAss(const std::string& data, std::vector<Cue>& out, std::string* error, Track* track) {
    out.clear();
    AssStyles styles;
    int play_res_x = 0;
    int play_res_y = 0;
    parseAssHeader(data, styles, play_res_x, play_res_y);
    if (track) {
        track->play_res_x = play_res_x;
        track->play_res_y = play_res_y;
    }

    std::istringstream stream(normalizeNewlines(data));
    std::string line;
    enum class Section { None, Info, Styles, Events } section = Section::None;
    AssFormat events_format;
    events_format.start = 1;
    events_format.end = 2;
    events_format.text = 9;

    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty()) {
            continue;
        }
        if (stripped[0] == '[') {
            if (startsWith(stripped, "[Events]")) {
                section = Section::Events;
            } else if (startsWith(stripped, "[V4+ Styles]") || startsWith(stripped, "[V4 Styles]")) {
                section = Section::Styles;
            } else if (startsWith(stripped, "[Script Info]")) {
                section = Section::Info;
            } else {
                section = Section::None;
            }
            continue;
        }
        if (section == Section::Events) {
            if (startsWith(stripped, "Format:")) {
                events_format = parseAssFormat(stripped.substr(7));
            } else if (startsWith(stripped, "Dialogue:")) {
                const size_t field_count =
                    static_cast<size_t>(std::max(events_format.text + 1, 10));
                const std::vector<std::string> fields =
                    splitLimited(stripped.substr(9), ',', field_count);
                const int start_index = events_format.start;
                const int end_index = events_format.end;
                const int text_index = events_format.text;
                if (start_index < 0 || end_index < 0 || text_index < 0 ||
                    static_cast<size_t>(text_index) >= fields.size()) {
                    continue;
                }
                Cue cue;
                if (!parseAssTime(trim(fields[static_cast<size_t>(start_index)]), cue.start) ||
                    !parseAssTime(trim(fields[static_cast<size_t>(end_index)]), cue.end)) {
                    continue;
                }
                const std::string style_name =
                    events_format.style >= 0 &&
                            static_cast<size_t>(events_format.style) < fields.size()
                        ? trim(fields[static_cast<size_t>(events_format.style)])
                        : std::string();
                applyAssLine(style_name, fields[static_cast<size_t>(text_index)], styles, cue);
                if (!cue.text.empty()) {
                    out.push_back(std::move(cue));
                }
            }
        }
    }
    finalizeCues(out);
    if (out.empty()) {
        if (error) {
            *error = "no ASS/SSA events found";
        }
        return false;
    }
    return true;
}

bool parseWebVtt(const std::string& data, std::vector<Cue>& out, std::string* error) {
    out.clear();
    std::istringstream stream(normalizeNewlines(data));
    std::string line;
    Cue current;
    bool have_timing = false;
    std::string text;
    bool in_note = false;

    const auto flush = [&] {
        if (have_timing) {
            current.text = stripTags(decodeEntities(trim(text)));
            out.push_back(current);
        }
        have_timing = false;
        text.clear();
    };

    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty()) {
            flush();
            in_note = false;
            continue;
        }
        if (startsWith(stripped, "WEBVTT")) {
            continue;
        }
        if (startsWith(stripped, "NOTE") || startsWith(stripped, "STYLE") ||
            startsWith(stripped, "REGION")) {
            in_note = true;
            continue;
        }
        if (in_note) {
            continue;
        }
        if (stripped.find("-->") != std::string::npos) {
            flush();
            const size_t arrow = stripped.find("-->");
            const std::string left = trim(stripped.substr(0, arrow));
            std::string right = trim(stripped.substr(arrow + 3));
            const size_t space = right.find_first_of(" \t");
            if (space != std::string::npos) {
                right = right.substr(0, space);
            }
            if (!parseVttTime(left, current.start) || !parseVttTime(right, current.end)) {
                have_timing = false;
                continue;
            }
            have_timing = true;
            continue;
        }
        if (have_timing) {
            if (!text.empty()) {
                text += '\n';
            }
            text += stripped;
        }
    }
    flush();
    finalizeCues(out);
    if (out.empty()) {
        if (error) {
            *error = "no WebVTT cues found";
        }
        return false;
    }
    return true;
}

bool parseMicroDvd(const std::string& data, std::vector<Cue>& out, std::string* error) {
    out.clear();
    std::istringstream stream(normalizeNewlines(data));
    std::string line;
    double fps = 23.976;
    bool have_fps = false;

    while (std::getline(stream, line)) {
        const std::string stripped = trim(line);
        if (stripped.size() < 6 || stripped[0] != '{') {
            continue;
        }
        const size_t first_end = stripped.find('}');
        if (first_end == std::string::npos) {
            continue;
        }
        const size_t second_start = stripped.find('{', first_end);
        const size_t second_end = stripped.find('}', second_start);
        if (second_start == std::string::npos || second_end == std::string::npos) {
            continue;
        }
        const long start_frame = std::atol(stripped.substr(1, first_end - 1).c_str());
        const long end_frame = std::atol(
            stripped.substr(second_start + 1, second_end - second_start - 1).c_str());
        std::string text = stripped.substr(second_end + 1);

        if (!have_fps) {
            const std::string candidate = trim(stripAssText(text));
            char* end = nullptr;
            const double value = std::strtod(candidate.c_str(), &end);
            if (end && *end == '\0' && value > 1.0 && value < 120.0) {
                fps = value;
                have_fps = true;
                continue;
            }
        }

        replaceAll(text, "|", "\n");
        text = stripAssText(text);
        Cue cue;
        cue.start = static_cast<double>(start_frame) / fps;
        cue.end = static_cast<double>(end_frame) / fps;
        cue.text = trim(text);
        if (!cue.text.empty()) {
            out.push_back(std::move(cue));
        }
    }
    finalizeCues(out);
    if (out.empty()) {
        if (error) {
            *error = "no MicroDVD cues found";
        }
        return false;
    }
    return true;
}

bool parseEmbeddedTrack(const std::string& codec, const std::string& codec_private,
                        const std::vector<SubtitleBlock>& blocks, std::vector<Cue>& out,
                        std::string* error, Track* track) {
    out.clear();
    if (codec == "S_TEXT/UTF8") {
        for (const SubtitleBlock& block : blocks) {
            Cue cue;
            cue.start = block.pts_ms / 1000.0;
            cue.end = block.duration_ms > 0 ? (block.pts_ms + block.duration_ms) / 1000.0 : 0.0;
            std::string text(block.data.begin(), block.data.end());
            cue.text = stripTags(decodeEntities(trim(stripTrailingNulls(text))));
            if (!cue.text.empty()) {
                out.push_back(std::move(cue));
            }
        }
    } else if (codec == "S_TEXT/ASS" || codec == "S_TEXT/SSA") {
        // Matroska keeps the styles and the script resolution in CodecPrivate
        // and the events in the blocks.
        AssStyles styles;
        int play_res_x = 0;
        int play_res_y = 0;
        parseAssHeader(codec_private, styles, play_res_x, play_res_y);
        if (track) {
            track->play_res_x = play_res_x;
            track->play_res_y = play_res_y;
        }
        for (const SubtitleBlock& block : blocks) {
            std::string payload = stripTrailingNulls(std::string(block.data.begin(), block.data.end()));
            // Matroska ASS events: ReadOrder,Layer,Style,Name,MarginL,MarginR,
            // MarginV,Effect,Text (9 fields).
            const std::vector<std::string> fields = splitLimited(payload, ',', 9);
            if (fields.size() < 9) {
                continue;
            }
            Cue cue;
            cue.start = block.pts_ms / 1000.0;
            cue.end = block.duration_ms > 0 ? (block.pts_ms + block.duration_ms) / 1000.0 : 0.0;
            applyAssLine(trim(fields[2]), fields[8], styles, cue);
            if (!cue.text.empty()) {
                out.push_back(std::move(cue));
            }
        }
    } else if (codec == "tx3g") {
        for (const SubtitleBlock& block : blocks) {
            if (block.data.size() < 2) {
                continue;
            }
            const size_t length = (static_cast<size_t>(block.data[0]) << 8) | block.data[1];
            const size_t available = std::min(length, block.data.size() - 2);
            Cue cue;
            cue.start = block.pts_ms / 1000.0;
            cue.end = block.duration_ms > 0 ? (block.pts_ms + block.duration_ms) / 1000.0 : 0.0;
            cue.text = trim(std::string(block.data.begin() + 2,
                                        block.data.begin() + 2 + available));
            if (!cue.text.empty()) {
                out.push_back(std::move(cue));
            }
        }
    } else {
        if (error) {
            *error = "unsupported embedded subtitle codec: " + codec;
        }
        return false;
    }
    finalizeCues(out);
    if (out.empty()) {
        if (error) {
            *error = "no subtitle cues found in embedded track";
        }
        return false;
    }
    return true;
}

bool loadSubtitleFile(const std::filesystem::path& path, Track& out, std::string* error) {
    bool ok = false;
    std::string data = readTextFile(path, ok);
    if (!ok) {
        if (error) {
            *error = "failed to read " + path.string();
        }
        return false;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    out = Track{};
    out.name = path.filename().string();
    out.embedded = false;

    bool parsed = false;
    if (extension == ".ass") {
        out.codec = "ass";
        parsed = parseAss(data, out.cues, error, &out);
    } else if (extension == ".ssa") {
        out.codec = "ssa";
        parsed = parseAss(data, out.cues, error, &out);
    } else if (extension == ".vtt") {
        out.codec = "vtt";
        parsed = parseWebVtt(data, out.cues, error);
    } else if (extension == ".sub") {
        out.codec = "sub";
        parsed = parseMicroDvd(data, out.cues, error);
    } else {
        out.codec = "srt";
        parsed = parseSrt(data, out.cues, error);
    }
    if (!parsed) {
        return false;
    }
    out.name = path.filename().string();
    return true;
}

}  // namespace player::subtitle
