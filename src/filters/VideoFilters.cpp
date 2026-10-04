#include "filters/VideoFilters.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace player::filters {
namespace {

int planeShiftX(decode::ChromaFormat format) {
    return format == decode::ChromaFormat::C444 ? 0 : 1;
}

int planeShiftY(decode::ChromaFormat format) {
    return format == decode::ChromaFormat::C420 ? 1 : 0;
}

template <typename T>
void scalePlaneBilinear(const T* src, int src_width, int src_height, int src_stride, T* dst,
                        int dst_width, int dst_height, int dst_stride, int max_value) {
    for (int y = 0; y < dst_height; ++y) {
        float fy = 0.0f;
        if (dst_height > 1 && src_height > 1) {
            fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src_height) /
                     static_cast<float>(dst_height) -
                 0.5f;
        }
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, src_height - 1);
        const int y1 = std::min(y0 + 1, src_height - 1);
        const float wy = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        T* out_row = dst + static_cast<size_t>(y) * dst_stride;
        const T* row0 = src + static_cast<size_t>(y0) * src_stride;
        const T* row1 = src + static_cast<size_t>(y1) * src_stride;
        for (int x = 0; x < dst_width; ++x) {
            float fx = 0.0f;
            if (dst_width > 1 && src_width > 1) {
                fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src_width) /
                         static_cast<float>(dst_width) -
                     0.5f;
            }
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, src_width - 1);
            const int x1 = std::min(x0 + 1, src_width - 1);
            const float wx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            const float top = static_cast<float>(row0[x0]) * (1.0f - wx) +
                              static_cast<float>(row0[x1]) * wx;
            const float bottom = static_cast<float>(row1[x0]) * (1.0f - wx) +
                                 static_cast<float>(row1[x1]) * wx;
            const float value = top * (1.0f - wy) + bottom * wy;
            out_row[x] = static_cast<T>(
                std::clamp(static_cast<long>(std::lround(value)), 0L, static_cast<long>(max_value)));
        }
    }
}

void scalePlane(const uint8_t* src, int pixel_size, int src_width, int src_height, int src_stride,
                uint8_t* dst, int dst_width, int dst_height, int dst_stride, int max_value) {
    if (pixel_size == 2) {
        scalePlaneBilinear<uint16_t>(reinterpret_cast<const uint16_t*>(src), src_width, src_height,
                                     src_stride, reinterpret_cast<uint16_t*>(dst), dst_width,
                                     dst_height, dst_stride, max_value);
    } else {
        scalePlaneBilinear<uint8_t>(src, src_width, src_height, src_stride, dst, dst_width,
                                    dst_height, dst_stride, max_value);
    }
}

template <typename T>
void flipPlane(T* plane, int width, int height, int stride, bool horizontal, bool vertical) {
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            std::reverse(plane + static_cast<size_t>(y) * stride,
                         plane + static_cast<size_t>(y) * stride + width);
        }
    }
    if (vertical) {
        for (int y = 0; y < height / 2; ++y) {
            T* top = plane + static_cast<size_t>(y) * stride;
            T* bottom = plane + static_cast<size_t>(height - 1 - y) * stride;
            for (int x = 0; x < width; ++x) {
                std::swap(top[x], bottom[x]);
            }
        }
    }
}

void fillPlane(uint8_t* plane, int pixel_size, int width, int height, int stride, int value) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (pixel_size == 2) {
        auto* samples = reinterpret_cast<uint16_t*>(plane);
        for (int y = 0; y < height; ++y) {
            uint16_t* row = samples + static_cast<size_t>(y) * stride;
            std::fill(row, row + width, static_cast<uint16_t>(value));
        }
    } else {
        for (int y = 0; y < height; ++y) {
            std::memset(plane + static_cast<size_t>(y) * stride, value, static_cast<size_t>(width));
        }
    }
}

bool makeOutputFrame(const decode::Frame& in, int width, int height, decode::Frame& out) {
    if (!out.alloc(width, height, in.format(), in.bitdepth_)) {
        return false;
    }
    out.bitdepth_c_ = in.bitdepth_c_;
    out.pts = in.pts;
    out.poc = in.poc;
    out.coding_type = in.coding_type;
    out.quantiser = in.quantiser;
    return true;
}

