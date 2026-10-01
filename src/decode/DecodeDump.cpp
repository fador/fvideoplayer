#include "decode/DecodeDump.hpp"

#include "decode/KvzdecDecoder.hpp"
#include "demux/Demuxer.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace player::decode {
namespace {

bool writePicture(FILE* out, const kvzdec_picture* picture) {
    for (int plane = 0; plane < 3; ++plane) {
        if (plane > 0 && picture->chroma_format == KVZDEC_CSP_400) {
            break;
        }
        const int shift_w =
            (plane > 0 && picture->chroma_format != KVZDEC_CSP_444) ? 1 : 0;
        const int shift_h = (plane > 0 && picture->chroma_format == KVZDEC_CSP_420) ? 1 : 0;
        const int width = (picture->width - picture->crop_left - picture->crop_right) >> shift_w;
        const int height = (picture->height - picture->crop_top - picture->crop_bottom) >> shift_h;
        const int stride = plane > 0 ? picture->stride_c : picture->stride;
        const uint8_t* source =
            static_cast<const uint8_t*>(picture->data[plane]) +
            (static_cast<size_t>(picture->crop_top >> shift_h) * stride +
             (picture->crop_left >> shift_w)) *
                picture->pixel_size;
        for (int y = 0; y < height; ++y) {
            const size_t bytes = static_cast<size_t>(width) * picture->pixel_size;
            if (std::fwrite(source + static_cast<size_t>(y) * stride * picture->pixel_size, 1, bytes,
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
    const int threads = std::getenv("HEVC_PLAYER_THREADS") ? std::atoi(std::getenv("HEVC_PLAYER_THREADS")) : -1;
    const int owf = std::getenv("HEVC_PLAYER_OWF") ? std::atoi(std::getenv("HEVC_PLAYER_OWF")) : -1;
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
            if (!writePicture(out, frame.picture)) {
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
        if (!decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                demuxer->video().format.annexb,
                                demuxer->video().format.nal_length_size)) {
            local_error = "decoder rejected a sample";
            ok = false;
            break;
        }
        frames.clear();
        decoder.pullFrames(frames);
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
        decoder.flush(frames);
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
