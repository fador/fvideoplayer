#include "decode/F264Decoder.hpp"

#include "decode/NalUnits.hpp"
#include "demux/Avcc.hpp"

// The backend is external: when its sources are absent the build defines
// PLAYER_HAS_F264DEC=0 and this file compiles to a stub that refuses to open.
#if PLAYER_HAS_F264DEC

#include <f264dec.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace player::decode {
namespace {

void appendAnnexBNal(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), data, data + size);
}

ChromaFormat toChromaFormat(f264_chroma_format format) {
    switch (format) {
        case F264_CSP_400:
            return ChromaFormat::Mono;
        case F264_CSP_422:
            return ChromaFormat::C422;
        case F264_CSP_444:
            return ChromaFormat::C444;
        case F264_CSP_420:
        default:
            return ChromaFormat::C420;
    }
}

void copyPlane(const uint8_t* source, int stride, int width, int height, int pixel_size,
               uint8_t* destination, int destination_stride) {
    const size_t row_bytes = static_cast<size_t>(width) * pixel_size;
    for (int y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * destination_stride * pixel_size,
                    source + static_cast<size_t>(y) * stride, row_bytes);
    }
}

// Copies a decoded picture out of the decoder's aliased buffers.
bool convertPicture(const f264_picture& picture, DecodedFrame& frame) {
    const ChromaFormat format = toChromaFormat(picture.chroma_format);
    const int pixel_size = picture.bit_depth > 8 ? 2 : 1;
    if (!frame.image.alloc(picture.width, picture.height, format, picture.bit_depth)) {
        return false;
    }
    copyPlane(picture.y, picture.stride, picture.width, picture.height, pixel_size,
              frame.image.plane(0), frame.image.planeStride(0));
    if (format != ChromaFormat::Mono && picture.u != nullptr && picture.v != nullptr) {
        copyPlane(picture.u, picture.stride_c, picture.width_c, picture.height_c, pixel_size,
                  frame.image.plane(1), frame.image.planeStride(1));
        copyPlane(picture.v, picture.stride_c, picture.width_c, picture.height_c, pixel_size,
                  frame.image.plane(2), frame.image.planeStride(2));
    }
    frame.image.poc = picture.poc;
    frame.poc = picture.poc;
    return true;
}

}  // namespace

struct F264Decoder::Impl {
    const f264_api* api = nullptr;
    f264_decoder* decoder = nullptr;

    // Access units pushed but not yet decoded, for the one-AU lookahead.
    int pending = 0;
    bool eos = false;

    // Output timeline: H.264 carries no timestamps, so the first sample's PTS
    // is the base and the smallest positive input step is the frame interval.
    int64_t base_pts = 0;
    int64_t frame_step = 0;
    int64_t last_input_pts = 0;
    int64_t output_index = 0;
    bool have_base = false;

    std::string last_error;
    std::vector<uint8_t> annexb;
};

void F264Decoder::decoderErrorCallback(void* user_data, int code, const char* message) {
    Impl* impl = static_cast<Impl*>(user_data);
    if (impl == nullptr) {
        return;
    }
    impl->last_error = "f264dec error " + std::to_string(code) + ": " +
                       (message != nullptr ? message : "unknown");
}

F264Decoder::F264Decoder() = default;

F264Decoder::~F264Decoder() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool F264Decoder::isOpen() const {
    return impl_ != nullptr && impl_->decoder != nullptr;
}

const char* h264BackendVersion() {
    const char* version = f264_get_version_string();
    return version != nullptr ? version : "unknown";
}

