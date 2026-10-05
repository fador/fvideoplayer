#pragma once

#include "decode/KvzdecDecoder.hpp"  // DecodedFrame

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player::decode {

// Thin wrapper around the f264dec H.264/AVC decoder API. Produces
// codec-neutral Frames by copying each decoded picture out of the decoder's
// aliased buffers.
//
// The decoder needs one access unit of lookahead to detect the end of a
// picture, so pushSample only feeds the decoder and pullFrames decodes one
// access unit behind the input.
class F264Decoder {
public:
    F264Decoder();
    ~F264Decoder();

    F264Decoder(const F264Decoder&) = delete;
    F264Decoder& operator=(const F264Decoder&) = delete;

    bool open(std::string* error = nullptr, int threads = -1);
    void close();
    bool isOpen() const;

    // Pushes the SPS/PPS of an AVCDecoderConfigurationRecord (avcC) in Annex B
    // form. Call before the first sample of a length-prefixed stream.
    bool pushParameterSets(const std::vector<uint8_t>& avcc, std::string* error = nullptr);

    // Feeds one access unit. Annex B data is passed through; length-prefixed
    // samples are split into NAL units and converted to Annex B.
    bool pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                    uint8_t nal_length_size);

    // Moves all currently available pictures into out. Returns the count.
    int pullFrames(std::vector<DecodedFrame>& out);

    // Decodes the remaining access units and moves the remaining pictures,
    // including the ones the DPB still holds, into out. Returns the count.
    int flush(std::vector<DecodedFrame>& out);

    // Last error reported by the decoder since the previous call. Empty when
    // nothing was reported.
    std::string takeLastError();

private:
    // Decoder state; defined in the .cpp so the public header stays free of
    // f264dec types.
    struct Impl;

    // Receives decoder errors instead of the library printing them.
    static void decoderErrorCallback(void* user_data, int code, const char* message);

    Impl* impl_ = nullptr;
};

// Version string of the built-in H.264 backend, for the startup log line.
const char* h264BackendVersion();

}  // namespace player::decode
