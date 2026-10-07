#include "decode/DecodeDump.hpp"

#include "decode/F264Decoder.hpp"
#include "decode/KvzdecDecoder.hpp"
#include "decode/flv1/Flv1Decoder.hpp"
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
        // Planar 4:2:0 rounds the chroma plane up, so an odd display height
        // keeps its last half row in the dump.
        const int width = (frame.displayWidth() + ((1 << shift_w) - 1)) >> shift_w;
        const int height = (frame.displayHeight() + ((1 << shift_h) - 1)) >> shift_h;
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
    F264Decoder h264_decoder;
    mpeg12::Decoder mpeg12_decoder;
    flv1::Decoder flv1_decoder;
    const demux::Codec codec = demuxer->video().codec;
    const bool is_mpeg = codec == demux::Codec::Mpeg1 || codec == demux::Codec::Mpeg2;
    const bool is_flv1 = codec == demux::Codec::Flv1;
    const bool is_h264 = codec == demux::Codec::H264;

    if (is_mpeg) {
        mpeg12_decoder.reset();
    } else if (is_flv1) {
        flv1_decoder.reset();
    } else if (is_h264) {
        if (!h264_decoder.open(&local_error)) {
            std::fclose(out);
            if (error) {
                *error = local_error;
            }
            return 1;
        }
        if (!demuxer->video().avcc.empty() &&
            !h264_decoder.pushParameterSets(demuxer->video().avcc, &local_error)) {
            std::fclose(out);
            if (error) {
                *error = local_error;
            }
            return 1;
        }
    } else {
        const int threads = std::getenv("FVIDEOPLAYER_THREADS")
                                ? std::atoi(std::getenv("FVIDEOPLAYER_THREADS"))
                                : -1;
        const int owf = std::getenv("FVIDEOPLAYER_OWF")
                            ? std::atoi(std::getenv("FVIDEOPLAYER_OWF"))
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
    // Returns false only when the decoder refuses the sample: an empty frame
    // list is normal while the decoder holds an access unit back for
    // reordering or picture-boundary detection.
    const auto pushSample = [&](const demux::Sample& sample,
                                std::vector<DecodedFrame>& frames) -> bool {
        if (is_mpeg) {
            mpeg12_decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            mpeg12_decoder.pullFrames(frames);
            return true;
        }
        if (is_flv1) {
            flv1_decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            flv1_decoder.pullFrames(frames);
            return true;
        }
        if (is_h264) {
            if (!h264_decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                         demuxer->video().format.annexb,
                                         demuxer->video().format.nal_length_size)) {
                return false;
            }
            h264_decoder.pullFrames(frames);
            return true;
        }
        if (!decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                demuxer->video().format.annexb,
                                demuxer->video().format.nal_length_size)) {
            return false;
        }
        decoder.pullFrames(frames);
        return true;
    };
    const auto flushDecoder = [&](std::vector<DecodedFrame>& frames) {
        if (is_mpeg) {
            return mpeg12_decoder.flush(frames);
        }
        if (is_flv1) {
            return flv1_decoder.flush(frames);
        }
        if (is_h264) {
            return h264_decoder.flush(frames);
        }
        return decoder.flush(frames);
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
        if (!pushSample(sample, frames)) {
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
    if (is_mpeg) {
        local_stats.damaged = static_cast<int64_t>(mpeg12_decoder.damagedPictures());
        local_stats.damage_mb_x = mpeg12_decoder.damageMbX();
        local_stats.damage_mb_y = mpeg12_decoder.damageMbY();
        local_stats.damage_bit = mpeg12_decoder.damageBit();
        local_stats.damage_reason = mpeg12_decoder.damageReason();
    } else if (is_flv1) {
        local_stats.damaged = flv1_decoder.damaged() ? 1 : 0;
    }
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