template <typename T>
void applyEquation(T* plane, int width, int height, int stride, float scale, float offset,
                   int max_value) {
    for (int y = 0; y < height; ++y) {
        T* row = plane + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const float value = static_cast<float>(row[x]) * scale + offset;
            row[x] = static_cast<T>(std::clamp(static_cast<long>(std::lround(value)), 0L,
                                               static_cast<long>(max_value)));
        }
    }
}

}  // namespace

bool ScaleFilter::configure(const VideoFormat& input, VideoFormat& output, std::string& error) {
    if (input.width <= 0 || input.height <= 0) {
        error = "input has no geometry";
        return false;
    }
    if (width_ == 0 || height_ == 0) {
        error = "width and height must be non-zero";
        return false;
    }
    if (width_ < 0 && height_ < 0) {
        error = "at least one of width and height must be fixed";
        return false;
    }
    int width = width_;
    int height = height_;
    if (width < 0) {
        width = static_cast<int>(
            std::lround(static_cast<double>(input.width) * height / input.height));
        if (width_ == -2) {
            width &= ~1;
        }
    }
    if (height < 0) {
        height = static_cast<int>(
            std::lround(static_cast<double>(input.height) * width / input.width));
        if (height_ == -2) {
            height &= ~1;
        }
    }
    if (width < 1 || height < 1) {
        error = "computed size is empty";
        return false;
    }
    out_width_ = width;
    out_height_ = height;
    output = input;
    output.width = width;
    output.height = height;
    return true;
}

bool ScaleFilter::process(decode::Frame& frame, std::string& error) {
    if (out_width_ <= 0 || out_height_ <= 0) {
        error = "not configured";
        return false;
    }
    if (out_width_ == frame.width_ && out_height_ == frame.height_) {
        return true;
    }
    decode::Frame out;
    if (!makeOutputFrame(frame, out_width_, out_height_, out)) {
        error = "cannot allocate output";
        return false;
    }
    const int planes = decode::chromaPlaneCount(frame.format());
    const int max_luma = (1 << frame.bitdepth_) - 1;
    const int max_chroma = (1 << frame.bitdepth_c_) - 1;
    for (int plane = 0; plane < planes; ++plane) {
        scalePlane(frame.plane(plane), frame.pixelSize(), frame.planeWidth(plane),
                   frame.planeHeight(plane), frame.planeStride(plane), out.plane(plane),
                   out.planeWidth(plane), out.planeHeight(plane), out.planeStride(plane),
                   plane == 0 ? max_luma : max_chroma);
    }
    frame = std::move(out);
    return true;
}

bool CropFilter::configure(const VideoFormat& input, VideoFormat& output, std::string& error) {
    if (input.width <= 0 || input.height <= 0) {
        error = "input has no geometry";
        return false;
    }
    int width = width_;
    int height = height_;
    if (width < 0) {
        width = input.width + width;
    }
    if (height < 0) {
        height = input.height + height;
    }
    if (width <= 0 || height <= 0 || width > input.width || height > input.height) {
        error = "crop rectangle is outside the frame";
        return false;
    }
    const bool centered_x = x_ < 0;
    const bool centered_y = y_ < 0;
    int x = x_;
    int y = y_;
    if (centered_x) {
        x = (input.width - width) / 2;
    }
    if (centered_y) {
        y = (input.height - height) / 2;
    }
    // A centered rectangle snaps to the chroma grid; an explicit odd offset is
    // an error, because half a chroma sample cannot be addressed.
    if (input.chroma == decode::ChromaFormat::C420 || input.chroma == decode::ChromaFormat::C422) {
        if (centered_x) {
            x &= ~1;
        }
        if (centered_y && input.chroma == decode::ChromaFormat::C420) {
            y &= ~1;
        }
    }
    if (x < 0 || y < 0 || x + width > input.width || y + height > input.height) {
        error = "crop rectangle is outside the frame";
        return false;
    }
    if (input.chroma != decode::ChromaFormat::Mono) {
        if (input.chroma != decode::ChromaFormat::C444 && (x % 2) != 0) {
            error = "x must be even for subsampled chroma";
            return false;
        }
        if (input.chroma == decode::ChromaFormat::C420 && (y % 2) != 0) {
            error = "y must be even for 4:2:0 chroma";
            return false;
        }
    }
    crop_x_ = x;
    crop_y_ = y;
    crop_w_ = width;
    crop_h_ = height;
    output = input;
    output.width = width;
    output.height = height;
    return true;
}

