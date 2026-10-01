#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace player::log {

enum class Level : uint8_t {
    Debug = 0,
    Info,
    Warning,
    Error,
};

struct Entry {
    double time_seconds = 0.0;  // since process start
    Level level = Level::Info;
    std::string text;
};

// Thread-safe ring buffer of log messages, shown in the in-app console.
void add(Level level, const std::string& text);
void addf(Level level, const char* format, ...);
void clear();

std::vector<Entry> snapshot();
uint64_t version();

const char* levelName(Level level);

}  // namespace player::log
