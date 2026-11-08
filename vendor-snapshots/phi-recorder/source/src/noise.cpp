// noise.cpp - Phigros 4.0 "噪域" (blockArea) renderer.
//
// Pipeline (official NoiseField material, reconstructed from Phira-Pro's RPEN
// skin + native material): see assets/default/noise/NoiseField/config.json.
//
//   blockMask (6 coverage bits, R8 at screen/8)
//     -> composeActive (R8) / composeDisabled (RG8)
//     -> edge (effects.r) + 5x glow ping-pong (pingA) -> glowExtract (effects.g)
//     -> disabled + ready: additive background layer (below judgement lines)
//     -> active: post-process overlay (scene refraction + sparks + glow)
#include "noise.h"
#include "stb_image_write.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace noise {

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Convert a GLSL ES 100 skin shader into a GLSL 330 core shader that matches the
// fullscreen vertex shader (which outputs `vUv`).
std::string adaptShader(std::string src) {
    // Replace the #version line and inject the fragment output / varying name.
    size_t vp = src.find("#version");
    if (vp == std::string::npos) return std::string();
    size_t lineEnd = src.find('\n', vp);
    if (lineEnd == std::string::npos) lineEnd = src.size();
    src = src.substr(0, vp) +
          "#version 330 core\nout vec4 fragColor;\n#define uv vUv\nin vec2 vUv;\n" +
          src.substr(lineEnd + 1);

    // Drop precision statements.
    for (;;) {
        size_t p = src.find("precision ");
        if (p == std::string::npos) break;
        size_t e = src.find('\n', p);
        if (e == std::string::npos) {
            src.erase(p);
            break;
        }
        src.erase(p, e + 1 - p);
    }

    // Drop the es100 varying declaration (we already declare `in vec2 vUv`).
    replaceAll(src, "varying highp vec2 uv;", "");
    replaceAll(src, "varying mediump vec2 uv;", "");
    replaceAll(src, "varying vec2 uv;", "");
    replaceAll(src, "attribute highp vec4 vertex;", "");
    replaceAll(src, "attribute vec4 vertex;", "");
    replaceAll(src, "texture2D", "texture");
    replaceAll(src, "gl_FragColor", "fragColor");
    return src;
}

// Mask rasterisation fragment shader (instanced quads, additive accumulation).
const char* kQuadColorFS = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

// Pack the three RGBA8 coverage textures into the R8 block mask.
// bit 0..5 = ANormal, ASubtract, D+R Normal, D+R Subtract, R Normal, R Subtract
// Normal coverage saturates the R channel; each subtract zone adds 0.1 to G, so
// "exactly one subtract" is G around 26/255.
const char* kMaskPackFS = R"(#version 330 core
in vec2 vUv;
uniform sampler2D maskA;
uniform sampler2D maskNA;
uniform sampler2D maskR;
out vec4 fragColor;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 a = texelFetch(maskA, p, 0).rgb;
    vec3 na = texelFetch(maskNA, p, 0).rgb;
    vec3 r = texelFetch(maskR, p, 0).rgb;
    int bits = 0;
    if (a.r > 0.5) bits |= 1;
    float sa = a.g * 255.0;
    if (sa > 23.5 && sa < 28.5) bits |= 2;
    if (na.r > 0.5) bits |= 4;
    float sna = na.g * 255.0;
    if (sna > 23.5 && sna < 28.5) bits |= 8;
    if (r.r > 0.5) bits |= 16;
    float sr = r.g * 255.0;
    if (sr > 23.5 && sr < 28.5) bits |= 32;
    fragColor = vec4(float(bits) / 255.0, 0.0, 0.0, 1.0);
}
)";

