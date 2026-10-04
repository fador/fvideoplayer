#pragma once

#include "filters/Filter.hpp"

namespace player::filters {

// Bilinear resize. A width or height of -1 derives the value from the other
// dimension preserving the aspect ratio; -2 additionally rounds the derived
// value down to an even number, as ffmpeg's scale filter does.
class ScaleFilter : public IVideoFilter {
public:
    ScaleFilter(int width, int height) : width_(width), height_(height) {}

    const char* name() const override { return "scale"; }
    bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) override;
    bool process(decode::Frame& frame, std::string& error) override;

private:
    int width_ = 0;
    int height_ = 0;
    int out_width_ = 0;
    int out_height_ = 0;
};

// Crops to width x height at x,y. Negative x/y center the rectangle; negative
// width/height are taken from the input size (crop=-40:-40 centers a rect 40
// pixels inside the frame edges).
class CropFilter : public IVideoFilter {
public:
    CropFilter(int width, int height, int x, int y)
        : width_(width), height_(height), x_(x), y_(y) {}

    const char* name() const override { return "crop"; }
    bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) override;
    bool process(decode::Frame& frame, std::string& error) override;

private:
    int width_ = 0;
    int height_ = 0;
    int x_ = 0;
    int y_ = 0;
    int crop_x_ = 0;
    int crop_y_ = 0;
    int crop_w_ = 0;
    int crop_h_ = 0;
};

// Mirrors the picture horizontally and/or vertically.
class FlipFilter : public IVideoFilter {
public:
    FlipFilter(bool horizontal, bool vertical) : horizontal_(horizontal), vertical_(vertical) {}

    const char* name() const override { return "flip"; }
    bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) override;
    bool process(decode::Frame& frame, std::string& error) override;

private:
    bool horizontal_ = true;
    bool vertical_ = false;
};

// Replaces the chroma planes with neutral values, keeping luma.
class GrayscaleFilter : public IVideoFilter {
public:
    const char* name() const override { return "grayscale"; }
    bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) override;
    bool process(decode::Frame& frame, std::string& error) override;
};

// Brightness / contrast on luma, saturation on chroma. Values follow ffmpeg's
// eq filter: brightness in [-1, 1], contrast and saturation are multipliers.
class EqFilter : public IVideoFilter {
public:
    EqFilter(float brightness, float contrast, float saturation)
        : brightness_(brightness), contrast_(contrast), saturation_(saturation) {}

    const char* name() const override { return "eq"; }
    bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) override;
    bool process(decode::Frame& frame, std::string& error) override;

private:
    float brightness_ = 0.0f;
    float contrast_ = 1.0f;
    float saturation_ = 1.0f;
};

}  // namespace player::filters
