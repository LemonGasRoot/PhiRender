// common.h - shared math / utility types for phirender
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

static constexpr float PI = 3.14159265358979323846f;
static constexpr float PI_OVER_180 = 0.017453292519943295f;
static constexpr float DEG_180_PI = 57.29577951308232f;

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
    explicit Vec2(float v) : x(v), y(v) {}
    Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
    Vec2 operator*(const Vec2& o) const { return Vec2(x * o.x, y * o.y); }
    Vec2 operator/(float s) const { return Vec2(x / s, y / s); }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    float length() const { return std::sqrt(x * x + y * y); }
    Vec2 normalize() const { float l = length(); return l > 1e-20f ? Vec2(x / l, y / l) : Vec2(0.0f, 0.0f); }
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float v) : x(v), y(v), z(v) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator*(const Vec3& o) const { return Vec3(x * o.x, y * o.y, z * o.z); }
    Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
};

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    Vec4() = default;
    Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    Vec4(const Vec3& c, float a) : x(c.x), y(c.y), z(c.z), w(a) {}
    explicit Vec4(float v) : x(v), y(v), z(v), w(v) {}
    Vec4 operator*(float s) const { return Vec4(x * s, y * s, z * s, w * s); }
    Vec3 rgb() const { return Vec3(x, y, z); }
};

// Column-major 4x4 matrix (OpenGL layout).
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float* data() const { return m; }
    static Mat4 identity() { return Mat4(); }
    // Orthographic projection; y_down=true maps y=0 to the top of the screen.
    static Mat4 ortho(float left, float right, float bottom, float top, bool y_down) {
        Mat4 r;
        float rl = right - left, tb = top - bottom;
        r.m[0] = 2.0f / rl;
        r.m[5] = 2.0f / tb * (y_down ? -1.0f : 1.0f);
        r.m[10] = -1.0f;
        r.m[12] = -(right + left) / rl;
        r.m[13] = (y_down ? 1.0f : -1.0f) * ((top + bottom) / tb) * 1.0f;
        r.m[15] = 1.0f;
        return r;
    }
    // Translate * Rotate(-rot_deg, z) * Scale matrix for 2D quads (y-down screen space).
    // Positive rot_deg rotates counter-clockwise on screen, matching Phigros line rotation.
    static Mat4 trs2d(float tx, float ty, float rot_deg, float sx, float sy) {
        float rad = -rot_deg * PI_OVER_180;
        float c = std::cos(rad), s = std::sin(rad);
        Mat4 m;
        m.m[0] = c * sx;  m.m[1] = s * sx;
        m.m[4] = -s * sy; m.m[5] = c * sy;
        m.m[12] = tx; m.m[13] = ty;
        return m;
    }
};

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }

// Official Phigros easing (integer-power family, ease types 0..14 as in chart JSON).
// p in [0,1]; returns value in [0,1] for identity start/end 0/1.
static inline float ease01(float p, int type) {
    if (p <= 0.0f) return 0.0f;
    if (p >= 1.0f) return 1.0f;
    switch (type) {
    case 0: return p;
    case 13: return 0.0f;
    case 14: return 1.0f;
    default: break;
    }
    if (type < 1 || type > 12) return p;
    int power = (type - 1) / 3 + 2;
    int mode = (type - 1) % 3;
    float u = p;
    if (mode == 0) { // in
        return std::pow(u, (float)power);
    } else if (mode == 1) { // out
        return 1.0f - std::pow(1.0f - u, (float)power);
    } else { // in-out
        if (u < 0.5f) {
            return std::pow(2.0f, (float)(power - 1)) * std::pow(u, (float)power);
        }
        return 1.0f - std::pow(2.0f - 2.0f * u, (float)power) * 0.5f;
    }
}

// Generic eased interpolation over a time window.
static inline float easeValue(float t, float st, float et, float sv, float ev, int type) {
    if (et <= st) return t >= et ? ev : sv;
    float p = clampf((t - st) / (et - st), 0.0f, 1.0f);
    return sv + (ev - sv) * ease01(p, type);
}

// HSV helpers (used by noise masks/particles).
static inline Vec3 hsv2rgb(const Vec3& hsv) {
    float h = hsv.x - std::floor(hsv.x);
    float s = clampf(hsv.y, 0.0f, 1.0f);
    float v = hsv.z;
    float r = clampf(std::fabs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    float g = clampf(std::fabs(std::fmod(h * 6.0f + 2.0f, 6.0f) - 2.0f) - 1.0f, 0.0f, 1.0f);
    float b = clampf(std::fabs(std::fmod(h * 6.0f + 4.0f, 6.0f) - 2.0f) - 1.0f, 0.0f, 1.0f);
    return Vec3(v * (1.0f + s * (r - 1.0f)), v * (1.0f + s * (g - 1.0f)), v * (1.0f + s * (b - 1.0f)));
}

static inline std::string formatStr(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

static inline std::string utf8Sub(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}
