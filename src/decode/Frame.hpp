#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace player::decode {

// Chroma sampling of a decoded picture.
enum class ChromaFormat {
    Mono,  // luma only
    C420,
    C422,
    C444,
};

// Number of chroma planes the format carries (1 for Mono, 3 otherwise).
inline int chromaPlaneCount(ChromaFormat format) {
    return format == ChromaFormat::Mono ? 1 : 3;
}

// Picture coding type of the slice a frame came from. MPEG-1 has I, P and B
// pictures; the values are shared by every backend so the UI can label them.
enum class SliceType {
    Unknown = -1,
    B = 0,
    P = 1,
    I = 2,
};

// Result of a decoded-picture hash check against the container's picture hash.
enum class HashResult {
    NotPresent = 0,
    Match = 1,
    Mismatch = 2,
};

// Codec-neutral decoded picture: three planar sample arrays in display order.
//
// A frame either owns its sample storage (alloc) or borrows storage owned by a
// backend decoder (adopt), in which case the backend payload is released
// through native_free. This keeps the renderer, frame queue, stats and YUV dumper
// independent of any particular video codec.
class Frame {
public:
    Frame() = default;
    ~Frame() { release(); }

    Frame(Frame&& other) noexcept { moveFrom(other); }
    Frame& operator=(Frame&& other) noexcept {
        if (this != &other) {
            release();
            moveFrom(other);
        }
        return *this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    // Allocates zeroed, tightly packed planar storage. The frame takes
    // ownership and release() frees it. Returns false when width/height are
    // unusable or allocation fails.
    bool alloc(int width, int height, ChromaFormat format, int bitdepth = 8) {
        release();
        if (width <= 0 || height <= 0) {
            return false;
        }
        const int shift_w = format == ChromaFormat::C444 ? 0 : 1;
        const int shift_h = format == ChromaFormat::C420 ? 1 : 0;

        width_ = width;
        height_ = height;
        stride_ = width;
        format_ = format;
        pixel_size_ = bitdepth > 8 ? 2 : 1;
        bitdepth_ = bitdepth;
        bitdepth_c_ = bitdepth;
        if (format == ChromaFormat::Mono) {
            width_c_ = height_c_ = stride_c_ = 0;
        } else {
            width_c_ = (width + shift_w) >> shift_w;
            height_c_ = (height + shift_h) >> shift_h;
            stride_c_ = width_c_;
        }

        const size_t luma = static_cast<size_t>(stride_) * height_ * pixel_size_;
        const size_t chroma =
            format == ChromaFormat::Mono ? 0 : static_cast<size_t>(stride_c_) * height_c_ * pixel_size_;
        buffer_.assign(luma + 2 * chroma, 0);
        if (buffer_.empty()) {
            return false;
        }
        uint8_t* base = buffer_.data();
        data[0] = base;
        data[1] = format == ChromaFormat::Mono ? nullptr : base + luma;
        data[2] = format == ChromaFormat::Mono ? nullptr : base + luma + chroma;
        return true;
    }

    // Borrows sample storage owned by a backend decoder. `planes` must point
    // into memory that stays valid until release_native() is called; `payload`
    // is passed to `free_payload` on destruction.
    void adopt(void* payload, void (*free_payload)(void*), uint8_t* planes[3], int width, int height,
               int stride, int width_c, int height_c, int stride_c, ChromaFormat format,
               int pixel_size, int bitdepth, int bitdepth_c) {
        release();
        payload_ = payload;
        free_payload_ = free_payload;
        data[0] = planes[0];
        data[1] = planes[1];
        data[2] = planes[2];
        width_ = width;
        height_ = height;
        stride_ = stride;
        width_c_ = width_c;
        height_c_ = height_c;
        stride_c_ = stride_c;
        format_ = format;
        pixel_size_ = pixel_size;
        bitdepth_ = bitdepth;
        bitdepth_c_ = bitdepth_c;
    }

    // Releases the backend payload and any owned storage, and resets the
    // geometry. Safe to call repeatedly.
    void release() {
        if (payload_ != nullptr && free_payload_ != nullptr) {
            free_payload_(payload_);
        }
        payload_ = nullptr;
        free_payload_ = nullptr;
        buffer_.clear();
        buffer_.shrink_to_fit();
        data[0] = data[1] = data[2] = nullptr;
        width_ = height_ = stride_ = 0;
        width_c_ = height_c_ = stride_c_ = 0;
        format_ = ChromaFormat::Mono;
        pixel_size_ = 1;
        bitdepth_ = bitdepth_c_ = 8;
    }

    // Releases the backend payload but keeps the adopted sample pointers, for
    // callers that recycle a frame's memory pool.
    void releaseNative() {
        if (payload_ != nullptr && free_payload_ != nullptr) {
            free_payload_(payload_);
        }
        payload_ = nullptr;
        free_payload_ = nullptr;
    }

    bool valid() const { return data[0] != nullptr && width_ > 0 && height_ > 0; }

    // Bytes per sample (1 for 8-bit, 2 for high bit depth).
    int pixelSize() const { return pixel_size_; }
    ChromaFormat format() const { return format_; }
    int planeWidth(int plane) const { return plane == 0 ? width_ : width_c_; }
    int planeHeight(int plane) const { return plane == 0 ? height_ : height_c_; }
    int planeStride(int plane) const { return plane == 0 ? stride_ : stride_c_; }

    // Visible dimensions after applying the conformance window.
    int displayWidth() const { return width_ - crop_left_ - crop_right_; }
    int displayHeight() const { return height_ - crop_top_ - crop_bottom_; }

    uint8_t* plane(int index) const { return data[index]; }
    uint8_t* row(int index, int y) const {
        return data[index] + static_cast<size_t>(y) * planeStride(index) * pixel_size_;
    }

    // Fills every plane with a neutral mid-grey value (128 for 8-bit output).
    void fillNeutral() {
        if (!valid()) {
            return;
        }
        const int plane_count = chromaPlaneCount(format_);
        for (int plane = 0; plane < plane_count; ++plane) {
            const int width = plane == 0 ? width_ : width_c_;
            const int height = plane == 0 ? height_ : height_c_;
            const int stride = plane == 0 ? stride_ : stride_c_;
            if (width <= 0 || height <= 0 || data[plane] == nullptr) {
                continue;
            }
            const size_t row_bytes = static_cast<size_t>(width) * pixel_size_;
            for (int y = 0; y < height; ++y) {
                uint8_t* out = data[plane] + static_cast<size_t>(y) * stride * pixel_size_;
                for (size_t i = 0; i < row_bytes; ++i) {
                    out[i] = 128;
                }
            }
        }
    }

    uint8_t* data[3] = {nullptr, nullptr, nullptr};
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    int width_c_ = 0;
    int height_c_ = 0;
    int stride_c_ = 0;
    ChromaFormat format_ = ChromaFormat::Mono;
    int pixel_size_ = 1;
    int bitdepth_ = 8;
    int bitdepth_c_ = 8;
    int crop_left_ = 0;
    int crop_right_ = 0;
    int crop_top_ = 0;
    int crop_bottom_ = 0;

    int64_t pts = 0;
    int32_t poc = 0;

private:
    void moveFrom(Frame& other) {
        buffer_ = std::move(other.buffer_);
        payload_ = other.payload_;
        free_payload_ = other.free_payload_;
        other.payload_ = nullptr;
        other.free_payload_ = nullptr;
        for (int i = 0; i < 3; ++i) {
            data[i] = other.data[i];
            other.data[i] = nullptr;
        }
        width_ = other.width_;
        height_ = other.height_;
        stride_ = other.stride_;
        width_c_ = other.width_c_;
        height_c_ = other.height_c_;
        stride_c_ = other.stride_c_;
        format_ = other.format_;
        pixel_size_ = other.pixel_size_;
        bitdepth_ = other.bitdepth_;
        bitdepth_c_ = other.bitdepth_c_;
        crop_left_ = other.crop_left_;
        crop_right_ = other.crop_right_;
        crop_top_ = other.crop_top_;
        crop_bottom_ = other.crop_bottom_;
        pts = other.pts;
        poc = other.poc;
    }

    void* payload_ = nullptr;
    void (*free_payload_)(void*) = nullptr;
    std::vector<uint8_t> buffer_;  // populated only by alloc()
};

}  // namespace player::decode
