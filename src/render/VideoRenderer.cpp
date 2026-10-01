#include "render/VideoRenderer.hpp"

#include "render/GLFunctions.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace player::render {
namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec2 v_uv;
out vec4 frag_color;
uniform sampler2D u_y;
uniform sampler2D u_u;
uniform sampler2D u_v;
uniform mat3 u_matrix;
uniform vec3 u_offset;
uniform float u_scale;
uniform int u_has_chroma;
uniform vec4 u_crop;  // u0, v0, u1, v1
void main() {
    vec2 uv = vec2(mix(u_crop.x, u_crop.z, v_uv.x), mix(u_crop.y, u_crop.w, v_uv.y));
    float y = texture(u_y, uv).r * u_scale;
    vec3 yuv = vec3(y, 0.5, 0.5);
    if (u_has_chroma == 1) {
        yuv.y = texture(u_u, uv).r * u_scale;
        yuv.z = texture(u_v, uv).r * u_scale;
    }
    vec3 rgb = u_matrix * yuv + u_offset;
    frag_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

bool compileShader(GLenum type, const char* source, GLuint& shader, std::string* error) {
    shader = gl.CreateShader(type);
    if (shader == 0) {
        if (error) {
            *error = "glCreateShader failed";
        }
        return false;
    }
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint status = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == 0) {
        char log[1024] = {};
        gl.GetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        if (error) {
            *error = std::string("shader compile failed: ") + log;
        }
        gl.DeleteShader(shader);
        shader = 0;
        return false;
    }
    return true;
}

void buildColorMatrix(bool bt709, bool full_range, float matrix[9], float offset[3]) {
    const float kr = bt709 ? 0.2126f : 0.299f;
    const float kb = bt709 ? 0.0722f : 0.114f;
    const float kg = 1.0f - kr - kb;
    const float y_scale = full_range ? 1.0f : 255.0f / 219.0f;
    const float c_scale = full_range ? 1.0f : 255.0f / 224.0f;
    const float y_offset = full_range ? 0.0f : 16.0f / 255.0f;
    const float c_offset = 0.5f;

    matrix[0] = y_scale;
    matrix[1] = 0.0f;
    matrix[2] = 2.0f * (1.0f - kr) * c_scale;
    matrix[3] = y_scale;
    matrix[4] = -2.0f * kb * (1.0f - kb) / kg * c_scale;
    matrix[5] = -2.0f * kr * (1.0f - kr) / kg * c_scale;
    matrix[6] = y_scale;
    matrix[7] = 2.0f * (1.0f - kb) * c_scale;
    matrix[8] = 0.0f;

    for (int row = 0; row < 3; ++row) {
        offset[row] = -(matrix[row * 3 + 0] * y_offset + matrix[row * 3 + 1] * c_offset +
                        matrix[row * 3 + 2] * c_offset);
    }
}

}  // namespace

bool VideoRenderer::init(std::string* error) {
    if (!gl.load(error)) {
        return false;
    }

    GLuint vertex = 0;
    GLuint fragment = 0;
    if (!compileShader(GL_VERTEX_SHADER, kVertexShader, vertex, error)) {
        return false;
    }
    if (!compileShader(GL_FRAGMENT_SHADER, kFragmentShader, fragment, error)) {
        gl.DeleteShader(vertex);
        return false;
    }

    program_ = gl.CreateProgram();
    gl.AttachShader(program_, vertex);
    gl.AttachShader(program_, fragment);
    gl.LinkProgram(program_);
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);

    GLint status = 0;
    gl.GetProgramiv(program_, GL_LINK_STATUS, &status);
    if (status == 0) {
        char log[1024] = {};
        gl.GetProgramInfoLog(program_, sizeof(log) - 1, nullptr, log);
        if (error) {
            *error = std::string("shader link failed: ") + log;
        }
        gl.DeleteProgram(program_);
        program_ = 0;
        return false;
    }

    uniform_y_ = gl.GetUniformLocation(program_, "u_y");
    uniform_u_ = gl.GetUniformLocation(program_, "u_u");
    uniform_v_ = gl.GetUniformLocation(program_, "u_v");
    uniform_matrix_ = gl.GetUniformLocation(program_, "u_matrix");
    uniform_offset_ = gl.GetUniformLocation(program_, "u_offset");
    uniform_scale_ = gl.GetUniformLocation(program_, "u_scale");
    uniform_has_chroma_ = gl.GetUniformLocation(program_, "u_has_chroma");
    uniform_crop_ = gl.GetUniformLocation(program_, "u_crop");

    const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 1.0f,
    };
    gl.GenVertexArrays(1, &vao_);
    gl.BindVertexArray(vao_);
    gl.GenBuffers(1, &vbo_);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           reinterpret_cast<const void*>(2 * sizeof(float)));
    gl.BindVertexArray(0);
    return true;
}

void VideoRenderer::shutdown() {
    destroyTextures();
    destroyFramebuffer();
    if (vbo_) {
        gl.DeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        gl.DeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_) {
        gl.DeleteProgram(program_);
        program_ = 0;
    }
}

