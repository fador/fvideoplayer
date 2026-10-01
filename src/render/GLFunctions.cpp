#include "render/GLFunctions.hpp"

#include <SDL3/SDL.h>

namespace player::render {

GLFunctions gl;

bool GLFunctions::load(std::string* error) {
    const auto resolve = [error](const char* name) -> void* {
        void* pointer = SDL_GL_GetProcAddress(name);
        if (!pointer && error) {
            *error = std::string("missing OpenGL function: ") + name;
        }
        return pointer;
    };

#define LOAD(field, name) field = reinterpret_cast<decltype(field)>(resolve(name))

    LOAD(CreateShader, "glCreateShader");
    LOAD(ShaderSource, "glShaderSource");
    LOAD(CompileShader, "glCompileShader");
    LOAD(GetShaderiv, "glGetShaderiv");
    LOAD(GetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(DeleteShader, "glDeleteShader");
    LOAD(CreateProgram, "glCreateProgram");
    LOAD(AttachShader, "glAttachShader");
    LOAD(LinkProgram, "glLinkProgram");
    LOAD(GetProgramiv, "glGetProgramiv");
    LOAD(GetProgramInfoLog, "glGetProgramInfoLog");
    LOAD(DeleteProgram, "glDeleteProgram");
    LOAD(UseProgram, "glUseProgram");
    LOAD(GetUniformLocation, "glGetUniformLocation");
    LOAD(Uniform1i, "glUniform1i");
    LOAD(Uniform1f, "glUniform1f");
    LOAD(Uniform3fv, "glUniform3fv");
    LOAD(Uniform4fv, "glUniform4fv");
    LOAD(UniformMatrix3fv, "glUniformMatrix3fv");
    LOAD(GenVertexArrays, "glGenVertexArrays");
    LOAD(BindVertexArray, "glBindVertexArray");
    LOAD(DeleteVertexArrays, "glDeleteVertexArrays");
    LOAD(GenBuffers, "glGenBuffers");
    LOAD(BindBuffer, "glBindBuffer");
    LOAD(BufferData, "glBufferData");
    LOAD(DeleteBuffers, "glDeleteBuffers");
    LOAD(EnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(VertexAttribPointer, "glVertexAttribPointer");
    LOAD(ActiveTexture, "glActiveTexture");
    LOAD(GenFramebuffers, "glGenFramebuffers");
    LOAD(BindFramebuffer, "glBindFramebuffer");
    LOAD(FramebufferTexture2D, "glFramebufferTexture2D");
    LOAD(CheckFramebufferStatus, "glCheckFramebufferStatus");
    LOAD(DeleteFramebuffers, "glDeleteFramebuffers");

#undef LOAD

    return !CreateShader || !ShaderSource || !CompileShader || !GetShaderiv ||
           !GetShaderInfoLog || !DeleteShader || !CreateProgram || !AttachShader ||
           !LinkProgram || !GetProgramiv || !GetProgramInfoLog || !DeleteProgram ||
           !UseProgram || !GetUniformLocation || !Uniform1i || !Uniform1f || !Uniform3fv ||
           !Uniform4fv || !UniformMatrix3fv || !GenVertexArrays || !BindVertexArray ||
           !DeleteVertexArrays ||
           !GenBuffers || !BindBuffer || !BufferData || !DeleteBuffers ||
           !EnableVertexAttribArray || !VertexAttribPointer || !ActiveTexture ||
           !GenFramebuffers || !BindFramebuffer || !FramebufferTexture2D ||
           !CheckFramebufferStatus || !DeleteFramebuffers
               ? false
               : true;
}

}  // namespace player::render
