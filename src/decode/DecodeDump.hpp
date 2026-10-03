#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace player::decode {

struct DumpStats {
    int64_t frames = 0;
    int64_t bytes = 0;
    double seconds = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    // Pictures whose slices did not parse cleanly. A damaged picture still
    // produces output, so this is the only way to tell that it happened.
    int64_t damaged = 0;
    // Where the most recent damaged slice gave up, for chasing a bad stream.
    int damage_mb_x = -1;
    int damage_mb_y = -1;
    size_t damage_bit = 0;
    const char* damage_reason = "";
};

// Decodes the file and writes cropped planar YUV to output. The first
// skip_frames decoded frames are discarded, then max_frames are written
// (max_frames < 0 writes the rest of the file). Returns 0 on success.
int dumpToYuv(const std::filesystem::path& input, const std::filesystem::path& output,
              int max_frames, int skip_frames, DumpStats* stats, std::string* error);

}  // namespace player::decode
