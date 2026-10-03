#include "decode/mpeg12/Mpeg12Motion.hpp"

#include <cstring>

namespace player::decode::mpeg12 {
namespace {

// MPEG halves always round up; neither standard has a rounder control bit.
inline int average(int a, int b) {
    return (a + b + 1) >> 1;
}

bool sourceFits(int src_x, int src_y, int width, int height, int frac_x, int frac_y,
                int ref_width, int ref_height) {
    return src_x >= 0 && src_y >= 0 && src_x + width + frac_x <= ref_width &&
           src_y + height + frac_y <= ref_height;
}

// Half-sample refinement of one grid row.
void refineRow(uint8_t* dst, const uint8_t* src, int src_stride, int length, bool horizontal,
               bool vertical) {
    if (!horizontal && !vertical) {
        std::memcpy(dst, src, static_cast<size_t>(length));
        return;
    }
    if (horizontal && !vertical) {
        for (int i = 0; i < length; ++i) {
            dst[i] = static_cast<uint8_t>(average(src[i], src[i + 1]));
        }
        return;
    }
    if (!horizontal && vertical) {
        for (int i = 0; i < length; ++i) {
            dst[i] = static_cast<uint8_t>(average(src[i], src[i + src_stride]));
        }
        return;
    }
    // Both components are fractional: average horizontally, then vertically.
    for (int i = 0; i < length; ++i) {
        const uint8_t* bottom = src + src_stride;
        const int top = average(src[i], src[i + 1]);
        const int low = average(bottom[i], bottom[i + 1]);
        dst[i] = static_cast<uint8_t>(average(top, low));
    }
}

}  // namespace

PlaneGrid frameGrid(Frame& frame, int plane) {
    PlaneGrid grid;
    grid.data = frame.plane(plane);
    grid.stride = plane == 0 ? frame.stride_ : frame.stride_c_;
    grid.width = plane == 0 ? frame.width_ : frame.width_c_;
    grid.height = plane == 0 ? frame.height_ : frame.height_c_;
    return grid;
}

PlaneGrid fieldGrid(Frame& frame, int plane, int field_select) {
    PlaneGrid grid = frameGrid(frame, plane);
    grid.data += static_cast<size_t>(field_select & 1) * grid.stride;
    grid.stride *= 2;
    grid.height /= 2;
    return grid;
}

PlaneGrid frameGrid(const Frame& frame, int plane) {
    PlaneGrid grid;
    grid.data = frame.plane(plane);
    grid.stride = plane == 0 ? frame.stride_ : frame.stride_c_;
    grid.width = plane == 0 ? frame.width_ : frame.width_c_;
    grid.height = plane == 0 ? frame.height_ : frame.height_c_;
    return grid;
}

PlaneGrid fieldGrid(const Frame& frame, int plane, int field_select) {
    PlaneGrid grid = frameGrid(frame, plane);
    grid.data += static_cast<size_t>(field_select & 1) * grid.stride;
    grid.stride *= 2;
    grid.height /= 2;
    return grid;
}

bool predictBlock(const PlaneGrid& dst, const PlaneGrid& src, int x, int y, int width, int height,
                  int mv_x, int mv_y) {
    if (dst.data == nullptr || src.data == nullptr) {
        return false;
    }
    const int src_x = x + (mv_x >> 1);
    const int src_y = y + (mv_y >> 1);
    const int frac_x = mv_x & 1;
    const int frac_y = mv_y & 1;

    if (!sourceFits(src_x, src_y, width, height, frac_x, frac_y, src.width, src.height)) {
        return false;
    }

    const uint8_t* source = src.data + static_cast<size_t>(src_y) * src.stride + src_x;
    uint8_t* target = dst.data + static_cast<size_t>(y) * dst.stride + x;
    for (int row = 0; row < height; ++row) {
        refineRow(target + static_cast<size_t>(row) * dst.stride, source, src.stride, width,
                  frac_x != 0, frac_y != 0);
        source += src.stride;
    }
    return true;
}

void copyBlock(const PlaneGrid& dst, const PlaneGrid& src, int x, int y, int width, int height) {
    if (dst.data == nullptr || src.data == nullptr) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst.data + static_cast<size_t>(y + row) * dst.stride + x,
                    src.data + static_cast<size_t>(y + row) * src.stride + x,
                    static_cast<size_t>(width));
    }
}

void averageBlock(const PlaneGrid& dst, const PlaneGrid& src, int x, int y, int width, int height) {
    if (dst.data == nullptr || src.data == nullptr) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint8_t* d = dst.data + static_cast<size_t>(y + row) * dst.stride + x;
        const uint8_t* s = src.data + static_cast<size_t>(y + row) * src.stride + x;
        for (int i = 0; i < width; ++i) {
            d[i] = static_cast<uint8_t>(average(d[i], s[i]));
        }
    }
}

}  // namespace player::decode::mpeg12
