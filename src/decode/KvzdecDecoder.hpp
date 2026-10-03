#pragma once

#include "decode/Frame.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace player::decode {

// A decoded picture plus the presentation metadata the pipeline needs.
struct DecodedFrame {
    Frame image;
    int64_t pts = 0;
    int64_t decode_us = 0;    // wall time measured by the caller
    size_t sample_bytes = 0;  // compressed bytes of the originating sample

    // Display order index of the picture, used by the stats overlay.
    int32_t poc = 0;
    int qp = 0;
    SliceType slice_type = SliceType::Unknown;
    HashResult hash_result = HashResult::NotPresent;
};

// Thin wrapper around the kvzdec decoder API. Produces codec-neutral Frames by
// adopting the kvzdec picture's sample storage.
class KvzdecDecoder {
public:
    KvzdecDecoder() = default;
    ~KvzdecDecoder();

    KvzdecDecoder(const KvzdecDecoder&) = delete;
    KvzdecDecoder& operator=(const KvzdecDecoder&) = delete;

    bool open(std::string* error = nullptr, int threads = -1, int owf = -1, bool hash_check = true);
    void close();
    bool isOpen() const;

    // Pushes the VPS/SPS/PPS NAL units of an hvcC record to the decoder.
    bool pushParameterSets(const std::vector<uint8_t>& hvcc, std::string* error = nullptr);

    // Feeds one compressed sample. Annex B data is passed through as a byte
    // stream; length-prefixed samples are split into NAL units.
    bool pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                    uint8_t nal_length_size);

    // Moves all currently available pictures into out. Returns the count.
    int pullFrames(std::vector<DecodedFrame>& out);

    // Signals end of stream and moves the remaining pictures into out.
    int flush(std::vector<DecodedFrame>& out);

private:
    // kvzdec state; defined in the .cpp so the public header stays free of
    // kvzdec types.
    struct Impl;

    Impl* impl_ = nullptr;
};

// Version string of the built-in HEVC backend, for the startup log line.
const char* hevcBackendVersion();

}  // namespace player::decode
