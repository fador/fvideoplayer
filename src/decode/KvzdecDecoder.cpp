#include "decode/KvzdecDecoder.hpp"

// The backend is external: when its sources are absent the build defines
// PLAYER_HAS_KVZDEC=0 and this file compiles to a stub that refuses to open,
// so the player, the dump path and the tests still link and run.
#if PLAYER_HAS_KVZDEC

#include "decode/NalUnits.hpp"
#include "demux/HvcC.hpp"

#include <kvzdec.h>

#include <algorithm>
#include <utility>

namespace player::decode {

// kvzdec state, kept out of the public header so that only this translation
// unit depends on the kvzdec types.
struct KvzdecDecoder::Impl {
    const kvzdec_api* api = nullptr;
    kvzdec_config* config = nullptr;
    kvzdec_decoder* decoder = nullptr;
};

namespace {

ChromaFormat toChromaFormat(kvzdec_chroma_format format) {
    switch (format) {
        case KVZDEC_CSP_400:
            return ChromaFormat::Mono;
        case KVZDEC_CSP_422:
            return ChromaFormat::C422;
        case KVZDEC_CSP_444:
            return ChromaFormat::C444;
        case KVZDEC_CSP_420:
        default:
            return ChromaFormat::C420;
    }
}

void freeKvzdecPicture(void* payload) {
    kvzdec_picture* picture = static_cast<kvzdec_picture*>(payload);
    const kvzdec_api* api = kvzdec_api_get();
    if (api && api->picture_free) {
        api->picture_free(picture);
    }
}

SliceType toSliceType(kvzdec_slice_type type) {
    switch (type) {
        case KVZDEC_SLICE_I:
            return SliceType::I;
        case KVZDEC_SLICE_P:
            return SliceType::P;
        case KVZDEC_SLICE_B:
            return SliceType::B;
        default:
            return SliceType::Unknown;
    }
}

HashResult toHashResult(kvzdec_hash_result result) {
    switch (result) {
        case KVZDEC_HASH_MATCH:
            return HashResult::Match;
        case KVZDEC_HASH_MISMATCH:
            return HashResult::Mismatch;
        default:
            return HashResult::NotPresent;
    }
}

}  // namespace

bool KvzdecDecoder::isOpen() const {
    return impl_ != nullptr && impl_->decoder != nullptr;
}

const char* hevcBackendVersion() {
    const char* version = kvzdec_get_version_string();
    return version != nullptr ? version : "unknown";
}

KvzdecDecoder::~KvzdecDecoder() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool KvzdecDecoder::open(std::string* error, int threads, int owf, bool hash_check) {
    close();
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    impl_->api = kvzdec_api_get();
    if (!impl_->api) {
        if (error) {
            *error = "kvzdec_api_get failed";
        }
        return false;
    }
    impl_->config = impl_->api->config_alloc();
    if (!impl_->config || !impl_->api->config_init(impl_->config)) {
        if (error) {
            *error = "failed to initialize kvzdec config";
        }
        close();
        return false;
    }
    impl_->config->threads = threads;
    impl_->config->owf = owf;
    impl_->config->cpuid = 1;
    impl_->config->hash_check = hash_check ? KVZDEC_HASH_CHECK_WARN : KVZDEC_HASH_CHECK_NONE;

    impl_->decoder = impl_->api->decoder_open(impl_->config);
    if (!impl_->decoder) {
        if (error) {
            *error = "failed to open kvzdec decoder";
        }
        close();
        return false;
    }
    return true;
}

void KvzdecDecoder::close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->decoder && impl_->api) {
        // Flush and release any pictures the decoder still holds; closing
        // without draining corrupts the decoder state.
        {
            std::vector<DecodedFrame> pending;
            impl_->api->decoder_flush(impl_->decoder);
            pullFrames(pending);
        }
        impl_->api->decoder_close(impl_->decoder);
        impl_->decoder = nullptr;
    }
    if (impl_->config && impl_->api) {
        impl_->api->config_destroy(impl_->config);
        impl_->config = nullptr;
    }
    impl_->api = nullptr;
}

bool KvzdecDecoder::pushParameterSets(const std::vector<uint8_t>& hvcc, std::string* error) {
    if (impl_ == nullptr || impl_->decoder == nullptr) {
        if (error) {
            *error = "decoder is not open";
        }
        return false;
    }
    if (hvcc.empty()) {
        return true;
    }
    demux::HvccInfo info;
    std::string parse_error;
    if (!demux::parseHvcc(hvcc.data(), hvcc.size(), info, &parse_error)) {
        if (error) {
            *error = "failed to parse hvcC: " + parse_error;
        }
        return false;
    }
    for (const demux::HvccNalUnit& nal : info.nals) {
        if (nal.nal_type != 32 && nal.nal_type != 33 && nal.nal_type != 34) {
            continue;
        }
        if (!impl_->api->decoder_push_nal(impl_->decoder, nal.data.data(), nal.data.size(), 0)) {
            if (error) {
                *error = "kvzdec rejected a parameter set NAL";
            }
            return false;
        }
    }
    return true;
}

