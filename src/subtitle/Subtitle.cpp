#include "subtitle/Subtitle.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <fstream>
#include <functional>
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
    std::string font_name;
    float font_size = 0.0f;
    float border = 0.0f;
    float shadow = 0.0f;
    float margin_l = 0.0f;
    float margin_r = 0.0f;
    float margin_v = 0.0f;
    uint32_t primary = 0xFFFFFF;
    uint32_t secondary = 0x999999;
    uint32_t outline = 0x000000;
    uint32_t back = 0x000000;
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
        style.font_name = field("Fontname", 1);
        style.font_size = static_cast<float>(std::atof(field("Fontsize", 2).c_str()));
        style.primary = assColor(field("PrimaryColour", 3), style.primary);
        style.secondary = assColor(field("SecondaryColour", 4), style.secondary);
        style.outline = assColor(field("OutlineColour", 5), style.outline);
        style.back = assColor(field("BackColour", 6), style.back);
        style.border = static_cast<float>(std::atof(field("Outline", 16).c_str()));
        style.shadow = static_cast<float>(std::atof(field("Shadow", 17).c_str()));
        style.margin_l = static_cast<float>(std::atof(field("MarginL", 19).c_str()));
        style.margin_r = static_cast<float>(std::atof(field("MarginR", 20).c_str()));
        style.margin_v = static_cast<float>(std::atof(field("MarginV", 21).c_str()));
        const int alignment = std::atoi(field("Alignment", 18).c_str());
        if (alignment >= 1 && alignment <= 9) {
            style.alignment = alignment;
        }
        styles[name] = style;
    }
}

// Splits an override block into tags, keeping nested \t(...) arguments with
// their tag. Names include the digits of \1c and friends.
void forEachAssTag(const std::string& block,
                   const std::function<void(const std::string&, const std::string&)>& visit) {
    size_t i = 0;
    while (i < block.size()) {
        if (block[i] != '\\') {
            ++i;
            continue;
        }
        size_t name_end = i + 1;
        // \1c, \3a and friends are the only names that start with a digit.
        if (name_end + 1 < block.size() && block[name_end] >= '1' && block[name_end] <= '4' &&
            std::isalpha(static_cast<unsigned char>(block[name_end + 1]))) {
            name_end += 2;
        } else {
            while (name_end < block.size() &&
                   std::isalpha(static_cast<unsigned char>(block[name_end]))) {
                ++name_end;
            }
        }
        size_t arg_end = name_end;
        int depth = 0;
        while (arg_end < block.size()) {
            const char c = block[arg_end];
            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                if (depth == 0) {
                    break;
                }
                --depth;
            } else if (c == '\\' && depth == 0) {
                break;
            }
            ++arg_end;
        }
        visit(block.substr(i + 1, name_end - i - 1), block.substr(name_end, arg_end - name_end));
        i = arg_end;
    }
}

// The three fields an override changes, or 0 when the name is not one of them.
// Used both for plain tags and for the targets inside \t.
struct AssFieldTarget {
    uint32_t field = 0;
    uint32_t color = 0;
    float number = 0.0f;
    int alpha = 0;
};

