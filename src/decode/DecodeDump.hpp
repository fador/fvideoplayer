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
};

// Decodes the file and writes cropped planar YUV to output. The first
// skip_frames decoded frames are discarded, then max_frames are written
// (max_frames < 0 writes the rest of the file). Returns 0 on success.
int dumpToYuv(const std::filesystem::path& input, const std::filesystem::path& output,
              int max_frames, int skip_frames, DumpStats* stats, std::string* error);

}  // namespace player::decode