// Disabled blocks, additive variant (official disabled.glsl minus the screen
// copy: fixed-function blend One/One on the chart target is equivalent).
const char* kDisabledAddFS = R"(#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D composeRT;
uniform sampler2D blockNoise;
uniform sampler2D pointNoise;
uniform float time;
uniform vec4 outlineColor;
uniform float disabledFillOpacity;
const float disabledDisplaceSpeed = 0.30000001192092896;
const vec2 disabledDisplaceDirection = vec2(1.0,1.0);
const vec2 disabledDisplaceMapST = vec2(0.50,0.20);
const vec2 disabledSparkMapST = vec2(3.00,1.20);
const float disabledSparkMapOpacity = 3.5;
const float disabledSparkDisplaceIntensity = 2.2899999618530273;
vec3 nfRGBtoHSV(vec3 c) {
    float hi = max(max(c.r,c.g),c.b);
    float lo = min(min(c.r,c.g),c.b);
    float d = hi-lo;
    float h = 0.0;
    if (d > 1e-10) {
        if (hi==c.r) h=mod((c.g-c.b)/d,6.0);
        else if (hi==c.g) h=(c.b-c.r)/d+2.0;
        else h=(c.r-c.g)/d+4.0;
        h=mod(h/6.0,1.0);
    }
    return vec3(h, hi<=1e-10 ? 0.0 : d/hi, hi);
}
vec3 nfHSVtoRGB(vec3 h) {
    vec3 p=clamp(abs(fract(h.xxx+vec3(1.0,2.0/3.0,1.0/3.0))*6.0-3.0)-1.0,0.0,1.0);
    return h.z*(vec3(1.0)+h.y*(p-vec3(1.0)));
}
vec3 nfVariant(float s, float v) {
    vec3 c=nfRGBtoHSV(outlineColor.rgb);
    c.y=clamp(c.y*s,0.0,1.0);
    c.z=clamp(c.z*v,0.0,1.0);
    return nfHSVtoRGB(c);
}
vec3 nfDisabledFill() { return nfVariant(1.0794083394988696,0.4970000088214874); }
vec3 nfDisabledSpark() { return nfVariant(1.1197181312580806,0.31132078170776367); }
void main() {
    float comp = texture(composeRT, vUv).r;
    if (comp - 0.0001 < 0.0) { fragColor = vec4(0.0); return; }
    vec2 dir = normalize(disabledDisplaceDirection);
    vec2 perp = vec2(-dir.y, dir.x);
    float t = (time / 20.0) * disabledDisplaceSpeed;
    vec2 duv = vUv * disabledDisplaceMapST;
    float dPerp = texture(blockNoise, duv + perp * t).r;
    float dDir  = texture(blockNoise, duv + dir  * t).r;
    float dispAvg = (dPerp + dDir) * 0.5;
    vec2 disp = dir * (dDir - 0.5) + perp * (dPerp - 0.5);
    vec2 sparkUV = vUv * disabledSparkMapST + disp * disabledSparkDisplaceIntensity;
    float spark = texture(pointNoise, sparkUV).r;
    vec3 rgb = nfDisabledFill() * disabledFillOpacity;
    rgb += spark * nfDisabledSpark() * dispAvg * disabledSparkMapOpacity;
    rgb *= comp;
    fragColor = vec4(rgb, 0.0);
}
)";

