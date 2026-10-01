#include "decode/KvzdecDecoder.hpp"

#include "decode/NalUnits.hpp"
#include "demux/HvcC.hpp"

#include <utility>

namespace player::decode {

DecodedFrame::~DecodedFrame() {
    if (picture) {
        const kvzdec_api* api = kvzdec_api_get();
        if (api && api->picture_free) {
            api->picture_free(picture);
        }
        picture = nullptr;
    }
}

DecodedFrame::DecodedFrame(DecodedFrame&& other) noexcept
    : picture(other.picture),
      info(other.info),
      pts(other.pts),
      decode_us(other.decode_us),
      sample_bytes(other.sample_bytes) {
    other.picture = nullptr;
}

DecodedFrame& DecodedFrame::operator=(DecodedFrame&& other) noexcept {
    if (this != &other) {
        if (picture) {
            const kvzdec_api* api = kvzdec_api_get();
            if (api && api->picture_free) {
                api->picture_free(picture);
            }
        }
        picture = other.picture;
        info = other.info;
        pts = other.pts;
        decode_us = other.decode_us;
        sample_bytes = other.sample_bytes;
        other.picture = nullptr;
    }
    return *this;
}

KvzdecDecoder::~KvzdecDecoder() {
    close();
}

bool KvzdecDecoder::open(std::string* error, int threads, int owf, bool hash_check) {
    close();
    api_ = kvzdec_api_get();
    if (!api_) {
        if (error) {
            *error = "kvzdec_api_get failed";
        }
        return false;
    }
    config_ = api_->config_alloc();
    if (!config_ || !api_->config_init(config_)) {
        if (error) {
            *error = "failed to initialize kvzdec config";
        }
        close();
        return false;
    }
    config_->threads = threads;
    config_->owf = owf;
    config_->cpuid = 1;
    config_->hash_check = hash_check ? KVZDEC_HASH_CHECK_WARN : KVZDEC_HASH_CHECK_NONE;

    decoder_ = api_->decoder_open(config_);
    if (!decoder_) {
        if (error) {
            *error = "failed to open kvzdec decoder";
        }
        close();
        return false;
    }
    return true;
}

void KvzdecDecoder::close() {
    if (decoder_ && api_) {
        api_->decoder_close(decoder_);
        decoder_ = nullptr;
    }
    if (config_ && api_) {
        api_->config_destroy(config_);
        config_ = nullptr;
    }
    api_ = nullptr;
}

bool KvzdecDecoder::pushParameterSets(const std::vector<uint8_t>& hvcc, std::string* error) {
    if (!decoder_) {
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
        if (!api_->decoder_push_nal(decoder_, nal.data.data(), nal.data.size(), 0)) {
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
    if (!decoder_ || !data || size == 0) {
        return false;
    }
    if (annexb) {
        return api_->decoder_push(decoder_, data, size, pts) != 0;
    }

    std::vector<NalView> nals;
    if (!splitLengthPrefixed(data, size, nal_length_size, nals)) {
        return false;
    }
    for (const NalView& nal : nals) {
        if (!api_->decoder_push_nal(decoder_, nal.data, nal.size, pts)) {
            return false;
        }
    }
    return true;
}

int KvzdecDecoder::pullFrames(std::vector<DecodedFrame>& out) {
    if (!decoder_) {
        return 0;
    }
    int count = 0;
    for (;;) {
        kvzdec_picture* picture = nullptr;
        kvzdec_frame_info info{};
        if (!api_->decoder_get_picture(decoder_, &picture, &info) || !picture) {
            break;
        }
        DecodedFrame frame;
        frame.picture = picture;
        frame.info = info;
        frame.pts = picture->pts;
        out.push_back(std::move(frame));
        ++count;
    }
    return count;
}

int KvzdecDecoder::flush(std::vector<DecodedFrame>& out) {
    if (!decoder_) {
        return 0;
    }
    api_->decoder_flush(decoder_);
    return pullFrames(out);
}

}  // namespace player::decode
