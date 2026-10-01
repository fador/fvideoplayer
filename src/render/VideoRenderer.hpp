#pragma once

#include <kvzdec.h>

#include <cstdint>
#include <string>
#include <vector>

namespace player::render {

struct VideoColorSettings {
    bool bt709 = true;
    bool full_range = false;
};

// Uploads decoded YUV planes into OpenGL textures and renders them through a
// YUV -> RGB shader into an offscreen texture that the UI displays.
class VideoRenderer {
public:
    bool init(std::string* error);
    void shutdown();

    // Bit depth parsed from the stream's hvcC, used when kvzdec does not
    // report one on the decoded picture.
    void setStreamBitDepth(int bitdepth) { stream_bitdepth_ = bitdepth; }

    void upload(const kvzdec_picture* picture);

    // Returns the FBO texture id, or 0 when no frame has been uploaded.
    unsigned int render(const VideoColorSettings& settings);

    int videoWidth() const { return cropped_width_; }
    int videoHeight() const { return cropped_height_; }
    bool hasFrame() const { return cropped_width_ > 0; }

    // Reads the rendered FBO back into a top-down RGB24 buffer (for tests).
    bool readback(std::vector<uint8_t>& rgb, int& width, int& height);

private:
    void ensureTextures(const kvzdec_picture* picture);
    void ensureFramebuffer(int width, int height);
    void destroyTextures();
    void destroyFramebuffer();

    unsigned int program_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;

    unsigned int textures_[3] = {0, 0, 0};
    int texture_width_[3] = {0, 0, 0};
    int texture_height_[3] = {0, 0, 0};
    int texture_bits_[3] = {0, 0, 0};
    bool texture_allocated_[3] = {false, false, false};
    std::vector<uint8_t> staging_[3];

    unsigned int framebuffer_ = 0;
    unsigned int framebuffer_texture_ = 0;
    int framebuffer_width_ = 0;
    int framebuffer_height_ = 0;

    int cropped_width_ = 0;
    int cropped_height_ = 0;
    float crop_u0_ = 0.0f;
    float crop_v0_ = 0.0f;
    float crop_u1_ = 1.0f;
    float crop_v1_ = 1.0f;
    int stream_bitdepth_ = 0;
    int luma_bitdepth_ = 8;
    int chroma_bitdepth_ = 8;
    float luma_scale_ = 1.0f;
    float chroma_scale_ = 1.0f;
    bool has_chroma_ = true;

    int uniform_y_ = -1;
    int uniform_u_ = -1;
    int uniform_v_ = -1;
    int uniform_matrix_ = -1;
    int uniform_offset_ = -1;
    int uniform_scale_ = -1;
    int uniform_scale_c_ = -1;
    int uniform_has_chroma_ = -1;
    int uniform_crop_ = -1;
};

}  // namespace player::render
