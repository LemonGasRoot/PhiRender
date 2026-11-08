// gpu.cpp - GL3.3 helper implementations
#include "gpu.h"

#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "stb_image.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace gpu {

    static std::string readText(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::string();
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // ------------------------------------------------------------------ Shader
    static GLuint compileStage(GLenum type, const std::string& src, const std::string& dbg,
                               std::string* error) {
        GLuint sh = glCreateShader(type);
        const char* p = src.c_str();
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[8192] = {0};
            glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
            if (error) *error = dbg + " compile error:\n" + log;
            std::fprintf(stderr, "[gpu] %s compile error:\n%s\n", dbg.c_str(), log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    bool Shader::build(const char* vs, const char* fs, const std::string& debugName,
                       std::string* error) {
        name = debugName;
        program = glCreateProgram();
        GLuint v = compileStage(GL_VERTEX_SHADER, vs, debugName + ".vert", error);
        if (!v) return false;
        GLuint f = compileStage(GL_FRAGMENT_SHADER, fs, debugName + ".frag", error);
        if (!f) {
            glDeleteShader(v);
            return false;
        }
        glAttachShader(program, v);
        glAttachShader(program, f);
        glLinkProgram(program);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[8192] = {0};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            if (error) *error = debugName + " link error:\n" + log;
            std::fprintf(stderr, "[gpu] %s link error:\n%s\n", debugName.c_str(), log);
            return false;
        }
        locations.clear();
        return true;
    }

    bool Shader::loadFile(const std::string& vsPath, const std::string& fsPath,
                          std::string* error) {
        std::string vs = readText(vsPath);
        std::string fs = readText(fsPath);
        if (vs.empty()) {
            if (error) *error = "cannot read " + vsPath;
            return false;
        }
        if (fs.empty()) {
            if (error) *error = "cannot read " + fsPath;
            return false;
        }
        return build(vs.c_str(), fs.c_str(), vsPath + "|" + fsPath, error);
    }

    void Shader::destroy() {
        if (program) glDeleteProgram(program);
        program = 0;
    }

    GLint Shader::loc(const char* uniform) {
        auto it = locations.find(uniform);
        if (it != locations.end()) return it->second;
        GLint l = glGetUniformLocation(program, uniform);
        locations[uniform] = l;
        return l;
    }

    void Shader::setTex(const char* n, int unit, GLuint texId) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texId);
        glUniform1i(loc(n), unit);
    }

    // ----------------------------------------------------------------- Texture
    static GLint internalFor(GLenum fmt) {
        switch (fmt) {
        case GL_R8: return GL_R8;
        case GL_RG8: return GL_RG8;
        case GL_RGB8: return GL_RGB8;
        case GL_RGBA8: return GL_RGBA8;
        default: return (GLint)fmt;
        }
    }
    static GLenum dataFmtFor(GLenum fmt) {
        switch (fmt) {
        case GL_R8: return GL_RED;
        case GL_RG8: return GL_RG;
        case GL_RGB8: return GL_RGB;
        case GL_RGBA8: return GL_RGBA;
        default: return fmt;
        }
    }

    void Texture::create(int w, int h, GLenum fmt, const void* data, GLenum dataFmt,
                         GLenum dataType, bool linear, bool repeat) {
        destroy();
        width = w;
        height = h;
        format = fmt;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFor(fmt), w, h, 0,
                     data ? dataFmt : dataFmtFor(fmt), dataType, data);
        GLint filter = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        GLint wrap = repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    }

    bool Texture::loadFile(const std::string& path, bool linear, bool repeat) {
        int channels = 0;
        unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (!data) {
            std::fprintf(stderr, "[gpu] failed to load image: %s\n", path.c_str());
            return false;
        }
        create(width, height, GL_RGBA8, data, GL_RGBA, GL_UNSIGNED_BYTE, linear, repeat);
        stbi_image_free(data);
        return true;
    }

    void Texture::bind(int unit) const {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, id);
    }

    void Texture::destroy() {
        if (id) glDeleteTextures(1, &id);
        id = 0;
        width = height = 0;
    }

    // ------------------------------------------------------------ RenderTarget
    void RenderTarget::create(int w, int h, GLenum fmt, bool linear) {
        destroy();
        width = w;
        height = h;
        color.create(w, h, fmt, nullptr, dataFmtFor(fmt), GL_UNSIGNED_BYTE, linear);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[gpu] framebuffer incomplete: 0x%04X (%dx%d fmt 0x%04X)\n",
                         status, w, h, fmt);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void RenderTarget::bind() const {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }

    void RenderTarget::clear(const Vec4& c) const {
        bind();
        glClearColor(c.x, c.y, c.z, c.w);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void RenderTarget::destroy() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        fbo = 0;
        color.destroy();
    }

    // --------------------------------------------------------- FullscreenPass
    static const char* kFullscreenVS = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

    bool FullscreenPass::build(const char* vs, const char* fs, const std::string& debugName,
                               std::string* error) {
        return shader.build(vs, fs, debugName, error);
    }

    static GLuint g_dummyVAO = 0;

    void FullscreenPass::draw() const {
        if (!g_dummyVAO) glGenVertexArrays(1, &g_dummyVAO);
        glBindVertexArray(g_dummyVAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
    }

    // -------------------------------------------------------------- QuadBatch
    static const char* kQuadVS = R"(#version 330 core
layout(location = 1) in vec2 aCenter;
layout(location = 2) in vec2 aAxisX;
layout(location = 3) in vec2 aAxisY;
layout(location = 4) in vec2 aUvMin;
layout(location = 5) in vec2 aUvMax;
layout(location = 6) in vec4 aColor;
uniform mat4 uProj;
out vec2 vUv;
out vec4 vColor;
void main() {
    vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    vec2 l = corners[gl_VertexID & 3];
    vec2 world = aCenter + aAxisX * l.x + aAxisY * l.y;
    gl_Position = uProj * vec4(world, 0.0, 1.0);
    vUv = mix(aUvMin, aUvMax, (l + 1.0) * 0.5);
    vColor = aColor;
}
)";

    const char* quadVertexShader() { return kQuadVS; }
    const char* fullscreenVertexShader() { return kFullscreenVS; }

    QuadBatch::Instance makeSprite(const Vec2& center, float w, float h, float rot, const Vec2& uvMin,
                                   const Vec2& uvMax, const Vec4& color) {
        float c = std::cos(rot), s = std::sin(rot);
        QuadBatch::Instance inst;
        inst.center = center;
        inst.axisX = Vec2(c * w * 0.5f, -s * w * 0.5f);
        inst.axisY = Vec2(s * h * 0.5f, c * h * 0.5f);
        inst.uvMin = uvMin;
        inst.uvMax = uvMax;
        inst.color = color;
        return inst;
    }

    QuadBatch::Instance makeSpriteAxes(const Vec2& center, const Vec2& axisX, const Vec2& axisY,
                                       const Vec2& uvMin, const Vec2& uvMax, const Vec4& color) {
        QuadBatch::Instance inst;
        inst.center = center;
        inst.axisX = axisX;
        inst.axisY = axisY;
        inst.uvMin = uvMin;
        inst.uvMax = uvMax;
        inst.color = color;
        return inst;
    }

    void setBlendAlpha() {
        glBlendEquation(GL_FUNC_ADD);
        // Separate alpha: accumulating alpha with ONE keeps the scene opaque while
        // still giving normal straight-alpha color compositing.
        if (glBlendFuncSeparate)
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        else
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void setBlendPremultiplied() {
        glBlendEquation(GL_FUNC_ADD);
        if (glBlendFuncSeparate)
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        else
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }

    void setBlendAdditive() {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
    }

    void QuadBatch::init(int maxInstances) {
        capacity = maxInstances;
        count = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(capacity * sizeof(Instance)), nullptr,
                     GL_DYNAMIC_DRAW);
        const GLsizei stride = sizeof(Instance);
        auto attr = [&](GLuint index, GLint size, size_t byteOffset) {
            glEnableVertexAttribArray(index);
            glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride,
                                  reinterpret_cast<const void*>(byteOffset));
            glVertexAttribDivisor(index, 1);
        };
        attr(1, 2, offsetof(Instance, center));
        attr(2, 2, offsetof(Instance, axisX));
        attr(3, 2, offsetof(Instance, axisY));
        attr(4, 2, offsetof(Instance, uvMin));
        attr(5, 2, offsetof(Instance, uvMax));
        attr(6, 4, offsetof(Instance, color));
        glBindVertexArray(0);
    }

    void QuadBatch::begin() {
        count = 0;
        instances.clear();
    }

    void QuadBatch::push(const Instance& inst) {
        if (count >= capacity) {
            // grow
            capacity = capacity * 2 + 256;
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(capacity * sizeof(Instance)), nullptr,
                         GL_DYNAMIC_DRAW);
        }
        instances.push_back(inst);
        count++;
    }

    void QuadBatch::end() {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * sizeof(Instance)),
                        instances.empty() ? nullptr : instances.data());
    }

    void QuadBatch::destroy() {
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        vbo = vao = 0;
        instances.clear();
    }

    void drawQuads(const QuadBatch& batch) {
        if (batch.count <= 0) return;
        glBindVertexArray(batch.vao);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, batch.count);
        glBindVertexArray(0);
    }

    void beginFrame(int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    void endFrame() {}

} // namespace gpu
