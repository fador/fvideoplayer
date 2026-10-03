#include "decode/mpeg12/Mpeg12Motion.hpp"

#include <cstring>

namespace player::decode::mpeg12 {
namespace {

// MPEG halves always round up; neither standard has a rounder control bit.
inline int average(int a, int b) {
    return (a + b + 1) >> 1;
}

// Clamps a source coordinate to the picture, which is how both standards
// extend the reference past its edges.
inline int clampCoord(int value, int limit) {
    if (value < 0) {
        return 0;
    }
    return value < limit ? value : limit - 1;
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

    // A vector may point outside the reference picture: both standards say the
    // reference is treated as if it were extended by repeating the edge
    // samples, so the coordinates are clamped rather than rejected.
    uint8_t* target = dst.data + static_cast<size_t>(y) * dst.stride + x;
    for (int row = 0; row < height; ++row) {
        const int sy = clampCoord(src_y + row, src.height);
        const int sy_next = frac_y ? clampCoord(src_y + row + 1, src.height) : sy;
        const uint8_t* top = src.data + static_cast<size_t>(sy) * src.stride;
        const uint8_t* bottom = src.data + static_cast<size_t>(sy_next) * src.stride;
        uint8_t* out = target + static_cast<size_t>(row) * dst.stride;
        for (int i = 0; i < width; ++i) {
            const int sx = clampCoord(src_x + i, src.width);
            const int sx_next = frac_x ? clampCoord(src_x + i + 1, src.width) : sx;
            int value;
            if (frac_x && frac_y) {
                const int t = average(top[sx], top[sx_next]);
                const int b = average(bottom[sx], bottom[sx_next]);
                value = average(t, b);
            } else if (frac_x) {
                value = average(top[sx], top[sx_next]);
            } else if (frac_y) {
                value = average(top[sx], bottom[sx]);
            } else {
                value = top[sx];
            }
            out[i] = static_cast<uint8_t>(value);
        }
    }
    return true;
}

void copyBlock(const PlaneGrid& dst, int dst_x, int dst_y, const PlaneGrid& src, int src_x,
               int src_y, int width, int height) {
    if (dst.data == nullptr || src.data == nullptr) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst.data + static_cast<size_t>(dst_y + row) * dst.stride + dst_x,
                    src.data + static_cast<size_t>(src_y + row) * src.stride + src_x,
                    static_cast<size_t>(width));
    }
}

void averageBlock(const PlaneGrid& dst, int dst_x, int dst_y, const PlaneGrid& src, int src_x,
                  int src_y, int width, int height) {
    if (dst.data == nullptr || src.data == nullptr) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint8_t* d = dst.data + static_cast<size_t>(dst_y + row) * dst.stride + dst_x;
        const uint8_t* s = src.data + static_cast<size_t>(src_y + row) * src.stride + src_x;
        for (int i = 0; i < width; ++i) {
            d[i] = static_cast<uint8_t>(average(d[i], s[i]));
        }
    }
}

}  // namespace player::decode::mpeg12
