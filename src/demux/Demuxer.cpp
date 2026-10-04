#include "demux/Demuxer.hpp"

#include "demux/Ac3Demuxer.hpp"
#include "demux/AviDemuxer.hpp"
#include "demux/FlacDemuxer.hpp"
#include "demux/FlvDemuxer.hpp"
#include "demux/MkvDemuxer.hpp"
#include "demux/Mp3Demuxer.hpp"
#include "demux/Mp4Demuxer.hpp"
#include "demux/MpegPsDemuxer.hpp"
#include "demux/RawHevcDemuxer.hpp"
#include "demux/RawMpegDemuxer.hpp"

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
    Flv,
    RawHevc,
    RawMpeg,
    MpegPs,
    Mp3,
    Flac,
    Ac3,
};

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Tells an MPEG-1 elementary stream from a raw HEVC one by the first start code
// byte. HEVC writes nal_unit_type as type << 1 with type at most 63, so every
// HEVC code byte is even and below 0x80. MPEG-1 uses named codes at 0xB0 and
// above plus odd slice start codes in 0x01..0xAF.
bool isMpeg1StartCode(uint8_t code) {
    return code >= 0xB0 || (code & 1) != 0;
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
    if (count >= 3 && std::memcmp(magic, "FLV", 3) == 0) {
        return Container::Flv;
    }
    if (count >= 4 && ((magic[0] == 0 && magic[1] == 0 && magic[2] == 1) ||
                       (magic[0] == 0 && magic[1] == 0 && magic[2] == 0 && magic[3] == 1))) {
        // The prefix is `000001` or `00000001`; the code byte follows it.
        const size_t code_index = magic[2] == 1 ? 3 : 4;
        if (code_index < count) {
            // A pack header or system header means an MPEG program stream
            // rather than a bare elementary stream.
            if (magic[code_index] == 0xBA || magic[code_index] == 0xBB) {
                return Container::MpegPs;
            }
            if (isMpeg1StartCode(magic[code_index])) {
                return Container::RawMpeg;
            }
        }
        return Container::RawHevc;
    }
    if (count >= 2 && magic[0] == 0x0B && magic[1] == 0x77) {
        return Container::Ac3;
    }
    if (count >= 3 && magic[0] == 'I' && magic[1] == 'D' && magic[2] == '3') {
        return Container::Mp3;
    }
if (count >= 4 && std::memcmp(magic, "fLaC", 4) == 0) {
        return Container::Flac;
    }
    if (count >= 2 && magic[0] == 0xFF && (magic[1] & 0xE0) == 0xE0) {
        return Container::Mp3;
    }

    const std::string ext = toLower(path.extension().string());
    if (ext == ".hevc" || ext == ".h265" || ext == ".265") {
        return Container::RawHevc;
    }
    if (ext == ".vob" || ext == ".evo") {
        return Container::MpegPs;
    }
    if (ext == ".mpg" || ext == ".mpeg" || ext == ".m1v" || ext == ".mpe" || ext == ".vcd" || ext == ".m2v" || ext == ".m2p" || ext == ".mpv") {
        return Container::RawMpeg;
    }
    if (ext == ".mp3") {
        return Container::Mp3;
    }
    if (ext == ".flac") {
        return Container::Flac;
    }
    if (ext == ".flv") {
        return Container::Flv;
    }
    if (ext == ".ac3") {
        return Container::Ac3;
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
        case Container::RawMpeg: {
            auto demuxer = std::make_unique<RawMpegDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::MpegPs: {
            auto demuxer = std::make_unique<MpegPsDemuxer>();
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
        case Container::Flv: {
            auto demuxer = std::make_unique<FlvDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Mp3: {
            auto demuxer = std::make_unique<Mp3Demuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Flac: {
            auto demuxer = std::make_unique<FlacDemuxer>();
            if (!demuxer->open(path, error)) {
                return nullptr;
            }
            return demuxer;
        }
        case Container::Ac3: {
            auto demuxer = std::make_unique<Ac3Demuxer>();
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