bool CropFilter::process(decode::Frame& frame, std::string& error) {
    if (crop_w_ <= 0 || crop_h_ <= 0) {
        error = "not configured";
        return false;
    }
    decode::Frame out;
    if (!makeOutputFrame(frame, crop_w_, crop_h_, out)) {
        error = "cannot allocate output";
        return false;
    }
    const int planes = decode::chromaPlaneCount(frame.format());
    const int shift_x = planeShiftX(frame.format());
    const int shift_y = planeShiftY(frame.format());
    const size_t pixel_size = static_cast<size_t>(frame.pixelSize());
    for (int plane = 0; plane < planes; ++plane) {
        const int src_x = plane == 0 ? crop_x_ : crop_x_ >> shift_x;
        const int src_y = plane == 0 ? crop_y_ : crop_y_ >> shift_y;
        const size_t row_bytes = static_cast<size_t>(frame.planeWidth(plane)) * pixel_size;
        for (int y = 0; y < out.planeHeight(plane); ++y) {
            std::memcpy(out.row(plane, y),
                        frame.row(plane, src_y + y) + static_cast<size_t>(src_x) * pixel_size,
                        row_bytes);
        }
    }
    frame = std::move(out);
    return true;
}

bool FlipFilter::configure(const VideoFormat& input, VideoFormat& output, std::string& error) {
    (void)error;
    output = input;
    return true;
}

bool FlipFilter::process(decode::Frame& frame, std::string& error) {
    (void)error;
    const int planes = decode::chromaPlaneCount(frame.format());
    for (int plane = 0; plane < planes; ++plane) {
        const int width = frame.planeWidth(plane);
        const int height = frame.planeHeight(plane);
        const int stride = frame.planeStride(plane);
        if (frame.pixelSize() == 2) {
            flipPlane<uint16_t>(reinterpret_cast<uint16_t*>(frame.plane(plane)), width, height,
                                stride, horizontal_, vertical_);
        } else {
            flipPlane<uint8_t>(frame.plane(plane), width, height, stride, horizontal_, vertical_);
        }
    }
    return true;
}

bool GrayscaleFilter::configure(const VideoFormat& input, VideoFormat& output, std::string& error) {
    (void)error;
    output = input;
    return true;
}

bool GrayscaleFilter::process(decode::Frame& frame, std::string& error) {
    (void)error;
    const int planes = decode::chromaPlaneCount(frame.format());
    const int neutral = 1 << (frame.bitdepth_c_ - 1);
    for (int plane = 1; plane < planes; ++plane) {
        fillPlane(frame.plane(plane), frame.pixelSize(), frame.planeWidth(plane),
                  frame.planeHeight(plane), frame.planeStride(plane), neutral);
    }
    return true;
}

bool EqFilter::configure(const VideoFormat& input, VideoFormat& output, std::string& error) {
    (void)error;
    output = input;
    return true;
}

bool EqFilter::process(decode::Frame& frame, std::string& error) {
    (void)error;
    const bool luma_changed = brightness_ != 0.0f || contrast_ != 1.0f;
    const bool chroma_changed = saturation_ != 1.0f;
    if (!luma_changed && !chroma_changed) {
        return true;
    }
    const int max_luma = (1 << frame.bitdepth_) - 1;
    const int max_chroma = (1 << frame.bitdepth_c_) - 1;
    const float luma_mid = static_cast<float>(1 << (frame.bitdepth_ - 1));
    const float chroma_mid = static_cast<float>(1 << (frame.bitdepth_c_ - 1));
    const float luma_scale = contrast_;
    const float luma_offset =
        luma_mid - luma_mid * contrast_ + brightness_ * static_cast<float>(max_luma);
    const int planes = decode::chromaPlaneCount(frame.format());
    for (int plane = 0; plane < planes; ++plane) {
        const bool luma = plane == 0;
        if (luma ? !luma_changed : !chroma_changed) {
            continue;
        }
        const float scale = luma ? luma_scale : saturation_;
        const float offset = luma ? luma_offset : chroma_mid - chroma_mid * saturation_;
        const int max_value = luma ? max_luma : max_chroma;
        const int width = frame.planeWidth(plane);
        const int height = frame.planeHeight(plane);
        const int stride = frame.planeStride(plane);
        if (frame.pixelSize() == 2) {
            applyEquation<uint16_t>(reinterpret_cast<uint16_t*>(frame.plane(plane)), width, height,
                                    stride, scale, offset, max_value);
        } else {
            applyEquation<uint8_t>(frame.plane(plane), width, height, stride, scale, offset,
                                   max_value);
        }
    }
    return true;
}

}  // namespace player::filters