bool KvzdecDecoder::pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                               uint8_t nal_length_size) {
    if (impl_ == nullptr || impl_->decoder == nullptr || !data || size == 0) {
        return false;
    }
    if (annexb) {
        constexpr size_t kPushChunk = 1 << 16;
        for (size_t offset = 0; offset < size; offset += kPushChunk) {
            const size_t count = std::min(kPushChunk, size - offset);
            if (!impl_->api->decoder_push(impl_->decoder, data + offset, count, pts)) {
                return false;
            }
        }
        return true;
    }

    std::vector<NalView> nals;
    if (!splitLengthPrefixed(data, size, nal_length_size, nals)) {
        return false;
    }
    for (const NalView& nal : nals) {
        if (!impl_->api->decoder_push_nal(impl_->decoder, nal.data, nal.size, pts)) {
            return false;
        }
    }
    return true;
}

int KvzdecDecoder::pullFrames(std::vector<DecodedFrame>& out) {
    if (impl_ == nullptr || impl_->decoder == nullptr) {
        return 0;
    }
    int count = 0;
    for (;;) {
        kvzdec_picture* picture = nullptr;
        kvzdec_frame_info info{};
        if (!impl_->api->decoder_get_picture(impl_->decoder, &picture, &info) || !picture) {
            break;
        }

        DecodedFrame frame;
        uint8_t* planes[3] = {
            static_cast<uint8_t*>(picture->y),
            static_cast<uint8_t*>(picture->u),
            static_cast<uint8_t*>(picture->v),
        };
        frame.image.adopt(picture, &freeKvzdecPicture, planes, picture->width, picture->height,
                          picture->stride, picture->width_c, picture->height_c,
                          picture->stride_c, toChromaFormat(picture->chroma_format),
                          picture->pixel_size, picture->bitdepth, picture->bitdepth_c);
        frame.image.crop_left_ = picture->crop_left;
        frame.image.crop_right_ = picture->crop_right;
        frame.image.crop_top_ = picture->crop_top;
        frame.image.crop_bottom_ = picture->crop_bottom;
        frame.image.pts = picture->pts;
        frame.image.poc = picture->poc;
        frame.pts = picture->pts;
        frame.poc = info.poc;
        frame.qp = info.qp;
        frame.slice_type = toSliceType(info.slice_type);
        frame.hash_result = toHashResult(static_cast<kvzdec_hash_result>(info.hash_result));
        out.push_back(std::move(frame));
        ++count;
    }
    return count;
}

int KvzdecDecoder::flush(std::vector<DecodedFrame>& out) {
    if (impl_ == nullptr || impl_->decoder == nullptr) {
        return 0;
    }
    impl_->api->decoder_flush(impl_->decoder);
    return pullFrames(out);
}

}  // namespace player::decode

#else  // !PLAYER_HAS_KVZDEC

namespace player::decode {
namespace {

constexpr const char* kNoBackend =
    "HEVC support was not built (kvzdec was not available or was disabled)";

}  // namespace

bool KvzdecDecoder::isOpen() const {
    return false;
}

const char* hevcBackendVersion() {
    return "not built";
}

KvzdecDecoder::~KvzdecDecoder() = default;

bool KvzdecDecoder::open(std::string* error, int threads, int owf, bool hash_check) {
    (void)threads;
    (void)owf;
    (void)hash_check;
    if (error) {
        *error = kNoBackend;
    }
    return false;
}

void KvzdecDecoder::close() {}

bool KvzdecDecoder::pushParameterSets(const std::vector<uint8_t>& hvcc, std::string* error) {
    (void)hvcc;
    if (error) {
        *error = kNoBackend;
    }
    return false;
}

bool KvzdecDecoder::pushSample(const uint8_t* data, size_t size, int64_t pts, bool annexb,
                               uint8_t nal_length_size) {
    (void)data;
    (void)size;
    (void)pts;
    (void)annexb;
    (void)nal_length_size;
    return false;
}

int KvzdecDecoder::pullFrames(std::vector<DecodedFrame>& out) {
    (void)out;
    return 0;
}

int KvzdecDecoder::flush(std::vector<DecodedFrame>& out) {
    (void)out;
    return 0;
}

}  // namespace player::decode

#endif  // PLAYER_HAS_KVZDEC
