#pragma once

#include <SDL3/SDL_opengl.h>

#include <cstddef>
#include <string>

// The GL header shipped with SDL only covers OpenGL 1.x, so the subset of
// OpenGL 2/3 entry points used by the renderer is loaded manually.

#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_R16
#define GL_R16 0x822A
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

typedef ptrdiff_t GLsizeiptr;

namespace player::render {

using PFN_glCreateShader = GLuint(APIENTRYP)(GLenum);
using PFN_glShaderSource = void(APIENTRYP)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader = void(APIENTRYP)(GLuint);
using PFN_glGetShaderiv = void(APIENTRYP)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog = void(APIENTRYP)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader = void(APIENTRYP)(GLuint);
using PFN_glCreateProgram = GLuint(APIENTRYP)();
using PFN_glAttachShader = void(APIENTRYP)(GLuint, GLuint);
using PFN_glLinkProgram = void(APIENTRYP)(GLuint);
using PFN_glGetProgramiv = void(APIENTRYP)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog = void(APIENTRYP)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteProgram = void(APIENTRYP)(GLuint);
using PFN_glUseProgram = void(APIENTRYP)(GLuint);
using PFN_glGetUniformLocation = GLint(APIENTRYP)(GLuint, const GLchar*);
using PFN_glUniform1i = void(APIENTRYP)(GLint, GLint);
using PFN_glUniform1f = void(APIENTRYP)(GLint, GLfloat);
using PFN_glUniform3fv = void(APIENTRYP)(GLint, GLsizei, const GLfloat*);
using PFN_glUniform4fv = void(APIENTRYP)(GLint, GLsizei, const GLfloat*);
using PFN_glUniformMatrix3fv = void(APIENTRYP)(GLint, GLsizei, GLboolean, const GLfloat*);
using PFN_glGenVertexArrays = void(APIENTRYP)(GLsizei, GLuint*);
using PFN_glBindVertexArray = void(APIENTRYP)(GLuint);
using PFN_glDeleteVertexArrays = void(APIENTRYP)(GLsizei, const GLuint*);
using PFN_glGenBuffers = void(APIENTRYP)(GLsizei, GLuint*);
using PFN_glBindBuffer = void(APIENTRYP)(GLenum, GLuint);
using PFN_glBufferData = void(APIENTRYP)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glDeleteBuffers = void(APIENTRYP)(GLsizei, const GLuint*);
using PFN_glEnableVertexAttribArray = void(APIENTRYP)(GLuint);
using PFN_glVertexAttribPointer =
    void(APIENTRYP)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glActiveTexture = void(APIENTRYP)(GLenum);
using PFN_glGenFramebuffers = void(APIENTRYP)(GLsizei, GLuint*);
using PFN_glBindFramebuffer = void(APIENTRYP)(GLenum, GLuint);
using PFN_glFramebufferTexture2D = void(APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStatus = GLenum(APIENTRYP)(GLenum);
using PFN_glDeleteFramebuffers = void(APIENTRYP)(GLsizei, const GLuint*);

struct GLFunctions {
    PFN_glCreateShader CreateShader = nullptr;
    PFN_glShaderSource ShaderSource = nullptr;
    PFN_glCompileShader CompileShader = nullptr;
    PFN_glGetShaderiv GetShaderiv = nullptr;
    PFN_glGetShaderInfoLog GetShaderInfoLog = nullptr;
    PFN_glDeleteShader DeleteShader = nullptr;
    PFN_glCreateProgram CreateProgram = nullptr;
    PFN_glAttachShader AttachShader = nullptr;
    PFN_glLinkProgram LinkProgram = nullptr;
    PFN_glGetProgramiv GetProgramiv = nullptr;
    PFN_glGetProgramInfoLog GetProgramInfoLog = nullptr;
    PFN_glDeleteProgram DeleteProgram = nullptr;
    PFN_glUseProgram UseProgram = nullptr;
    PFN_glGetUniformLocation GetUniformLocation = nullptr;
    PFN_glUniform1i Uniform1i = nullptr;
    PFN_glUniform1f Uniform1f = nullptr;
    PFN_glUniform3fv Uniform3fv = nullptr;
    PFN_glUniform4fv Uniform4fv = nullptr;
    PFN_glUniformMatrix3fv UniformMatrix3fv = nullptr;
    PFN_glGenVertexArrays GenVertexArrays = nullptr;
    PFN_glBindVertexArray BindVertexArray = nullptr;
    PFN_glDeleteVertexArrays DeleteVertexArrays = nullptr;
    PFN_glGenBuffers GenBuffers = nullptr;
    PFN_glBindBuffer BindBuffer = nullptr;
    PFN_glBufferData BufferData = nullptr;
    PFN_glDeleteBuffers DeleteBuffers = nullptr;
    PFN_glEnableVertexAttribArray EnableVertexAttribArray = nullptr;
    PFN_glVertexAttribPointer VertexAttribPointer = nullptr;
    PFN_glActiveTexture ActiveTexture = nullptr;
    PFN_glGenFramebuffers GenFramebuffers = nullptr;
    PFN_glBindFramebuffer BindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D FramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus = nullptr;
    PFN_glDeleteFramebuffers DeleteFramebuffers = nullptr;

    bool load(std::string* error);
};

extern GLFunctions gl;

}  // namespace player::render
