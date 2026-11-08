// gl.h - minimal OpenGL 3.3 function loader (no dependency on GL headers)
#pragma once

#include <cstddef>
#include <string>
#include <vector>

// ---- GL types -------------------------------------------------------------
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef unsigned char GLubyte;
typedef short GLshort;
typedef unsigned short GLushort;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef char GLchar;
typedef long long GLintptr;
typedef long long GLsizeiptr;
typedef void GLvoid;

// ---- GL constants ---------------------------------------------------------
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ZERO 0
#define GL_ONE 1
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_FUNC_ADD 0x8006
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901
#define GL_MIRRORED_REPEAT 0x8370
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_STATIC_DRAW 0x88E4
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_READ_ONLY 0x88B8
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_STREAM_READ 0x88E1
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_TEST 0x0C11
#define GL_SCISSOR_BOX 0x0C10
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_FRAGMENT_SHADER_DERIVATIVE_HINT 0x8B8B

#ifndef GL_APIENTRY
#define GL_APIENTRY
#endif

// ---- GL function table ----------------------------------------------------
#define GL_FUNC_LIST(X)                                                                  \
    X(const GLubyte*, glGetString, (GLenum name))                                         \
    X(GLenum, glGetError, (void))                                                         \
    X(void, glGetIntegerv, (GLenum pname, GLint* params))                                 \
    X(void, glGetFloatv, (GLenum pname, GLfloat* params))                                 \
    X(void, glEnable, (GLenum cap))                                                       \
    X(void, glDisable, (GLenum cap))                                                      \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor))                                \
    X(void, glBlendFuncSeparate, (GLenum sf, GLenum df, GLenum sfa, GLenum dfa))          \
    X(void, glBlendEquation, (GLenum mode))                                               \
    X(void, glColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))            \
    X(void, glViewport, (GLint x, GLint y, GLsizei w, GLsizei h))                         \
    X(void, glScissor, (GLint x, GLint y, GLsizei w, GLsizei h))                          \
    X(void, glClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                   \
    X(void, glClear, (GLbitfield mask))                                                   \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* data)) \
    X(void, glPixelStorei, (GLenum pname, GLint param))                                   \
    X(void, glFinish, (void))                                                             \
    X(void, glFlush, (void))                                                              \
    X(void, glDepthMask, (GLboolean flag))                                                \
    X(void, glGenTextures, (GLsizei n, GLuint* textures))                                 \
    X(void, glBindTexture, (GLenum target, GLuint texture))                               \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalfmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* data)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* data)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param))                  \
    X(void, glGenerateMipmap, (GLenum target))                                            \
    X(void, glDeleteTextures, (GLsizei n, const GLuint* textures))                        \
    X(void, glActiveTexture, (GLenum texture))                                            \
    X(void, glGenFramebuffers, (GLsizei n, GLuint* framebuffers))                         \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer))                       \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target))                                  \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))                \
    X(void, glBlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)) \
    X(void, glDrawBuffers, (GLsizei n, const GLenum* bufs))                               \
    X(GLuint, glCreateShader, (GLenum type))                                              \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)) \
    X(void, glCompileShader, (GLuint shader))                                             \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params))                  \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteShader, (GLuint shader))                                              \
    X(GLuint, glCreateProgram, (void))                                                    \
    X(void, glAttachShader, (GLuint program, GLuint shader))                              \
    X(void, glLinkProgram, (GLuint program))                                              \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params))                \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteProgram, (GLuint program))                                            \
    X(void, glUseProgram, (GLuint program))                                               \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name))                  \
    X(void, glUniform1i, (GLint location, GLint v0))                                      \
    X(void, glUniform1f, (GLint location, GLfloat v0))                                    \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1))                        \
    X(void, glUniform2i, (GLint location, GLint v0, GLint v1))                            \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2))            \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3))\
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat* value))           \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void, glGenBuffers, (GLsizei n, GLuint* buffers))                                   \
    X(void, glBindBuffer, (GLenum target, GLuint buffer))                                 \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void, glBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers))                          \
    X(void*, glMapBuffer, (GLenum target, GLenum access))                                 \
    X(void, glGetBufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, void* data)) \
    X(GLboolean, glUnmapBuffer, (GLenum target))                                          \
    X(void, glGenVertexArrays, (GLsizei n, GLuint* arrays))                               \
    X(void, glBindVertexArray, (GLuint array))                                            \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays))                      \
    X(void, glEnableVertexAttribArray, (GLuint index))                                    \
    X(void, glDisableVertexAttribArray, (GLuint index))                                   \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)) \
    X(void, glVertexAttribDivisor, (GLuint index, GLuint divisor))                        \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count))                      \
    X(void, glDrawArraysInstanced, (GLenum mode, GLint first, GLsizei count, GLsizei instancecount)) \
    X(void, glDrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))

#define GL_DECL(ret, name, params) typedef ret(GL_APIENTRY* PFN_##name) params; extern PFN_##name name;
GL_FUNC_LIST(GL_DECL)
#undef GL_DECL

namespace gl {
    struct ContextInfo {
        int width = 0;
        int height = 0;
        std::string vendor;
        std::string renderer;
        std::string version;
        std::string glsl;
    };

    // Creates a hidden-window OpenGL 3.3 core context (works headless on Windows).
    bool createContext(int width, int height, ContextInfo* info, std::string* error);
    void destroyContext();

    bool loadFunctions(std::vector<std::string>* missing);

    const ContextInfo& info();

    bool checkError(const char* where);
} // namespace gl
