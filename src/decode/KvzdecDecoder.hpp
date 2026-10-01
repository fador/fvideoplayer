#pragma once

#include <kvzdec.h>

#include <cstdint>
#include <string>
#include <vector>

namespace player::decode {

// Owns a decoded kvzdec picture and its metadata.
struct DecodedFrame {
    kvzdec_picture* picture = nullptr;
    kvzdec_frame_info info{};
    int64_t pts = 0;
    int64_t decode_us = 0;    // wall time measured by the caller
    size_t sample_bytes = 0;  // compressed bytes of the originating sample

    DecodedFrame() = default;
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&& other) noexcept;
    DecodedFrame& operator=(DecodedFrame&& other) noexcept;
    DecodedFrame(const DecodedFrame&) = delete;
    DecodedFrame& operator=(const DecodedFrame&) = delete;
};

// Thin wrapper around the kvzdec decoder API.
class KvzdecDecoder {
public:
    KvzdecDecoder() = default;
    ~KvzdecDecoder();

    KvzdecDecoder(const KvzdecDecoder&) = delete;
    KvzdecDecoder& operator=(const KvzdecDecoder&) = delete;

    bool open(std::string* error = nullptr, int threads = -1, int owf = -1, bool hash_check = true);
    void close();
    bool isOpen() const { return decoder_ != nullptr; }

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

    const kvzdec_api* api() const { return api_; }

private:
    const kvzdec_api* api_ = nullptr;
    kvzdec_config* config_ = nullptr;
    kvzdec_decoder* decoder_ = nullptr;
};

}  // namespace player::decode
