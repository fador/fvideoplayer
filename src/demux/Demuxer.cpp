#include "demux/Demuxer.hpp"

#include "demux/AviDemuxer.hpp"
#include "demux/MkvDemuxer.hpp"
#include "demux/Mp4Demuxer.hpp"
#include "demux/RawHevcDemuxer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace player::demux {
namespace {

enum class Container {
    Unknown,
    Mp4,
    Mkv,
    Avi,
    RawHevc,
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

Container detectContainer(const std::filesystem::path& path) {
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) {
        return Container::Unknown;
    }
    uint8_t magic[16] = {};
    const size_t count = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);

    if (count >= 8 && std::memcmp(magic + 4, "ftyp", 4) == 0) {
        return Container::Mp4;
    }
    if (count >= 4 && magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3) {
        return Container::Mkv;
    }
    if (count >= 12 && std::memcmp(magic, "RIFF", 4) == 0 && std::memcmp(magic + 8, "AVI ", 4) == 0) {
        return Container::Avi;
    }
    if (count >= 4 && ((magic[0] == 0 && magic[1] == 0 && magic[2] == 1) ||
                       (magic[0] == 0 && magic[1] == 0 && magic[2] == 0 && magic[3] == 1))) {
        return Container::RawHevc;
    }

    const std::string ext = toLower(path.extension().string());
    if (ext == ".hevc" || ext == ".h265" || ext == ".265") {
        return Container::RawHevc;
    }
    return Container::Unknown;
}

}  // namespace

std::unique_ptr<IDemuxer> createDemuxer(const std::filesystem::path& path, std::string& error) {
    switch (detectContainer(path)) {
        case Container::RawHevc: {
            auto demuxer = std::make_unique<RawHevcDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Mp4: {
            auto demuxer = std::make_unique<Mp4Demuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Mkv: {
            auto demuxer = std::make_unique<MkvDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Avi: {
            auto demuxer = std::make_unique<AviDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Unknown:
            break;
    }
    error = "unsupported file format: " + path.string();
    return nullptr;
}

}  // namespace player::demux
