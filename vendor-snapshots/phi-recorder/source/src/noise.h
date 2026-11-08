// noise.h - Phigros 4.0 "噪域" (blockArea) renderer.
//
// Implements the official NoiseField material pipeline (Stellate's RPEN skin,
// reconstructed from the native Unity material): block mask rasterisation ->
// active/disabled compose -> edge + 5-pass glow -> disabled/ready background
// composite -> active overlay with scene refraction and sparks.
#pragma once

#include "common.h"
#include "chart.h"
#include "gpu.h"

#include <string>
#include <vector>

namespace noise {

// One evaluated block area for the current frame, in chart space:
//   x in [-1, 1], y in [-1/aspect, 1/aspect]
struct Zone {
    Vec2 center;
    Vec2 axisX;      // half width vector (local +x direction * halfWidth)
    Vec2 axisY;      // half height vector (local +y direction * halfHeight)
    bool isSubtract = false;
    int state = 0;   // 0 = active, 1 = disabled/ready-part, 2 = ready
    float flash = 0.0f;  // 0..1 white pop-in flash (non-linear decay, per zone)
};

// Evaluated zone from a chart block transform.
Zone makeZone(const BlockArea::Transform& tr, bool isSubtract, int state);

// Official NoiseField material parameters (also used as user-editable knobs).
struct Settings {
    Vec4 outlineColor = Vec4(1.0f, 0.33018857f, 0.33018857f, 1.0f);
    float fillOpacity = 0.667f;
    float fillStrength = 0.667f;
    float edgeOpacity = 0.8f;
    float glowIntensity = 0.8f;
    float backgroundPixelScale = 6.0f;
    float activeDisplaceSpeed = 1.5f;
    float activeDisplaceStrength = 0.15f;
    float activeSparkMapOpacity = 5.69f;
    float disabledFillOpacity = 0.4f;
    float shineBrightness = 0.12f;
    float shineSpeed = 37.9f;
    // 噪域出现闪烁: a noise area whitens for a fraction of a second the moment
    // it becomes visible (see BlockArea::phaseStart). The steady breathing of a
    // Ready area is the official shine pulse above, not a strobe.
    float flashStrength = 0.85f;   // how strongly a fresh noise area whitens
    float flashTime = 0.18f;       // seconds a fresh area stays whitened
};

class NoiseField {
public:
    bool init(const std::string& assetDir, int screenW, int screenH, float aspect,
              std::string* error);
    bool resize(int screenW, int screenH, float aspect, std::string* error);
    void destroy();

    void setSettings(const Settings& s) { settings_ = s; }
    Settings& settingsRef() { return settings_; }
    const Settings& settingsRef() const { return settings_; }
    void setTime(float t) { time_ = t; }

    // True when at least one zone was visible in the last buildMask().
    bool hasZones() const { return hasZone_; }

    // Rasterise the six coverage bits of every zone into the block mask and run
    // the mask-resolution compose/edge/glow passes.
    void buildMask(const std::vector<Zone>& zones);

    // Disabled + Ready blocks: additive background layer, drawn right after the
    // chart background and before judgement lines (matches native sorting order).
    void drawBackground(gpu::RenderTarget& scene);

    // Active blocks: post-process overlay drawn after notes / HUD. Refracts the
    // scene, adds sparks, edges and glow.
    void drawActive(gpu::RenderTarget& scene);

    // Debug: dump every intermediate render target to <prefix><name>.png (RGBA8).
    bool debugDump(const std::string& prefix, std::string* error);

    int screenWidth() const { return w_; }
    int screenHeight() const { return h_; }

private:
    struct Targets {
        gpu::RenderTarget maskA, maskNA, maskR;  // RGBA8 coverage accumulation
        gpu::RenderTarget activeCompose;         // R8
        gpu::RenderTarget disabledCompose;       // RG8
        gpu::RenderTarget effects;               // RG8 (edge in r, glow in g)
        gpu::RenderTarget pingA, pingB;          // RG8 glow ping-pong
        gpu::RenderTarget sceneColor;            // RGBA8 1/6
        gpu::RenderTarget blockMask;             // R8 packed bits
        gpu::RenderTarget flash;                 // R8 per-zone pop-in flash
        gpu::RenderTarget screen;                // RGBA8 full (scene copy)
        bool ready = false;
    } t_;

    gpu::Shader shComposeActive_, shComposeDisabled_, shEdge_, shGlow_, shGlowExtract_;
    gpu::Shader shMain_, shCopy_, shMaskPack_, shDisabled_, shReady_, shQuad_;

    gpu::Texture texBlockNoise_, texPointNoise_, texFieldNoise_;
    gpu::QuadBatch quad_;

    Settings settings_;
    std::string assetDir_;
    int w_ = 0, h_ = 0;
    int mw_ = 0, mh_ = 0;   // mask resolution (screen / 8)
    float aspect_ = 1.7777f;
    float time_ = 0.0f;

    bool hasZone_ = false;
    bool hasActive_ = false, hasDisabled_ = false, hasReady_ = false, hasFlash_ = false;

    bool loadShader(gpu::Shader& sh, const std::string& file, std::string* error);
    bool makeTargets(std::string* error);
    void drawFullscreen();
    void renderOverlay(gpu::RenderTarget& target, bool keepAlpha);
    void copy(gpu::RenderTarget& dst, const gpu::Texture& src, bool linear = false);
    void drawMaskQuads(gpu::RenderTarget& dst, const std::vector<Zone>& zones, int state);
};

}  // namespace noise