// Applies one tag to a state, reporting which field it changed.
uint32_t applyAssStateTag(const std::string& name, const std::string& args, AssState& state) {
    if (name == "b") {
        state.bold = std::atoi(args.c_str()) != 0;  // a weight counts as bold
        return 0;
    }
    if (name == "i") {
        state.italic = std::atoi(args.c_str()) != 0;
        return 0;
    }
    if (name == "u") {
        state.underline = std::atoi(args.c_str()) != 0;
        return 0;
    }
    if (name == "s") {
        state.strikeout = std::atoi(args.c_str()) != 0;
        return 0;
    }
    if (name == "frz") {
        state.angle = static_cast<float>(std::atof(args.c_str()));
        return kAssAngle;
    }
    if (name == "frx" || name == "fry") {
        return 0;  // 3D rotation is not drawn; the line stays flat
    }
    if (name == "fax" || name == "fay") {
        const float value = static_cast<float>(std::atof(args.c_str()));
        if (name == "fax") {
            state.shear_x = value;
            return kAssShearX;
        }
        state.shear_y = value;
        return kAssShearY;
    }
    if (name == "fsp") {
        state.spacing = static_cast<float>(std::atof(args.c_str()));
        return kAssSpacing;
    }
    if (name == "be") {
        state.blur = static_cast<float>(std::atof(args.c_str()));
        return kAssBlur;
    }
    if (name == "fs") {
        const float value = static_cast<float>(std::atof(args.c_str()));
        if (value <= 0.0f) {
            return 0;
        }
        state.font_size = value;
        return kAssFontSize;
    }
    if (name == "fscx" || name == "fscy") {
        const float value = static_cast<float>(std::atof(args.c_str()));
        if (value <= 0.0f) {
            return 0;
        }
        if (name == "fscx") {
            state.scale_x = value;
            return kAssScaleX;
        }
        state.scale_y = value;
        return kAssScaleY;
    }
    if (name == "bord") {
        state.border = static_cast<float>(std::atof(args.c_str()));
        return kAssBorder;
    }
    if (name == "shad") {
        state.shadow = static_cast<float>(std::atof(args.c_str()));
        return kAssShadow;
    }
    if (name == "blur") {
        state.blur = static_cast<float>(std::atof(args.c_str()));
        return kAssBlur;
    }
    if (name == "alpha") {
        // \alpha<aa>: one value for all four elements.
        const size_t start = args.find("&H");
        const int value = static_cast<int>(
            std::strtol(args.c_str() + (start == std::string::npos ? 0 : start + 2), nullptr, 16)) &
            0xFF;
        state.alpha1 = state.alpha2 = state.alpha3 = state.alpha4 = value;
        return kAssAlpha1 | kAssAlpha2 | kAssAlpha3 | kAssAlpha4;
    }
    if (name.size() == 2 && name[1] == 'a' && name[0] >= '1' && name[0] <= '4') {
        const size_t start = args.find("&H");
        const int value = static_cast<int>(
            std::strtol(args.c_str() + (start == std::string::npos ? 0 : start + 2), nullptr, 16)) &
            0xFF;
        switch (name[0]) {
            case '1': state.alpha1 = value; return kAssAlpha1;
            case '2': state.alpha2 = value; return kAssAlpha2;
            case '3': state.alpha3 = value; return kAssAlpha3;
            default: state.alpha4 = value; return kAssAlpha4;
        }
    }
    const auto color_tag = [&](uint32_t& target, uint32_t field) {
        if (args.find("&H") == std::string::npos) {
            return 0u;
        }
        target = assColor(args, target);
        return field;
    };
    if (name == "c" || name == "1c") {
        return color_tag(state.primary, kAssPrimary);
    }
    if (name == "2c") {
        return color_tag(state.secondary, kAssSecondary);
    }
    if (name == "3c") {
        return color_tag(state.outline, kAssOutline);
    }
    if (name == "4c") {
        return color_tag(state.back, kAssBack);
    }
    return 0;
}

