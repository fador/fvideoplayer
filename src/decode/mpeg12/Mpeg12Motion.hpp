#pragma once

#include "decode/Frame.hpp"

namespace player::decode::mpeg12 {

// A view of one plane as a grid of lines. A frame picture uses the plane
// itself; a field uses one parity of the plane with twice the line spacing, so
// motion compensation can work on either without special cases.
struct PlaneGrid {
    uint8_t* data = nullptr;
    int stride = 0;  // bytes between consecutive grid lines
    int width = 0;   // grid width in samples
    int height = 0;  // grid height in samples
};

// The full frame view of `plane` for a non-const frame.
PlaneGrid frameGrid(Frame& frame, int plane);
// The field view of `plane`: field 0 is the top field, 1 the bottom.
PlaneGrid fieldGrid(Frame& frame, int plane, int field_select);
// Read-only views for reference pictures.
PlaneGrid frameGrid(const Frame& frame, int plane);
PlaneGrid fieldGrid(const Frame& frame, int plane, int field_select);

// MPEG-1 and MPEG-2 have full-sample and half-sample prediction only, and the
// half-sample average always rounds up. Vectors are in half-sample units.
//
// A vector that leaves the reference is not padded: both standards forbid it,
// so such a block is not predicted and false is returned.
bool predictBlock(const PlaneGrid& dst, const PlaneGrid& src, int x, int y, int width, int height,
                  int mv_x, int mv_y);

// Copies `width` x `height` samples between grids, used to stash a prediction
// before averaging it with a second one. The two grids can have different
// strides and origins, which is what lets a macroblock be stashed into a
// macroblock-sized scratch grid at 0,0.
void copyBlock(const PlaneGrid& dst, int dst_x, int dst_y, const PlaneGrid& src, int src_x,
               int src_y, int width, int height);

// Averages `src` into `dst`, rounding up, with independent origins.
void averageBlock(const PlaneGrid& dst, int dst_x, int dst_y, const PlaneGrid& src, int src_x,
                  int src_y, int width, int height);

}  // namespace player::decode::mpeg12
