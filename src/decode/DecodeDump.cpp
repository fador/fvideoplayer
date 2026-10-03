#include "decode/DecodeDump.hpp"

#include "decode/KvzdecDecoder.hpp"
#include "decode/mpeg12/Mpeg12Decoder.hpp"
#include "demux/Demuxer.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace player::decode {
namespace {

bool writePicture(FILE* out, const Frame& frame) {
    const int plane_count = chromaPlaneCount(frame.format());
    for (int plane = 0; plane < plane_count; ++plane) {
        const int shift_w = (plane > 0 && frame.format() != ChromaFormat::C444) ? 1 : 0;
        const int shift_h = (plane > 0 && frame.format() == ChromaFormat::C420) ? 1 : 0;
        const int width = (frame.displayWidth()) >> shift_w;
        const int height = (frame.displayHeight()) >> shift_h;
        const int stride = frame.planeStride(plane);
        const uint8_t* source =
            frame.plane(plane) +
            (static_cast<size_t>(frame.crop_top_ >> shift_h) * stride +
             (frame.crop_left_ >> shift_w)) *
                frame.pixelSize();
        for (int y = 0; y < height; ++y) {
            const size_t bytes = static_cast<size_t>(width) * frame.pixelSize();
            if (std::fwrite(source + static_cast<size_t>(y) * stride * frame.pixelSize(), 1, bytes,
                            out) != bytes) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int dumpToYuv(const std::filesystem::path& input, const std::filesystem::path& output,
              int max_frames, int skip_frames, DumpStats* stats, std::string* error) {
    std::string local_error;
    std::unique_ptr<demux::IDemuxer> demuxer = demux::createDemuxer(input, local_error);
    if (!demuxer) {
        if (error) {
            *error = local_error;
        }
        return 1;
    }

    FILE* out = std::fopen(output.string().c_str(), "wb");
    if (!out) {
        if (error) {
            *error = "failed to open output file " + output.string();
        }
        return 1;
    }

    KvzdecDecoder decoder;
    mpeg12::Decoder mpeg12_decoder;
    const demux::Codec codec = demuxer->video().codec;

    if (codec == demux::Codec::Mpeg1 || codec == demux::Codec::Mpeg2) {
        mpeg12_decoder.reset();
    } else {
        const int threads = std::getenv("HEVC_PLAYER_THREADS")
                                ? std::atoi(std::getenv("HEVC_PLAYER_THREADS"))
                                : -1;
        const int owf = std::getenv("HEVC_PLAYER_OWF") ? std::atoi(std::getenv("HEVC_PLAYER_OWF"))
                                                         : -1;
        if (!decoder.open(&local_error, threads, owf)) {
            std::fclose(out);
            if (error) {
                *error = local_error;
            }
            return 1;
        }
        if (!demuxer->video().hvcc.empty() &&
            !decoder.pushParameterSets(demuxer->video().hvcc, &local_error)) {
            std::fclose(out);
            if (error) {
                *error = local_error;
            }
            return 1;
        }
    }

    // Feeds one sample to the selected backend and drains its pictures.
    const auto pushSample = [&](const demux::Sample& sample, std::vector<DecodedFrame>& frames) {
        if (codec == demux::Codec::Mpeg1 || codec == demux::Codec::Mpeg2) {
            mpeg12_decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            return mpeg12_decoder.pullFrames(frames);
        }
        if (!decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                demuxer->video().format.annexb,
                                demuxer->video().format.nal_length_size)) {
            return 0;
        }
        return decoder.pullFrames(frames);
    };
    const auto flushDecoder = [&](std::vector<DecodedFrame>& frames) {
        return (codec == demux::Codec::Mpeg1 || codec == demux::Codec::Mpeg2) ? mpeg12_decoder.flush(frames) : decoder.flush(frames);
    };

    const auto start = std::chrono::steady_clock::now();
    DumpStats local_stats;
    local_stats.width = demuxer->video().width;
    local_stats.height = demuxer->video().height;

    int64_t decoded_count = 0;
    const auto writeFrames = [&](std::vector<DecodedFrame>& frames) {
        bool wrote = true;
        for (DecodedFrame& frame : frames) {
            if (skip_frames > 0 && decoded_count < skip_frames) {
                ++decoded_count;
                continue;
            }
            if (max_frames >= 0 && local_stats.frames >= max_frames) {
                break;
            }
            if (!writePicture(out, frame.image)) {
                local_error = "failed to write output";
                wrote = false;
                break;
            }
            ++local_stats.frames;
            ++decoded_count;
        }
        return wrote;
    };

    std::vector<DecodedFrame> frames;
    demux::Sample sample;
    bool ok = true;
    while (demuxer->readSample(sample)) {
        local_stats.bytes += static_cast<int64_t>(sample.data.size());
        frames.clear();
        if (pushSample(sample, frames) == 0 && !sample.data.empty() &&
            codec != demux::Codec::Mpeg1 && codec != demux::Codec::Mpeg2) {
            local_error = "decoder rejected a sample";
            ok = false;
            break;
        }
        if (!writeFrames(frames)) {
            ok = false;
            break;
        }
        if (max_frames >= 0 && local_stats.frames >= max_frames) {
            break;
        }
    }

    if (ok && (max_frames < 0 || local_stats.frames < max_frames)) {
        frames.clear();
        flushDecoder(frames);
        if (!writeFrames(frames)) {
            ok = false;
        }
    }

    local_stats.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fclose(out);

    if (!ok) {
        if (error) {
            *error = local_error;
        }
        return 1;
    }
    if (stats) {
        *stats = local_stats;
    }
    return 0;
}

}  // namespace player::decode