// Ready blocks, additive variant (official ready.glsl minus the screen copy).
// 噪域闪烁: the official pulse is `sin(time * shineSpeed) * .5 + 1` scaled by
// shineBrightness — a slow breathing highlight, not a strobe. Kept verbatim.
const char* kReadyAddFS = R"(#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D composeRT;
uniform sampler2D blockMask;
uniform vec2 screenSize;
uniform float time;
uniform float shineBrightness;
uniform float shineSpeed;
const float maskPixelScale = 8.0;
float nfBit(float value, float position) { return mod(floor(value / exp2(position)), 2.0); }
float nfMaskBit(vec2 p, float bit) {
    vec2 msz = max(floor(screenSize / max(maskPixelScale, 1.0)), vec2(1.0));
    vec2 q = (floor(p * msz) + 0.5) / msz;
    float maskWord = floor(texture(blockMask, q).r * 255.0 + 0.5);
    return nfBit(maskWord, bit);
}
void main() {
    float dn = nfMaskBit(vUv, 4.0);
    float ds = nfMaskBit(vUv, 5.0);
    float m = ds - dn;
    float c = texture(composeRT, vUv).r;
    float a = c * abs(m);
    if (a - 0.0001 < 0.0) { fragColor = vec4(0.0); return; }
    float pulse = sin(time * shineSpeed) * 0.5 + 1.0;
    vec3 rgb = vec3(a) * pulse * (vec3(1.0) * shineBrightness);
    fragColor = vec4(rgb * a, 0.0);
}
)";

}  // namespace

Zone makeZone(const BlockArea::Transform& tr, bool isSubtract, int state) {
    Zone z;
    Vec2 corners[4];
    blockCorners(tr, corners);  // [(-w,-h), (w,-h), (w,h), (-w,h)]
    z.center = (corners[0] + corners[2]) * 0.5f;
    z.axisX = (corners[1] - corners[0]) * 0.5f;
    z.axisY = (corners[3] - corners[0]) * 0.5f;
    z.isSubtract = isSubtract;
    z.state = state;
    return z;
}

bool NoiseField::loadShader(gpu::Shader& sh, const std::string& file, std::string* error) {
    std::string path = assetDir_ + "/noise/NoiseField/" + file;
    std::string src = readFile(path);
    if (src.empty()) {
        if (error) *error = "cannot read noise shader: " + path;
        return false;
    }
    std::string adapted = adaptShader(src);
    return sh.build(gpu::fullscreenVertexShader(), adapted.c_str(), file, error);
}

bool NoiseField::makeTargets(std::string* error) {
    mw_ = std::max(1, w_ / 8);
    mh_ = std::max(1, h_ / 8);
    const int ew = std::max(1, w_ / 4), eh = std::max(1, h_ / 4);
    const int sw = std::max(1, w_ / 6), shh = std::max(1, h_ / 6);

    t_.maskA.create(mw_, mh_, GL_RGBA8, false);
    t_.maskNA.create(mw_, mh_, GL_RGBA8, false);
    t_.maskR.create(mw_, mh_, GL_RGBA8, false);
    // Match NoiseField/config.json and RPEN Skin::target: unspecified filter
    // means nearest. Interpolating compose before edge dilation softens the
    // discrete contour irreversibly. Only effects explicitly requests linear
    // (its red edge is sampled at texel centers, green glow stays smooth).
    t_.activeCompose.create(mw_, mh_, GL_R8, false);
    t_.disabledCompose.create(mw_, mh_, GL_RG8, false);
    t_.effects.create(ew, eh, GL_RG8, true);
    t_.pingA.create(ew, eh, GL_RG8, false);
    t_.pingB.create(ew, eh, GL_RG8, false);
    t_.sceneColor.create(sw, shh, GL_RGBA8, false);
    t_.flash.create(mw_, mh_, GL_R8, false);
    t_.blockMask.create(mw_, mh_, GL_R8, false);
    t_.screen.create(w_, h_, GL_RGBA8, true);
    if (!t_.blockMask.color.valid()) {
        if (error) *error = "noise: failed to create render targets";
        return false;
    }
    (void)error;
    return true;
}

