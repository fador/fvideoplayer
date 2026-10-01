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

// Decodes the whole file and writes cropped planar YUV to output.
// Returns 0 on success.
int dumpToYuv(const std::filesystem::path& input, const std::filesystem::path& output,
              DumpStats* stats, std::string* error);

}  // namespace player::decode