void VideoRenderer::ensureTextures(const kvzdec_picture* picture) {
    const int plane_count = picture->chroma_format == KVZDEC_CSP_400 ? 1 : 3;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    for (int plane = 0; plane < plane_count; ++plane) {
        const bool chroma = plane > 0;
        const int width = chroma ? picture->width_c : picture->width;
        const int height = chroma ? picture->height_c : picture->height;
        const int stride = chroma ? picture->stride_c : picture->stride;
        const int bits = picture->pixel_size == 2 ? 16 : 8;
        const uint8_t* data = static_cast<const uint8_t*>(picture->data[plane]);

        // Repack into tightly packed rows so the upload does not depend on
        // GL_UNPACK_ROW_LENGTH handling.
        const size_t row_bytes = static_cast<size_t>(width) * picture->pixel_size;
        staging_[plane].resize(row_bytes * static_cast<size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(staging_[plane].data() + static_cast<size_t>(y) * row_bytes,
                        data + static_cast<size_t>(y) * stride * picture->pixel_size, row_bytes);
        }
        const void* packed = staging_[plane].data();

        if (textures_[plane] == 0) {
            glGenTextures(1, &textures_[plane]);
        }
        glBindTexture(GL_TEXTURE_2D, textures_[plane]);
        if (!texture_allocated_[plane] || texture_width_[plane] != width ||
            texture_height_[plane] != height || texture_bits_[plane] != bits) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, bits == 16 ? GL_R16 : GL_R8, width, height, 0,
                         GL_RED, bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
            texture_width_[plane] = width;
            texture_height_[plane] = height;
            texture_bits_[plane] = bits;
            texture_allocated_[plane] = true;
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED,
                        bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, packed);
    }
}

void VideoRenderer::ensureFramebuffer(int width, int height) {
    if (framebuffer_ != 0 && framebuffer_width_ == width && framebuffer_height_ == height) {
        return;
    }
    destroyFramebuffer();

    glGenTextures(1, &framebuffer_texture_);
    glBindTexture(GL_TEXTURE_2D, framebuffer_texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);

    gl.GenFramebuffers(1, &framebuffer_);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           framebuffer_texture_, 0);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    framebuffer_width_ = width;
    framebuffer_height_ = height;
}

void VideoRenderer::destroyTextures() {
    for (int i = 0; i < 3; ++i) {
        if (textures_[i] != 0) {
            glDeleteTextures(1, &textures_[i]);
            textures_[i] = 0;
        }
        texture_allocated_[i] = false;
        texture_width_[i] = 0;
        texture_height_[i] = 0;
        texture_bits_[i] = 0;
    }
}

void VideoRenderer::destroyFramebuffer() {
    if (framebuffer_ != 0) {
        gl.DeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (framebuffer_texture_ != 0) {
        glDeleteTextures(1, &framebuffer_texture_);
        framebuffer_texture_ = 0;
    }
    framebuffer_width_ = 0;
    framebuffer_height_ = 0;
}

void VideoRenderer::upload(const kvzdec_picture* picture) {
    if (!picture) {
        return;
    }
    const int width = picture->width;
    const int height = picture->height;
    if (width <= 0 || height <= 0) {
        return;
    }

    cropped_width_ = width - picture->crop_left - picture->crop_right;
    cropped_height_ = height - picture->crop_top - picture->crop_bottom;
    crop_u0_ = static_cast<float>(picture->crop_left) / static_cast<float>(width);
    crop_v0_ = static_cast<float>(picture->crop_top) / static_cast<float>(height);
    crop_u1_ = 1.0f - static_cast<float>(picture->crop_right) / static_cast<float>(width);
    crop_v1_ = 1.0f - static_cast<float>(picture->crop_bottom) / static_cast<float>(height);
    has_chroma_ = picture->chroma_format != KVZDEC_CSP_400;
    if (picture->pixel_size == 2 && picture->bitdepth > 0) {
        normalize_scale_ =
            65535.0f / static_cast<float>((1 << picture->bitdepth) - 1);
    } else {
        normalize_scale_ = 1.0f;
    }

    ensureTextures(picture);
}

unsigned int VideoRenderer::render(const VideoColorSettings& settings) {
    if (!hasFrame() || program_ == 0) {
        return 0;
    }
    ensureFramebuffer(cropped_width_, cropped_height_);
    if (framebuffer_ == 0) {
        return 0;
    }

    float matrix[9] = {};
    float offset[3] = {};
    buildColorMatrix(settings.bt709, settings.full_range, matrix, offset);

    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, cropped_width_, cropped_height_);
    glDisable(GL_BLEND);

    gl.UseProgram(program_);
    gl.Uniform1i(uniform_y_, 0);
    gl.Uniform1i(uniform_u_, 1);
    gl.Uniform1i(uniform_v_, 2);
    gl.Uniform1i(uniform_has_chroma_, has_chroma_ ? 1 : 0);
    gl.Uniform1f(uniform_scale_, normalize_scale_);
    gl.UniformMatrix3fv(uniform_matrix_, 1, GL_TRUE, matrix);
    gl.Uniform3fv(uniform_offset_, 1, offset);
    const float crop[4] = {crop_u0_, crop_v0_, crop_u1_, crop_v1_};
    gl.Uniform4fv(uniform_crop_, 1, crop);

    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    gl.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, has_chroma_ ? textures_[1] : 0);
    gl.ActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, has_chroma_ ? textures_[2] : 0);
    gl.ActiveTexture(GL_TEXTURE0);

    gl.BindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl.BindVertexArray(0);
    gl.UseProgram(0);

    glEnable(GL_BLEND);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    return framebuffer_texture_;
}

bool VideoRenderer::readback(std::vector<uint8_t>& rgb, int& width, int& height) {
    if (!hasFrame() || framebuffer_ == 0) {
        return false;
    }
    width = cropped_width_;
    height = cropped_height_;
    rgb.resize(static_cast<size_t>(width) * height * 3);

    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

    // glReadPixels returns the framebuffer bottom row first. The FBO was
    // rendered with the picture's top row at the framebuffer bottom (so that
    // ImGui, which samples texture row 0 at the top, shows it upright), which
    // means the returned rows are already in top-down order.
    return true;
}

}  // namespace player::render