// Parses the vector commands of a \p drawing or a vector \clip: m and n start
// a contour, l, p and s add points, b adds cubic beziers and c closes. The
// coordinates are in units of 1 / 2^(scale-1) script pixels.
void parseAssDrawing(const std::string& text, int scale, AssDrawing& out) {
    out.scale = scale < 1 ? 1 : scale;
    std::vector<std::pair<float, float>>* contour = nullptr;
    std::vector<float> numbers;
    const auto finish = [&](char command) {
        const auto point = [&](size_t index) {
            return std::make_pair(numbers[index], numbers[index + 1]);
        };
        switch (command) {
            case 'm':
                // A move starts a contour; extra pairs continue with lines.
                for (size_t i = 0; i + 1 < numbers.size(); i += 2) {
                    if (i == 0) {
                        out.contours.emplace_back();
                        contour = &out.contours.back();
                    }
                    if (contour != nullptr) {
                        contour->push_back(point(i));
                    }
                }
                break;
            case 'l':
            case 'p':
            case 's':
                if (contour == nullptr) {
                    break;
                }
                for (size_t i = 0; i + 1 < numbers.size(); i += 2) {
                    contour->push_back(point(i));
                }
                break;
            case 'b':
                if (contour == nullptr || numbers.size() < 6) {
                    break;
                }
                for (size_t i = 0; i + 5 < numbers.size(); i += 6) {
                    const std::pair<float, float> start = contour->back();
                    const float x1 = numbers[i];
                    const float y1 = numbers[i + 1];
                    const float x2 = numbers[i + 2];
                    const float y2 = numbers[i + 3];
                    const float x3 = numbers[i + 4];
                    const float y3 = numbers[i + 5];
                    constexpr int kSteps = 8;  // flattening resolution
                    for (int step = 1; step <= kSteps; ++step) {
                        const float t = static_cast<float>(step) / kSteps;
                        const float u = 1.0f - t;
                        const float x = u * u * u * start.first + 3 * u * u * t * x1 +
                                        3 * u * t * t * x2 + t * t * t * x3;
                        const float y = u * u * u * start.second + 3 * u * u * t * y1 +
                                        3 * u * t * t * y2 + t * t * t * y3;
                        contour->push_back({x, y});
                    }
                }
                break;
            case 'c':
                break;  // contours are closed when they are filled
            default:
                break;
        }
        numbers.clear();
    };

    char command = 0;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isalpha(static_cast<unsigned char>(c))) {
            finish(command);
            command = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            char* end = nullptr;
            const float value = std::strtof(text.c_str() + i, &end);
            if (end == text.c_str() + i) {
                ++i;
                continue;
            }
            numbers.push_back(value);
            i = static_cast<size_t>(end - text.c_str());
            continue;
        }
        ++i;  // separators
    }
    finish(command);
}

