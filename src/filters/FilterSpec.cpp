#include "filters/FilterSpec.hpp"

#include "filters/AudioFilters.hpp"
#include "filters/VideoFilters.hpp"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <vector>

namespace player::filters {
namespace {

struct FilterToken {
    std::string name;
    std::vector<std::string> positional;
    std::map<std::string, std::string, std::less<>> values;
};

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool parseToken(const std::string& line, FilterToken& token, std::string& error) {
    const std::string text = trim(line);
    if (text.empty()) {
        error = "empty filter description";
        return false;
    }
    std::string arguments;
    const size_t equals = text.find('=');
    if (equals == std::string::npos) {
        token.name = text;
    } else {
        token.name = trim(text.substr(0, equals));
        arguments = text.substr(equals + 1);
    }
    if (token.name.empty()) {
        error = "filter name is empty";
        return false;
    }
    size_t start = 0;
    while (start <= arguments.size() && !arguments.empty()) {
        const size_t colon = arguments.find(':', start);
        const std::string item =
            trim(arguments.substr(start, colon == std::string::npos ? std::string::npos
                                                                    : colon - start));
        if (!item.empty()) {
            const size_t item_equals = item.find('=');
            if (item_equals == std::string::npos) {
                token.positional.push_back(item);
            } else {
                const std::string key = trim(item.substr(0, item_equals));
                const std::string value = trim(item.substr(item_equals + 1));
                if (key.empty()) {
                    error = "argument name is empty in '" + line + "'";
                    return false;
                }
                token.values[key] = value;
            }
        }
        if (colon == std::string::npos) {
            break;
        }
        start = colon + 1;
    }
    return true;
}

const std::string* findValue(const FilterToken& token, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = token.values.find(key);
        if (it != token.values.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

std::string valueAt(const FilterToken& token, std::initializer_list<const char*> keys,
                    size_t index) {
    if (const std::string* named = findValue(token, keys)) {
        return *named;
    }
    return index < token.positional.size() ? token.positional[index] : std::string();
}

bool parseInt(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    try {
        size_t used = 0;
        const long value = std::stol(text, &used);
        if (used != text.size() || value < INT_MIN || value > INT_MAX) {
            return false;
        }
        out = static_cast<int>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    try {
        size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(value)) {
            return false;
        }
        out = value;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::unique_ptr<IVideoFilter> buildVideoFilter(const FilterToken& token, std::string& error) {
    const std::string name = lower(token.name);
    if (name == "scale") {
        const std::string width_text = valueAt(token, {"w", "width"}, 0);
        const std::string height_text = valueAt(token, {"h", "height"}, 1);
        int width = 0;
        int height = 0;
        if (width_text.empty() || height_text.empty()) {
            error = "scale expects scale=WIDTH:HEIGHT";
            return nullptr;
        }
        if (!parseInt(width_text, width) || !parseInt(height_text, height)) {
            error = "scale: invalid width or height";
            return nullptr;
        }
        return std::make_unique<ScaleFilter>(width, height);
    }
    if (name == "crop") {
        const std::string width_text = valueAt(token, {"w", "width"}, 0);
        const std::string height_text = valueAt(token, {"h", "height"}, 1);
        const std::string x_text = valueAt(token, {"x"}, 2);
        const std::string y_text = valueAt(token, {"y"}, 3);
        int width = 0;
        int height = 0;
        int x = -1;
        int y = -1;
        if (width_text.empty() || height_text.empty()) {
            error = "crop expects crop=WIDTH:HEIGHT[:X:Y]";
            return nullptr;
        }
        if (!parseInt(width_text, width) || !parseInt(height_text, height)) {
            error = "crop: invalid width or height";
            return nullptr;
        }
        if ((!x_text.empty() && !parseInt(x_text, x)) ||
            (!y_text.empty() && !parseInt(y_text, y))) {
            error = "crop: invalid x or y";
            return nullptr;
        }
        return std::make_unique<CropFilter>(width, height, x, y);
    }
    if (name == "flip" || name == "hflip" || name == "vflip") {
        std::string flags = valueAt(token, {"flags", "f"}, 0);
        bool horizontal = name != "vflip";
        bool vertical = name == "vflip";
        if (name == "flip") {
            if (flags.empty()) {
                error = "flip expects flip=h, flip=v or flip=hv";
                return nullptr;
            }
            horizontal = false;
            vertical = false;
            for (const char c : lower(flags)) {
                if (c == 'h') {
                    horizontal = true;
                } else if (c == 'v') {
                    vertical = true;
                }
            }
            if (!horizontal && !vertical) {
                error = "flip expects flip=h, flip=v or flip=hv";
                return nullptr;
            }
        }
        return std::make_unique<FlipFilter>(horizontal, vertical);
    }
    if (name == "grayscale" || name == "gray") {
        return std::make_unique<GrayscaleFilter>();
    }
    if (name == "eq") {
        double brightness = 0.0;
        double contrast = 1.0;
        double saturation = 1.0;
        const std::string brightness_text = valueAt(token, {"brightness", "b"}, 0);
        const std::string contrast_text = valueAt(token, {"contrast", "c"}, 1);
        const std::string saturation_text = valueAt(token, {"saturation", "s"}, 2);
        if ((!brightness_text.empty() && !parseDouble(brightness_text, brightness)) ||
            (!contrast_text.empty() && !parseDouble(contrast_text, contrast)) ||
            (!saturation_text.empty() && !parseDouble(saturation_text, saturation))) {
            error = "eq: invalid brightness, contrast or saturation";
            return nullptr;
        }
        return std::make_unique<EqFilter>(static_cast<float>(brightness), static_cast<float>(contrast),
                                          static_cast<float>(saturation));
    }
    error = "unknown video filter '" + token.name + "'";
    return nullptr;
}

std::unique_ptr<IAudioFilter> buildAudioFilter(const FilterToken& token, std::string& error) {
    const std::string name = lower(token.name);
    if (name == "volume" || name == "vol") {
        const std::string text = valueAt(token, {"volume", "gain", "g"}, 0);
        double gain = 0.0;
        if (text.empty()) {
            error = "volume expects volume=GAIN or volume=NDB";
            return nullptr;
        }
        if (text.size() > 2 && lower(text.substr(text.size() - 2)) == "db") {
            double decibels = 0.0;
            if (!parseDouble(text.substr(0, text.size() - 2), decibels)) {
                error = "volume: invalid decibel value";
                return nullptr;
            }
            gain = std::pow(10.0, decibels / 20.0);
        } else if (!parseDouble(text, gain)) {
            error = "volume: invalid gain";
            return nullptr;
        }
        return std::make_unique<VolumeFilter>(static_cast<float>(gain));
    }
    if (name == "resample" || name == "aresample") {
        const std::string text = valueAt(token, {"rate", "sample_rate", "r"}, 0);
        int rate = 0;
        if (text.empty() || !parseInt(text, rate) || rate <= 0) {
            error = "resample expects resample=RATE";
            return nullptr;
        }
        return std::make_unique<ResampleFilter>(static_cast<uint32_t>(rate));
    }
    if (name == "channels" || name == "pan") {
        const std::string text = valueAt(token, {"channels", "c"}, 0);
        int channels = 0;
        if (text.empty() || !parseInt(text, channels) || (channels != 1 && channels != 2)) {
            error = "channels expects channels=1 or channels=2";
            return nullptr;
        }
        return std::make_unique<ChannelsFilter>(static_cast<uint32_t>(channels));
    }
    if (name == "equalizer" || name == "eq") {
        double bass = 0.0;
        double mid = 0.0;
        double treble = 0.0;
        const std::string bass_text = valueAt(token, {"bass", "b"}, 0);
        const std::string mid_text = valueAt(token, {"mid", "m"}, 1);
        const std::string treble_text = valueAt(token, {"treble", "high", "t"}, 2);
        if ((!bass_text.empty() && !parseDouble(bass_text, bass)) ||
            (!mid_text.empty() && !parseDouble(mid_text, mid)) ||
            (!treble_text.empty() && !parseDouble(treble_text, treble))) {
            error = "equalizer: invalid bass, mid or treble gain";
            return nullptr;
        }
        return std::make_unique<EqualizerFilter>(bass, mid, treble);
    }
    error = "unknown audio filter '" + token.name + "'";
    return nullptr;
}

template <typename Chain, typename Build>
std::shared_ptr<Chain> parseSpec(const std::string& spec, Build build, std::string& error) {
    auto chain = std::make_shared<Chain>();
    const std::string text = trim(spec);
    if (text.empty()) {
        return chain;
    }
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const std::string description =
            trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!description.empty()) {
            FilterToken token;
            if (!parseToken(description, token, error)) {
                return nullptr;
            }
            auto filter = build(token, error);
            if (!filter) {
                return nullptr;
            }
            chain->add(std::move(filter));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return chain;
}

}  // namespace

std::unique_ptr<IVideoFilter> createVideoFilter(const std::string& description, std::string& error) {
    FilterToken token;
    if (!parseToken(description, token, error)) {
        return nullptr;
    }
    return buildVideoFilter(token, error);
}

std::unique_ptr<IAudioFilter> createAudioFilter(const std::string& description, std::string& error) {
    FilterToken token;
    if (!parseToken(description, token, error)) {
        return nullptr;
    }
    return buildAudioFilter(token, error);
}

std::shared_ptr<VideoFilterChain> parseVideoFilterSpec(const std::string& spec, std::string& error) {
    return parseSpec<VideoFilterChain>(
        spec, [](const FilterToken& token, std::string& message) { return buildVideoFilter(token, message); },
        error);
}

std::shared_ptr<AudioFilterChain> parseAudioFilterSpec(const std::string& spec, std::string& error) {
    return parseSpec<AudioFilterChain>(
        spec, [](const FilterToken& token, std::string& message) { return buildAudioFilter(token, message); },
        error);
}

}  // namespace player::filters
