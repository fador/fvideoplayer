#include "utils/Log.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace player::log {
namespace {

constexpr size_t kMaxEntries = 2000;

std::mutex g_mutex;
std::deque<Entry> g_entries;
uint64_t g_version = 0;

double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

const char* levelName(Level level) {
    switch (level) {
        case Level::Debug:
            return "D";
        case Level::Info:
            return "I";
        case Level::Warning:
            return "W";
        case Level::Error:
            return "E";
    }
    return "?";
}

void add(Level level, const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Entry entry;
        entry.time_seconds = nowSeconds();
        entry.level = level;
        entry.text = text;
        g_entries.push_back(std::move(entry));
        while (g_entries.size() > kMaxEntries) {
            g_entries.pop_front();
        }
        ++g_version;
    }
    // Also mirror to stderr; harmless when no console is attached.
    std::fprintf(stderr, "[%s] %s\n", levelName(level), text.c_str());
}

void addf(Level level, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    add(level, buffer);
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_entries.clear();
    ++g_version;
}

std::vector<Entry> snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::vector<Entry>(g_entries.begin(), g_entries.end());
}

uint64_t version() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_version;
}

}  // namespace player::log