bool F264Decoder::open(std::string* error, int threads) {
    close();
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->api = f264_api_get(8);
    if (impl_->api == nullptr) {
        if (error) {
            *error = "f264_api_get failed";
        }
        return false;
    }
    f264_config* config = impl_->api->config_alloc();
    if (config == nullptr) {
        if (error) {
            *error = "failed to allocate f264dec config";
        }
        return false;
    }
    config->memory_input = 1;
    config->silent = 1;
    config->cpuid = 1;
    config->threads = threads;
    config->error_cb = &F264Decoder::decoderErrorCallback;
    config->error_cb_user_data = impl_;
    impl_->decoder = impl_->api->decoder_open(config);
    impl_->api->config_destroy(config);
    if (impl_->decoder == nullptr) {
        if (error) {
            *error = impl_->last_error.empty() ? "failed to open f264dec decoder"
                                               : impl_->last_error;
        }
        return false;
    }
    return true;
}

void F264Decoder::close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->decoder != nullptr && impl_->api != nullptr) {
        impl_->api->decoder_close(impl_->decoder);
        impl_->decoder = nullptr;
    }
    impl_->api = nullptr;
    impl_->pending = 0;
    impl_->eos = false;
    impl_->base_pts = 0;
    impl_->frame_step = 0;
    impl_->last_input_pts = 0;
    impl_->output_index = 0;
    impl_->have_base = false;
    impl_->last_error.clear();
    impl_->annexb.clear();
}

bool F264Decoder::pushParameterSets(const std::vector<uint8_t>& avcc, std::string* error) {
    if (!isOpen()) {
        if (error) {
            *error = "decoder is not open";
        }
        return false;
    }
    if (avcc.empty()) {
        return true;
    }
    demux::AvccInfo info;
    std::string parse_error;
    if (!demux::parseAvcc(avcc.data(), avcc.size(), info, &parse_error)) {
        if (error) {
            *error = "failed to parse avcC: " + parse_error;
        }
        return false;
    }
    std::vector<uint8_t> annexb;
    for (const demux::AvccNalUnit& nal : info.sps) {
        demux::appendAnnexBNal(nal, annexb);
    }
    for (const demux::AvccNalUnit& nal : info.pps) {
        demux::appendAnnexBNal(nal, annexb);
    }
    if (impl_->api->decoder_push(impl_->decoder, annexb.data(), annexb.size()) < 0) {
        if (error) {
            *error = impl_->last_error.empty() ? "f264dec rejected the parameter sets"
                                               : impl_->last_error;
        }
        return false;
    }
    return true;
}

bool F264Decoder::pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                             uint8_t nal_length_size) {
    if (!isOpen() || data == nullptr || size == 0) {
        return false;
    }

    const uint8_t* payload = data;
    size_t payload_size = size;
    if (!annexb) {
        std::vector<NalView> nals;
        if (!splitLengthPrefixed(data, size, nal_length_size, nals)) {
            return false;
        }
        impl_->annexb.clear();
        for (const NalView& nal : nals) {
            appendAnnexBNal(nal.data, nal.size, impl_->annexb);
        }
        payload = impl_->annexb.data();
        payload_size = impl_->annexb.size();
        if (payload_size == 0) {
            return false;
        }
    }

    if (impl_->api->decoder_push(impl_->decoder, payload, payload_size) < 0) {
        return false;
    }

    if (!impl_->have_base) {
        impl_->base_pts = pts;
        impl_->have_base = true;
    } else {
        const int64_t step = pts - impl_->last_input_pts;
        if (step > 0 && (impl_->frame_step == 0 || step < impl_->frame_step)) {
            impl_->frame_step = step;
        }
    }
    impl_->last_input_pts = pts;
    ++impl_->pending;
    return true;
}