bool NoiseField::init(const std::string& assetDir, int screenW, int screenH, float aspect,
                      std::string* error) {
    assetDir_ = assetDir;
    w_ = screenW;
    h_ = screenH;
    aspect_ = aspect;

    std::string err;
    struct ShaderDef {
        gpu::Shader* sh;
        const char* file;
    };
    ShaderDef defs[] = {
        {&shComposeActive_, "composeActive.glsl"},
        {&shComposeDisabled_, "composeDisabled.glsl"},
        {&shEdge_, "edge.glsl"},
        {&shGlow_, "glow.glsl"},
        {&shGlowExtract_, "glowExtract.glsl"},
        {&shMain_, "main.glsl"},
        {&shCopy_, "copy.glsl"},
    };
    for (const auto& d : defs) {
        if (!loadShader(*d.sh, d.file, error)) return false;
    }
    if (!shMaskPack_.build(gpu::fullscreenVertexShader(), kMaskPackFS, "maskPack", error)) return false;
    if (!shDisabled_.build(gpu::fullscreenVertexShader(), kDisabledAddFS, "disabledAdd", error)) return false;
    if (!shReady_.build(gpu::fullscreenVertexShader(), kReadyAddFS, "readyAdd", error)) return false;
    if (!shQuad_.build(gpu::quadVertexShader(), kQuadColorFS, "quadColor", error)) return false;

    // Noise textures: BlockNoise1 & FD_Noise mirror, PointNoise repeat (nearest).
    if (!texBlockNoise_.loadFile(assetDir + "/blockarea/BlockNoise1.png", false, false)) {
        if (error) *error = "cannot load BlockNoise1.png";
        return false;
    }
    if (!texPointNoise_.loadFile(assetDir + "/blockarea/PointNoise.png", false, true)) {
        if (error) *error = "cannot load PointNoise.png";
        return false;
    }
    if (!texFieldNoise_.loadFile(assetDir + "/blockarea/FD_Noise.png", false, false)) {
        if (error) *error = "cannot load FD_Noise.png";
        return false;
    }
    auto setWrap = [](gpu::Texture& t, GLenum wrapS, GLenum wrapT) {
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)wrapS);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)wrapT);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    setWrap(texBlockNoise_, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT);
    setWrap(texPointNoise_, GL_REPEAT, GL_REPEAT);
    setWrap(texFieldNoise_, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT);

    quad_.init(4096);
    if (!makeTargets(error)) return false;
    t_.flash.clear();
    t_.blockMask.clear();
    t_.ready = true;
    return true;
}

bool NoiseField::resize(int screenW, int screenH, float aspect, std::string* error) {
    if (screenW == w_ && screenH == h_) {
        aspect_ = aspect;
        return true;
    }
    destroy();
    return init(assetDir_, screenW, screenH, aspect, error);
}

void NoiseField::destroy() {
    if (!t_.ready) return;
    t_.maskA.destroy();
    t_.maskNA.destroy();
    t_.maskR.destroy();
    t_.activeCompose.destroy();
    t_.disabledCompose.destroy();
    t_.effects.destroy();
    t_.pingA.destroy();
    t_.pingB.destroy();
    t_.sceneColor.destroy();
    t_.flash.destroy();
    t_.blockMask.destroy();
    t_.screen.destroy();
    t_.ready = false;
    shComposeActive_.destroy();
    shComposeDisabled_.destroy();
    shEdge_.destroy();
    shGlow_.destroy();
    shGlowExtract_.destroy();
    shMain_.destroy();
    shCopy_.destroy();
    shMaskPack_.destroy();
    shDisabled_.destroy();
    shReady_.destroy();
    shQuad_.destroy();
    texBlockNoise_.destroy();
    texPointNoise_.destroy();
    texFieldNoise_.destroy();
    quad_.destroy();
}

