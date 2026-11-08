// gpu.h - small GL3.3 helper layer: shaders, textures, render targets, fullscreen passes,
//         instanced quads.
#pragma once

#include "common.h"
#include "gl.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace gpu {

    struct Shader {
        GLuint program = 0;
        std::string name;
        std::unordered_map<std::string, GLint> locations;

        bool build(const char* vs, const char* fs, const std::string& debugName, std::string* error);
        bool loadFile(const std::string& vsPath, const std::string& fsPath, std::string* error);
        void destroy();
        void use() const { glUseProgram(program); }
        GLint loc(const char* uniform);

        void set(const char* n, int v) { glUniform1i(loc(n), v); }
        void set2i(const char* n, int x, int y) { glUniform2i(loc(n), x, y); }
        void set(const char* n, float v) { glUniform1f(loc(n), v); }
        void set(const char* n, float x, float y) { glUniform2f(loc(n), x, y); }
        void set(const char* n, float x, float y, float z) { glUniform3f(loc(n), x, y, z); }
        void set(const char* n, float x, float y, float z, float w) { glUniform4f(loc(n), x, y, z, w); }
        void set(const char* n, const Vec2& v) { glUniform2f(loc(n), v.x, v.y); }
        void set(const char* n, const Vec3& v) { glUniform3f(loc(n), v.x, v.y, v.z); }
        void set(const char* n, const Vec4& v) { glUniform4f(loc(n), v.x, v.y, v.z, v.w); }
        void setMat4(const char* n, const Mat4& m) { glUniformMatrix4fv(loc(n), 1, GL_FALSE, m.data()); }
        void setVec4Array(const char* n, const Vec4* v, int count) {
            glUniform4fv(loc(n), count, reinterpret_cast<const float*>(v));
        }
        void setTex(const char* n, int unit, GLuint texId);
    };

    struct Texture {
        GLuint id = 0;
        int width = 0;
        int height = 0;
        GLenum format = GL_RGBA8;

        void create(int w, int h, GLenum fmt, const void* data = nullptr, GLenum dataFmt = GL_RGBA,
                    GLenum dataType = GL_UNSIGNED_BYTE, bool linear = true, bool repeat = false);
        bool loadFile(const std::string& path, bool linear = true, bool repeat = false);
        void bind(int unit) const;
        void destroy();
        bool valid() const { return id != 0; }
    };

    struct RenderTarget {
        GLuint fbo = 0;
        Texture color;
        int width = 0;
        int height = 0;

        void create(int w, int h, GLenum fmt, bool linear = true);
        void bind() const; // binds fbo + viewport
        void clear(const Vec4& c = Vec4(0, 0, 0, 0)) const;
        void destroy();
    };

    // Fullscreen triangle pass (no VBO needed).
    struct FullscreenPass {
        Shader shader;
        bool build(const char* vs, const char* fs, const std::string& debugName, std::string* error);
        void draw() const; // call shader.use() and set uniforms before
    };

    // Instanced textured quads, fully affine: world = center + axisX*lx + axisY*ly,
    // local coords lx,ly in [-1,1]; (axisX,axisY) is the image's half-width/height vectors
    // (axisY pointing "down" the image in screen space when the quad is unrotated).
    struct QuadBatch {
        GLuint vao = 0;
        GLuint vbo = 0;
        int capacity = 0;
        int count = 0;

        struct Instance {
            Vec2 center;
            Vec2 axisX;    // vector from center to local x = +1 (right edge midpoint)
            Vec2 axisY;    // vector from center to local y = +1 (bottom edge midpoint)
            Vec2 uvMin;    // texture coords of the local (-1,-1) corner (image top-left)
            Vec2 uvMax;    // texture coords of the local (+1,+1) corner (image bottom-right)
            Vec4 color;    // rgba multiplier
        };

        std::vector<Instance> instances;

        void init(int maxInstances = 4096);
        void begin();
        void push(const Instance& inst);
        void end(); // uploads
        void destroy();
    };

    // Build an instance for a sprite that is `w` x `h` world units, centered at `center`,
    // rotated by `rot` radians (positive = counter-clockwise on screen).
    QuadBatch::Instance makeSprite(const Vec2& center, float w, float h, float rot,
                                   const Vec2& uvMin = Vec2(0, 0), const Vec2& uvMax = Vec2(1, 1),
                                   const Vec4& color = Vec4(1, 1, 1, 1));

    // Build an instance for a sprite from explicit axis vectors.
    QuadBatch::Instance makeSpriteAxes(const Vec2& center, const Vec2& axisX, const Vec2& axisY,
                                       const Vec2& uvMin = Vec2(0, 0), const Vec2& uvMax = Vec2(1, 1),
                                       const Vec4& color = Vec4(1, 1, 1, 1));

    // Blend helpers.
    void setBlendAlpha();          // straight alpha: src*a + dst*(1-a)
    void setBlendPremultiplied();  // premultiplied: src + dst*(1-a)
    void setBlendAdditive();       // src + dst

    // Draw a batch with the currently bound shader (attributes: loc 1 center,
    // 2 axisX, 3 axisY, 4 uvMin, 5 uvMax, 6 color => vec4).
    void drawQuads(const QuadBatch& batch);

    // Vertex shader source for QuadBatch drawing (declare `uniform mat4 uProj`).
    const char* quadVertexShader();

    // Vertex shader source for a fullscreen triangle (outputs `vUv` in [0,1]).
    const char* fullscreenVertexShader();

    void beginFrame(int w, int h);
    void endFrame();

} // namespace gpu