int F264Decoder::pullFrames(std::vector<DecodedFrame>& out) {
    if (!isOpen()) {
        return 0;
    }
    int count = 0;
    const auto drain = [&] {
        for (;;) {
            f264_picture* picture = impl_->api->decoder_get_picture(impl_->decoder);
            if (picture == nullptr) {
                break;
            }
            DecodedFrame frame;
            if (!convertPicture(*picture, frame)) {
                continue;
            }
            frame.pts = impl_->base_pts + impl_->output_index * impl_->frame_step;
            frame.image.pts = frame.pts;
            ++impl_->output_index;
            out.push_back(std::move(frame));
            ++count;
        }
    };

    // One access unit of lookahead: only decode once the next one is in, so
    // the decoder can see the next picture's first slice.
    while (impl_->pending >= 2 && !impl_->eos) {
        const int result = impl_->api->decoder_decode(impl_->decoder, nullptr);
        if (result == F264_EOS) {
            impl_->eos = true;
            break;
        }
        if (result != F264_OK) {
            break;
        }
        --impl_->pending;
        drain();
    }
    drain();
    return count;
}

int F264Decoder::flush(std::vector<DecodedFrame>& out) {
    if (!isOpen()) {
        return 0;
    }
    int count = 0;
    const auto drain = [&] {
        for (;;) {
            f264_picture* picture = impl_->api->decoder_get_picture(impl_->decoder);
            if (picture == nullptr) {
                break;
            }
            DecodedFrame frame;
            if (!convertPicture(*picture, frame)) {
                continue;
            }
            frame.pts = impl_->base_pts + impl_->output_index * impl_->frame_step;
            frame.image.pts = frame.pts;
            ++impl_->output_index;
            out.push_back(std::move(frame));
            ++count;
        }
    };

    while (impl_->pending > 0 && !impl_->eos) {
        const int result = impl_->api->decoder_decode(impl_->decoder, nullptr);
        if (result == F264_EOS) {
            impl_->eos = true;
            break;
        }
        if (result != F264_OK) {
            break;
        }
        --impl_->pending;
        drain();
    }
    for (;;) {
        f264_picture* picture = nullptr;
        const int result = impl_->api->decoder_flush(impl_->decoder, &picture);
        if (picture != nullptr) {
            DecodedFrame frame;
            if (convertPicture(*picture, frame)) {
                frame.pts = impl_->base_pts + impl_->output_index * impl_->frame_step;
                frame.image.pts = frame.pts;
                ++impl_->output_index;
                out.push_back(std::move(frame));
                ++count;
            }
        }
        if (result != F264_OK) {
            break;
        }
    }
    drain();
    return count;
}

std::string F264Decoder::takeLastError() {
    if (impl_ == nullptr) {
        return {};
    }
    std::string error = std::move(impl_->last_error);
    impl_->last_error.clear();
    return error;
}

}  // namespace player::decode

#else  // !PLAYER_HAS_F264DEC

namespace player::decode {
namespace {

constexpr const char* kNoBackend =
    "H.264 support was not built (f264dec was not available or was disabled)";

}  // namespace

struct F264Decoder::Impl {};

F264Decoder::F264Decoder() = default;

F264Decoder::~F264Decoder() {
    delete impl_;
    impl_ = nullptr;
}

bool F264Decoder::isOpen() const {
    return false;
}

const char* h264BackendVersion() {
    return "not built";
}

bool F264Decoder::open(std::string* error, int threads) {
    (void)threads;
    if (error) {
        *error = kNoBackend;
    }
    return false;
}

void F264Decoder::close() {}

bool F264Decoder::pushParameterSets(const std::vector<uint8_t>& avcc, std::string* error) {
    (void)avcc;
    if (error) {
        *error = kNoBackend;
    }
    return false;
}

bool F264Decoder::pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                             uint8_t nal_length_size) {
    (void)data;
    (void)size;
    (void)pts;
    (void)annexb;
    (void)nal_length_size;
    return false;
}

int F264Decoder::pullFrames(std::vector<DecodedFrame>& out) {
    (void)out;
    return 0;
}

int F264Decoder::flush(std::vector<DecodedFrame>& out) {
    (void)out;
    return 0;
}

std::string F264Decoder::takeLastError() {
    return {};
}

}  // namespace player::decode

#endif  // PLAYER_HAS_F264DEC