void NoiseField::drawFullscreen() {
    static GLuint vao = 0;
    if (!vao) glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void NoiseField::copy(gpu::RenderTarget& dst, const gpu::Texture& src, bool /*linear*/) {
    dst.bind();
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    shCopy_.use();
    shCopy_.setTex("mainTex", 0, src.id);
    drawFullscreen();
}

void NoiseField::drawMaskQuads(gpu::RenderTarget& dst, const std::vector<Zone>& zones, int state) {
    dst.bind();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    quad_.begin();
    for (const Zone& z : zones) {
        if (z.state != state) continue;
        gpu::QuadBatch::Instance inst;
        inst.center = z.center;
        inst.axisX = z.axisX;
        inst.axisY = z.axisY;
        inst.uvMin = Vec2(0, 0);
        inst.uvMax = Vec2(1, 1);
        inst.color = z.isSubtract ? Vec4(0.0f, 0.1f, 0.0f, 0.0f) : Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        quad_.push(inst);
    }
    quad_.end();
    if (quad_.count == 0) return;
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    shQuad_.use();
    Mat4 proj;  // identity
    proj.m[5] = aspect_;  // chart space y in [-1/aspect, 1/aspect] -> NDC [-1,1]
    shQuad_.setMat4("uProj", proj);
    gpu::drawQuads(quad_);
}

void NoiseField::buildMask(const std::vector<Zone>& zones) {
    hasZone_ = !zones.empty();
    hasActive_ = hasDisabled_ = hasReady_ = false;
    for (const Zone& z : zones) {
        if (z.state == 0) hasActive_ = true;
        else if (z.state == 1) hasDisabled_ = true;
        else hasReady_ = true;
        if (z.flash > 0.002f) hasFlash_ = true;
    }
    if (!hasZone_) {
        t_.blockMask.clear();
        t_.flash.clear();
        hasFlash_ = false;
        return;
    }

    drawMaskQuads(t_.maskA, zones, 0);
    drawMaskQuads(t_.maskNA, zones, 1);
    drawMaskQuads(t_.maskR, zones, 2);

    // Pack the six coverage bits.
    t_.blockMask.bind();
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    shMaskPack_.use();
    shMaskPack_.setTex("maskA", 0, t_.maskA.color.id);
    shMaskPack_.setTex("maskNA", 1, t_.maskNA.color.id);
    shMaskPack_.setTex("maskR", 2, t_.maskR.color.id);
    drawFullscreen();

    // Per-zone pop-in flash: a small R8 mask holding the current whitening
    // intensity, so each noise area flashes on its own timeline.
    t_.flash.bind();
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (hasFlash_) {
        quad_.begin();
        for (const Zone& z : zones) {
            if (z.flash <= 0.002f) continue;
            gpu::QuadBatch::Instance inst;
            inst.center = z.center;
            inst.axisX = z.axisX;
            inst.axisY = z.axisY;
            inst.uvMin = Vec2(0, 0);
            inst.uvMax = Vec2(1, 1);
            inst.color = Vec4(z.flash, 0.0f, 0.0f, 0.0f);
            quad_.push(inst);
        }
        quad_.end();
        if (quad_.count > 0) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ONE);
            shQuad_.use();
            Mat4 proj;  // identity
            proj.m[5] = aspect_;
            shQuad_.setMat4("uProj", proj);
            gpu::drawQuads(quad_);
        }
    }

    // Active compose chain.
    t_.activeCompose.bind();
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    shComposeActive_.use();
    shComposeActive_.setTex("blockMask", 0, t_.blockMask.color.id);
    shComposeActive_.setTex("blockNoise", 1, texBlockNoise_.id);
    shComposeActive_.set("screenSize", (float)w_, (float)h_);
    shComposeActive_.set("time", time_);
    drawFullscreen();

    t_.disabledCompose.bind();
    glClear(GL_COLOR_BUFFER_BIT);
    shComposeDisabled_.use();
    shComposeDisabled_.setTex("blockMask", 0, t_.blockMask.color.id);
    shComposeDisabled_.set("screenSize", (float)w_, (float)h_);
    drawFullscreen();
    if (!hasActive_) return;

    // Edge + glow.
    t_.effects.bind();
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    shEdge_.use();
    shEdge_.setTex("mainTex", 0, t_.activeCompose.color.id);
    shEdge_.setTex("composeRT", 1, t_.activeCompose.color.id);
    shEdge_.set("targetTexelSize", 1.0f / (float)t_.effects.width, 1.0f / (float)t_.effects.height);
    drawFullscreen();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const float weights[5] = {0.458567970132f, 0.282861223139f, 0.156589227109f,
                              0.07305908496f, 0.024947807942f};
    gpu::RenderTarget* src = &t_.activeCompose;
    for (int i = 0; i < 5; ++i) {
        gpu::RenderTarget& dst = (i % 2 == 0) ? t_.pingA : t_.pingB;
        dst.bind();
        shGlow_.use();
        shGlow_.setTex("mainTex", 0, src->color.id);
        shGlow_.setTex("composeRT", 1, t_.activeCompose.color.id);
        shGlow_.set("targetTexelSize", 1.0f / (float)dst.width, 1.0f / (float)dst.height);
        shGlow_.set("passWeight", weights[i]);
        shGlow_.set("firstPass", i == 0 ? 1.0f : 0.0f);
        drawFullscreen();
        src = &dst;
    }

    t_.effects.bind();
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    shGlowExtract_.use();
    shGlowExtract_.setTex("mainTex", 0, t_.pingA.color.id);
    drawFullscreen();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void NoiseField::drawBackground(gpu::RenderTarget& scene) {
    if (!t_.ready || !hasZone_ || (!hasDisabled_ && !hasReady_)) return;

    // No scene copy here: the additive variants of disabled.glsl / ready.glsl do
    // not sample the screen (the original skin shader did; the additive rewrite
    // drops that read), so copying the frame would be a full-screen no-op.
    scene.bind();
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    if (hasDisabled_) {
        shDisabled_.use();
        shDisabled_.setTex("composeRT", 0, t_.disabledCompose.color.id);
        shDisabled_.setTex("blockNoise", 1, texBlockNoise_.id);
        shDisabled_.setTex("pointNoise", 2, texPointNoise_.id);
        shDisabled_.set("time", time_);
        shDisabled_.set("outlineColor", settings_.outlineColor);
        shDisabled_.set("disabledFillOpacity", settings_.disabledFillOpacity);
        drawFullscreen();
    }
    if (hasReady_) {
        shReady_.use();
        shReady_.setTex("composeRT", 0, t_.disabledCompose.color.id);
        shReady_.setTex("blockMask", 1, t_.blockMask.color.id);
        shReady_.set("screenSize", (float)w_, (float)h_);
        shReady_.set("time", time_);
        shReady_.set("shineBrightness", settings_.shineBrightness);
        shReady_.set("shineSpeed", settings_.shineSpeed);
        drawFullscreen();
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

void NoiseField::drawActive(gpu::RenderTarget& scene) {
    if (!t_.ready || !hasZone_ || !hasActive_) return;

    copy(t_.screen, scene.color);  // main.glsl samples the pre-overlay frame
    copy(t_.sceneColor, scene.color);
    renderOverlay(scene, false);
}

// Active-block composite (formerly rendered into a full-size `overlay` target and
// blitted back).  Writing straight into the destination saves one full-screen
// pass and one full-screen blit per frame; the result is bit-identical because
// the deferred blit only copied rgb bytes.
void NoiseField::renderOverlay(gpu::RenderTarget& target, bool keepAlpha) {
    target.bind();
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, keepAlpha ? GL_TRUE : GL_FALSE);
    shMain_.use();
    shMain_.setTex("screenTexture", 0, t_.screen.color.id);
    shMain_.setTex("blockMask", 1, t_.blockMask.color.id);
    shMain_.setTex("composeRT", 2, t_.activeCompose.color.id);
    shMain_.setTex("effectRT", 3, t_.effects.color.id);
    shMain_.setTex("readyComposeRT", 4, t_.disabledCompose.color.id);
    shMain_.setTex("sceneColorRT", 5, t_.sceneColor.color.id);
    shMain_.setTex("blockNoise", 7, texBlockNoise_.id);
    shMain_.setTex("pointNoise", 8, texPointNoise_.id);
    shMain_.setTex("fieldNoise", 9, texFieldNoise_.id);
    shMain_.setTex("flashRT", 10, t_.flash.color.id);
    shMain_.set("screenSize", (float)w_, (float)h_);
    shMain_.set("effectRTSize", (float)t_.effects.width, (float)t_.effects.height);
    shMain_.set("time", time_);
    shMain_.set("outlineColor", settings_.outlineColor);
    shMain_.set("fillOpacity", settings_.fillOpacity);
    shMain_.set("fillStrength", settings_.fillStrength);
    shMain_.set("edgeOpacity", settings_.edgeOpacity);
    shMain_.set("glowIntensity", settings_.glowIntensity);
    shMain_.set("backgroundPixelScale", settings_.backgroundPixelScale);
    shMain_.set("activeDisplaceSpeed", settings_.activeDisplaceSpeed);
    shMain_.set("activeDisplaceStrength", settings_.activeDisplaceStrength);
    shMain_.set("activeSparkMapOpacity", settings_.activeSparkMapOpacity);
    shMain_.set("disabledFillOpacity", settings_.disabledFillOpacity);
    shMain_.set("shineBrightness", settings_.shineBrightness);
    shMain_.set("shineSpeed", settings_.shineSpeed);
    // (Ready strobe intensity is baked into the ready compose layer.)
    shMain_.set("flashStrength", settings_.flashStrength);
    drawFullscreen();

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

bool NoiseField::debugDump(const std::string& prefix, std::string* error) {
    if (!t_.ready) {
        if (error) *error = "noise targets not ready";
        return false;
    }
    unsigned char* px = (unsigned char*)std::malloc((size_t)w_ * (size_t)h_ * 4);
    unsigned char* flip = (unsigned char*)std::malloc((size_t)w_ * (size_t)h_ * 4);
    if (!px || !flip) {
        std::free(px);
        std::free(flip);
        if (error) *error = "out of memory";
        return false;
    }
    auto save = [&](gpu::RenderTarget& src, int tw, int th, const char* name) {
        gpu::RenderTarget tmp;
        tmp.create(tw, th, GL_RGBA8, false);
        copy(tmp, src.color);
        tmp.bind();
        glReadPixels(0, 0, tw, th, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // Flip rows so the PNG is top-down.
        const int rowBytes = tw * 4;
        for (int y = 0; y < th; ++y)
            std::memcpy(flip + (size_t)y * rowBytes, px + (size_t)(th - 1 - y) * rowBytes, rowBytes);
        std::string path = prefix + name;
        stbi_write_png(path.c_str(), tw, th, 4, flip, rowBytes);
        tmp.destroy();
    };
    save(t_.blockMask, mw_, mh_, "blockMask.png");
    save(t_.maskA, mw_, mh_, "maskA.png");
    save(t_.maskNA, mw_, mh_, "maskNA.png");
    save(t_.maskR, mw_, mh_, "maskR.png");
    save(t_.activeCompose, mw_, mh_, "activeCompose.png");
    save(t_.disabledCompose, mw_, mh_, "disabledCompose.png");
    save(t_.effects, w_ / 4, h_ / 4, "effects.png");
    save(t_.pingA, w_ / 4, h_ / 4, "pingA.png");
    save(t_.sceneColor, w_ / 6, h_ / 6, "sceneColor.png");
    save(t_.screen, w_, h_, "screen.png");
    // The active composite has no persistent target any more: rebuild it on demand.
    {
        gpu::RenderTarget overlayTmp;
        overlayTmp.create(w_, h_, GL_RGBA8, false);
        renderOverlay(overlayTmp, true);
        save(overlayTmp, w_, h_, "overlay.png");
        overlayTmp.destroy();
    }
    std::free(px);
    std::free(flip);
    return true;
}

}  // namespace noise