// \clip(x1,y1,x2,y2) or \clip(<commands>); \iclip inverts it.
void applyAssClip(const std::string& name, const std::string& args, Cue& cue) {
    size_t open = args.find('(');
    size_t close = args.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return;
    }
    const std::string body = args.substr(open + 1, close - open - 1);
    AssClip clip;
    clip.inverse = name == "iclip";
    // The rectangle form is four numbers with no command letters.
    if (body.find_first_of("mnlbspc") == std::string::npos) {
        float values[4] = {};
        int count = 0;
        size_t pos = 0;
        while (count < 4) {
            const size_t comma = body.find_first_of(", ", pos);
            const std::string piece = trim(body.substr(pos, comma - pos));
            if (!piece.empty()) {
                values[count++] = static_cast<float>(std::atof(piece.c_str()));
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
        if (count == 4) {
            clip.x1 = std::min(values[0], values[2]);
            clip.y1 = std::min(values[1], values[3]);
            clip.x2 = std::max(values[0], values[2]);
            clip.y2 = std::max(values[1], values[3]);
            cue.clips.push_back(std::move(clip));
            return;
        }
    }
    // A vector clip may carry a leading scale number.
    int scale = 1;
    std::string commands = body;
    if (!commands.empty() && std::isdigit(static_cast<unsigned char>(commands[0]))) {
        scale = std::atoi(commands.c_str());
        const size_t comma = commands.find(',');
        commands = comma == std::string::npos ? std::string() : commands.substr(comma + 1);
    }
    AssDrawing drawing;
    parseAssDrawing(commands, scale < 1 ? 1 : scale, drawing);
    if (drawing.contours.empty()) {
        return;
    }
    clip.rect = false;
    clip.points = drawing.contours.front();
    cue.clips.push_back(std::move(clip));
}

// \move(x1,y1,x2,y2[,t1,t2]): times are milliseconds from the cue start.
void applyAssMove(const std::string& args, Cue& cue) {    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    int t1 = 0;
    int t2 = 0;
    const int fields = std::sscanf(args.c_str(), "(%f,%f,%f,%f,%d,%d)", &x1, &y1, &x2, &y2, &t1, &t2);
    if (fields < 4) {
        return;
    }
    cue.has_move = true;
    cue.has_position = true;
    cue.move_x1 = x1;
    cue.move_y1 = y1;
    cue.move_x2 = x2;
    cue.move_y2 = y2;
    cue.move_t1 = t1 / 1000.0;
    cue.move_t2 = t2 > 0 ? t2 / 1000.0 : (cue.end - cue.start);
}

// \fad(a,b) fades in over a milliseconds and out over b; \fade takes four
// alphas and four times.
void applyAssFade(const std::string& name, const std::string& args, Cue& cue) {
    double values[7] = {};
    int count = 0;
    size_t start = args.find('(');
    if (start == std::string::npos) {
        return;
    }
    std::string numbers = args.substr(start + 1);
    size_t pos = 0;
    while (count < 7) {
        const size_t comma = numbers.find_first_of(",)", pos);
        if (comma == std::string::npos) {
            break;
        }
        values[count++] = std::atof(numbers.substr(pos, comma - pos).c_str());
        if (numbers[comma] == ')') {
            break;
        }
        pos = comma + 1;
    }
    if (count < 2) {
        return;
    }
    cue.fade = AssFade{};
    cue.fade.present = true;
    const double duration = cue.end - cue.start;
    if (name == "fad" || count == 2) {
        // \fad(a,b): transparent at the start, opaque after a, opaque until
        // the end minus b, transparent again at the end.
        cue.fade.a1 = 255;
        cue.fade.a2 = 0;
        cue.fade.a3 = 0;
        cue.fade.a4 = 255;
        cue.fade.t1 = 0.0;
        cue.fade.t2 = values[0] / 1000.0;
        cue.fade.t3 = duration - values[1] / 1000.0;
        cue.fade.t4 = duration;
        return;
    }
    // \fade(a1,a2,a3,t1,t2,t3,t4) ends opaque.
    cue.fade.a1 = static_cast<int>(values[0]);
    cue.fade.a2 = static_cast<int>(values[1]);
    cue.fade.a3 = static_cast<int>(values[2]);
    cue.fade.a4 = 0;
    cue.fade.t1 = values[3] / 1000.0;
    cue.fade.t2 = values[4] / 1000.0;
    cue.fade.t3 = values[5] / 1000.0;
    cue.fade.t4 = values[6] / 1000.0;
}

// One override block: plain tags change the state, \t records a transition and
// \fad, \fade and \move set their own fields.
void parseAssOverrides(const std::string& block, Cue& cue, const AssState& reset_state) {
    forEachAssTag(block, [&](const std::string& name, const std::string& args) {
        if (name == "pos") {
            float x = 0.0f;
            float y = 0.0f;
            if (std::sscanf(args.c_str(), "(%f,%f)", &x, &y) == 2) {
                cue.has_position = true;
                cue.pos_x = x;
                cue.pos_y = y;
            }
            return;
        }
        if (name == "an") {
            const int value = std::atoi(args.c_str());
            if (value >= 1 && value <= 9) {
                cue.alignment = value;
            }
            return;
        }
        if (name == "move") {
            applyAssMove(args, cue);
            return;
        }
        if (name == "org") {
            float x = 0.0f;
            float y = 0.0f;
            if (std::sscanf(args.c_str(), "(%f,%f)", &x, &y) == 2) {
                cue.has_origin = true;
                cue.origin_x = x;
                cue.origin_y = y;
            }
            return;
        }
        if (name == "clip" || name == "iclip") {
            applyAssClip(name, args, cue);
            return;
        }
        if (name == "q") {
            cue.wrap_style = std::atoi(args.c_str());
            return;
        }
        if (name == "fad" || name == "fade") {
            applyAssFade(name, args, cue);
            return;
        }
        if (name == "r") {
            cue.state = reset_state;
            return;
        }
        if (name == "t") {
            // \t([t1,][t2,]tags): the leading numbers are milliseconds.
            size_t open = args.find('(');
            if (open == std::string::npos) {
                return;
            }
            const std::string body = args.substr(open + 1);
            std::vector<double> times;
            size_t pos = 0;
            while (times.size() < 2) {
                const size_t comma = body.find(',', pos);
                if (comma == std::string::npos) {
                    break;
                }
                const std::string piece = trim(body.substr(pos, comma - pos));
                if (!piece.empty() && std::isdigit(static_cast<unsigned char>(piece[0]))) {
                    times.push_back(std::atof(piece.c_str()));
                    pos = comma + 1;
                    continue;
                }
                break;
            }
            if (pos >= body.size()) {
                return;
            }
            AssTransition transition;
            transition.t1 = times.size() == 2 ? times[0] / 1000.0 : 0.0;
            transition.t2 = times.size() >= 1 ? times.back() / 1000.0
                                              : (cue.end - cue.start);
            transition.target = cue.state;
            forEachAssTag(body.substr(pos), [&](const std::string& nested,
                                                const std::string& nested_args) {
                transition.mask |= applyAssStateTag(nested, nested_args, transition.target);
            });
            if (transition.mask != 0) {
                cue.transitions.push_back(std::move(transition));
            }
            return;
        }
        if (name == "k" || name == "K" || name == "kf" || name == "ko") {
            return;  // handled while splitting the text into syllables
        }
        applyAssStateTag(name, args, cue.state);
    });
}

// The duration of a karaoke tag in centiseconds, or -1 when the block has none.
// \k, \K, \kf and \ko all carry the timing of the run that follows.
double parseAssKaraokeTag(const std::string& block) {
    double duration = -1.0;
    forEachAssTag(block, [&](const std::string& name, const std::string& args) {
        if (duration >= 0.0) {
            return;
        }
        if (name == "k" || name == "K" || name == "kf" || name == "ko") {
            duration = std::atof(args.c_str()) / 100.0;
        }
    });
    return duration;
}

// Turns one ASS event into a cue: the style provides the defaults, the override
// blocks refine them and the karaoke tags split the text into timed runs.
void applyAssLine(const std::string& style_name, const std::string& raw, const AssStyles& styles,
                  Cue& cue) {
    AssState style_state;
    const auto style = styles.find(style_name);
    if (style != styles.end()) {
        cue.font_name = style->second.font_name;
        cue.margin_l = style->second.margin_l;
        cue.margin_r = style->second.margin_r;
        cue.margin_v = style->second.margin_v;
        cue.alignment = style->second.alignment;
        style_state.font_size = style->second.font_size;
        style_state.border = style->second.border;
        style_state.shadow = style->second.shadow;
        style_state.primary = style->second.primary;
        style_state.secondary = style->second.secondary;
        style_state.outline = style->second.outline;
        style_state.back = style->second.back;
    }
    cue.state = style_state;

    std::string text;
    std::string drawing_text;
    int drawing_scale = 0;  // \p: 0 while text is being read
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
            parseAssOverrides(block, cue, style_state);
            // \p switches between text and drawing commands.
            int requested = -1;
            forEachAssTag(block, [&](const std::string& name, const std::string& args) {
                if (name == "p") {
                    requested = args.empty() ? 1 : std::atoi(args.c_str());
                }
            });
            if (requested == 0 && drawing_scale > 0) {
                AssDrawing drawing;
                parseAssDrawing(drawing_text, drawing_scale, drawing);
                if (!drawing.contours.empty()) {
                    cue.drawings.push_back(std::move(drawing));
                }
                drawing_text.clear();
                drawing_scale = 0;
            } else if (requested > 0) {
                drawing_scale = requested;
            }
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
        if (drawing_scale > 0) {
            drawing_text += run;
            i = end;
            continue;
        }
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
    if (drawing_scale > 0) {
        AssDrawing drawing;
        parseAssDrawing(drawing_text, drawing_scale, drawing);
        if (!drawing.contours.empty()) {
            cue.drawings.push_back(std::move(drawing));
        }
    }
    cue.text = trim(text);
}

// Linear blend of two colours, channel by channel.
uint32_t lerpColor(uint32_t from, uint32_t to, float factor) {
    const auto channel = [&](int shift) {
        const int a = static_cast<int>((from >> shift) & 0xFF);
        const int b = static_cast<int>((to >> shift) & 0xFF);
        return static_cast<uint32_t>(a + (b - a) * factor) & 0xFF;
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// Applies the masked fields of one transition at the given factor.
void blendAssState(AssState& state, const AssTransition& transition, float factor) {
    const AssState& target = transition.target;
    const auto number = [&](uint32_t field, float current, float wanted) {
        return (transition.mask & field) != 0 ? current + (wanted - current) * factor : current;
    };
    state.font_size = number(kAssFontSize, state.font_size, target.font_size);
    state.scale_x = number(kAssScaleX, state.scale_x, target.scale_x);
    state.scale_y = number(kAssScaleY, state.scale_y, target.scale_y);
    state.border = number(kAssBorder, state.border, target.border);
    state.shadow = number(kAssShadow, state.shadow, target.shadow);
    state.blur = number(kAssBlur, state.blur, target.blur);
    state.angle = number(kAssAngle, state.angle, target.angle);
    state.shear_x = number(kAssShearX, state.shear_x, target.shear_x);
    state.shear_y = number(kAssShearY, state.shear_y, target.shear_y);
    state.spacing = number(kAssSpacing, state.spacing, target.spacing);
    const auto alpha = [&](uint32_t field, int current, int wanted) {
        return (transition.mask & field) != 0
                   ? static_cast<int>(current + (wanted - current) * factor)
                   : current;
    };
    state.alpha1 = alpha(kAssAlpha1, state.alpha1, target.alpha1);
    state.alpha2 = alpha(kAssAlpha2, state.alpha2, target.alpha2);
    state.alpha3 = alpha(kAssAlpha3, state.alpha3, target.alpha3);
    state.alpha4 = alpha(kAssAlpha4, state.alpha4, target.alpha4);
    if ((transition.mask & kAssPrimary) != 0) {
        state.primary = lerpColor(state.primary, target.primary, factor);
    }
    if ((transition.mask & kAssSecondary) != 0) {
        state.secondary = lerpColor(state.secondary, target.secondary, factor);
    }
    if ((transition.mask & kAssOutline) != 0) {
        state.outline = lerpColor(state.outline, target.outline, factor);
    }
    if ((transition.mask & kAssBack) != 0) {
        state.back = lerpColor(state.back, target.back, factor);
    }
}

// Opacity from \fad and \fade: a1 at t1, a2 at t2, a3 at t3 and opaque at t4,
// with the time before t1 and after t4 held at the end values.
float assFadeOpacity(const AssFade& fade, double time) {
    if (!fade.present) {
        return 1.0f;
    }
    const auto between = [&](double from_time, double to_time, int from_alpha, int to_alpha) {
        if (to_time <= from_time) {
            return static_cast<float>(from_alpha);
        }
        const double factor = std::clamp((time - from_time) / (to_time - from_time), 0.0, 1.0);
        return static_cast<float>(from_alpha + (to_alpha - from_alpha) * factor);
    };
    int alpha = 0;
    if (time < fade.t2) {
        alpha = static_cast<int>(between(fade.t1, fade.t2, fade.a1, fade.a2));
    } else if (time < fade.t3) {
        alpha = static_cast<int>(between(fade.t2, fade.t3, fade.a2, fade.a3));
    } else {
        alpha = static_cast<int>(between(fade.t3, fade.t4, fade.a3, fade.a4));
    }
    return 1.0f - std::clamp(alpha, 0, 255) / 255.0f;
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
    int margin_l = -1;
    int margin_r = -1;
    int margin_v = -1;
};

AssFormat parseAssFormat(const std::string& line) {
    AssFormat format;
    const std::vector<std::string> names = splitLimited(line, ',', 64);
    format.start = findField(names, "Start");
    format.end = findField(names, "End");
    format.text = findField(names, "Text");
    format.style = findField(names, "Style");
    format.margin_l = findField(names, "MarginL");
    format.margin_r = findField(names, "MarginR");
    format.margin_v = findField(names, "MarginV");
    return format;
}

// Per event margins override the style's, but only when they are set: zero
// means "use the style".
float assFieldValue(const std::vector<std::string>& fields, int index) {
    return index >= 0 && static_cast<size_t>(index) < fields.size()
               ? static_cast<float>(std::atof(trim(fields[static_cast<size_t>(index)]).c_str()))
               : 0.0f;
}

void applyAssEventMargins(const std::vector<std::string>& fields, int margin_l, int margin_r,
                          int margin_v, Cue& cue) {
    const float left = assFieldValue(fields, margin_l);
    const float right = assFieldValue(fields, margin_r);
    const float vertical = assFieldValue(fields, margin_v);
    if (left > 0.0f) {
        cue.margin_l = left;
    }
    if (right > 0.0f) {
        cue.margin_r = right;
    }
    if (vertical > 0.0f) {
        cue.margin_v = vertical;
    }
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

AssRenderState evaluateAssCue(const Cue& cue, double time) {
    AssState state = cue.state;
    for (const AssTransition& transition : cue.transitions) {
        const float factor = transition.t2 <= transition.t1
                                 ? 1.0f
                                 : static_cast<float>(std::clamp(
                                       (time - cue.start - transition.t1) /
                                           (transition.t2 - transition.t1),
                                       0.0, 1.0));
        blendAssState(state, transition, factor);
    }

    AssRenderState out;
    out.font_size = state.font_size;
    out.scale_x = state.scale_x;
    out.scale_y = state.scale_y;
    out.border = state.border;
    out.shadow = state.shadow;
    out.blur = state.blur;
    out.angle = state.angle;
    out.shear_x = state.shear_x;
    out.shear_y = state.shear_y;
    out.spacing = state.spacing;
    out.bold = state.bold;
    out.italic = state.italic;
    out.underline = state.underline;
    out.strikeout = state.strikeout;
    out.primary = state.primary;
    out.secondary = state.secondary;
    out.outline = state.outline;
    out.back = state.back;
    out.alignment = cue.alignment;

    const float fade = assFadeOpacity(cue.fade, time - cue.start);
    out.text_opacity = fade * (1.0f - state.alpha1 / 255.0f);
    out.outline_opacity = fade * (1.0f - state.alpha3 / 255.0f);
    out.shadow_opacity = fade * (1.0f - state.alpha4 / 255.0f);

    out.has_position = cue.has_position;
    out.pos_x = cue.pos_x;
    out.pos_y = cue.pos_y;
    if (cue.has_move) {
        const double span = cue.move_t2 - cue.move_t1;
        const float factor = span <= 0.0
                                 ? 1.0f
                                 : static_cast<float>(std::clamp(
                                       (time - cue.start - cue.move_t1) / span, 0.0, 1.0));
        out.pos_x = cue.move_x1 + (cue.move_x2 - cue.move_x1) * factor;
        out.pos_y = cue.move_y1 + (cue.move_y2 - cue.move_y1) * factor;
    }
    return out;
}

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
                applyAssEventMargins(fields, events_format.margin_l, events_format.margin_r,
                                     events_format.margin_v, cue);
                if (!cue.text.empty() || !cue.drawings.empty()) {
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
            applyAssEventMargins(fields, 4, 5, 6, cue);
            if (!cue.text.empty() || !cue.drawings.empty()) {
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