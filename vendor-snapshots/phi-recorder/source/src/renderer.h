// renderer.h - Phigros chart scene renderer (background, judgement lines, notes,
//              noise areas / 噪域, later HUD + hit effects).
//
// Scene coordinate space is pixels with y pointing down (0 = top). The chart's
// "size" zoom factor is 1.0; the official constants (PGRW/PGRH/NOTE_SIZE/...)
// are applied exactly as in the reference renderers.
#pragma once

#include "addon.h"
#include "chart.h"
#include "gpu.h"
#include "hud.h"
#include "noise.h"

#include <array>
#include <string>
#include <vector>

// Resource-pack layout.  The default pack follows PhiVideo's official-style
// naming (Notes/Tap.png, HoldAtlas bands, 5x6 HitFx grid); a Phira-Pro style
// respack/ directory overrides it (click.png / hold.png / hit_fx.png, 8x7
// HitFx grid, 50/50 hold bands, hitFxScale).
struct PackInfo {
    std::string notesDir;
    std::string tap = "Tap.png", tapMH = "TapMH.png";
    std::string drag = "Drag.png", dragMH = "DragMH.png";
    std::string flick = "Flick.png", flickMH = "FlickMH.png";
    std::string hold = "Hold.png", holdMH = "HoldMH.png";
    std::string hitFx = "HitFx.png";
    int hitFxCols = 5, hitFxRows = 6;
    int holdHead = 50, holdTail = 50;
    int holdHeadMH = 100, holdTailMH = 100;
    float hitFxScale = 1.0f;
    std::string name, author;
    bool custom = false;
};

class Renderer {
public:
    enum TexIndex {
        TEX_BG = 0,
        TEX_WHITE,
        TEX_TAP_A,
        TEX_TAP_B,
        TEX_DRAG_A,
        TEX_DRAG_B,
        TEX_FLICK_A,
        TEX_FLICK_B,
        TEX_HOLD_HEAD_A,
        TEX_HOLD_HEAD_B,
        TEX_HOLD_BODY_A,
        TEX_HOLD_BODY_B,
        TEX_HOLD_TAIL_A,
        TEX_HOLD_TAIL_B,
        TEX_HITFX,
        TEX_LINE,
        TEX_HUD,
        TEX_COUNT
    };

    bool init(const std::string& assetDir, const std::string& chartPath,
              const std::string& backgroundPath, std::string* error);
    bool resize(int w, int h, std::string* error);
    void destroy();

    // Optional post-process add-on (see addon.h).  Off by default: when no
    // add-on is loaded the frame path is unchanged.
    bool enableAddon(const std::string& idOrPath, std::string* error);
    // Loads every add-on under <chartdir>/addons (each sub-folder is one add-on)
    // and applies the parameter overrides from <chartdir>/addons/config.json.
    // Missing folder or config is fine.  Returns how many were loaded.
    int loadChartAddons(const std::string& chartPath, std::string* error);
    addon::Addon* lastAddon() { return addons_.empty() ? nullptr : &addons_.back(); }
    size_t addonCount() const { return addons_.size(); }

    // Renders one frame at `time` seconds into the scene target.
    void render(float time);

    bool savePng(const std::string& path) const;
    bool readPixels(std::vector<unsigned char>& rgba) const;  // top-down RGBA8
    bool readPixelsRaw(std::vector<unsigned char>& rgba) const;  // bottom-up RGBA8 (GL order)
    // Same pixels, but through a pixel-pack buffer: startReadback() queues the
    // GPU copy and returns, collectReadback() waits for that copy and pulls it
    // into host memory. Two slots let the next frame render while one transfers.
    void startReadback(int slot);
    bool collectReadback(int slot, unsigned char* dst, size_t bytes);
    // Same handshake for a YUV420p pipe: the scene is converted into three R8
    // planes on the GPU (BT.601 studio range, rows already flipped), so the pipe
    // carries w*h*3/2 bytes instead of w*h*4 and ffmpeg needs no -vf vflip.
    void startReadbackYuv(int slot);
    bool collectReadbackYuv(int slot, unsigned char* dst, size_t bytes);

    const Chart& chart() const { return chart_; }
    // Chart playback view settings (all official defaults).
    struct ViewSettings {
        float flowSpeed = 1.0f;    // official config.flow_speed: scales note scroll speed only
        float bgBaseDim = 0.30f;   // official full-screen black layer (prpr scene.rs)
        float bgDim = 0.60f;       // official background_dim (prpr config.rs)
        float bgBlur = 10.0f;       // extra blur radius on the background texture, in source texels
        float fxScale = 1.45f;     // hit-fx size over the official 0.2 chart-unit base
        float noteScale = 1.0f;    // global note-size multiplier
        int mhMode = 0;            // multi-press hint art: 0 = chart, 1 = always on, 2 = never
        bool mirror = false;       // mirror the chart left/right (practice layouts)
        bool whiteLine = false;    // judge line + hit-fx colour: gold (default) or white
    };
    void setFrameRate(float fps) { frameRate_ = fps; }
    void setHudEnabled(bool enabled) { hudEnabled_ = enabled; }
    void setHudFonts(const std::string& gameplayPath, const std::string& gameplayFamily,
                     const std::string& infoPath, const std::string& infoFamily) {
        hud_.setFonts(gameplayPath, gameplayFamily, infoPath, infoFamily);
    }
    ViewSettings& viewSettings() { return view_; }
    const ViewSettings& viewSettings() const { return view_; }

    int width() const { return w_; }
    int height() const { return h_; }

    // Debug counters for the last rendered frame.
    int dbgTapQuads = 0, dbgHoldQuads = 0, dbgLines = 0;
    int dbgZonesA = 0, dbgZonesD = 0, dbgZonesR = 0;

    // Extra noise areas contributed by add-ons: they go through the real
    // NoiseField pipeline, so they occlude notes like chart block areas.
    void addNoiseBlocks(const std::vector<addon::BlockSpec>& specs) {
        for (const auto& s : specs) noiseBlocks_.push_back(s);
    }

    // Applies the chart side of an add-on: extra notes / judge lines and the
    // time ranges where the chart's own notes or lines are hidden.
    void applyAddonChart(const addon::Addon& a);

    void setHideLayers(bool lines, bool notes, bool noise) { hideLines_ = lines; hideNotes_ = notes; hideNoise_ = noise; }

    // Judge line / hit-effect colour: official gold, or plain white.
    Vec4 lineColor() const;

    struct HitEffect {
        float time = 0.0f;
        Vec2 pos;
        float rot = 0.0f;
        float size = 1.0f;  // note.size: the flash is scaled to the note it hits
        int type = 1;       // note type: taps/flicks flash white, drags/holds gold
        std::array<Vec2,4> particles; // angle (radians), radius (reference pixels)
    };
    float duration() const { return duration_; }

    bool debugNoiseDump(const std::string& prefix, std::string* error) {
        return noise_.debugDump(prefix, error);
    }

private:
    struct Sprite {
        gpu::Texture tex;
        gpu::QuadBatch batch;
    };

    std::array<Sprite, TEX_COUNT> sprites_;
    gpu::Shader shSprite_;
    gpu::Shader shAddSprite_;
    gpu::RenderTarget scene_;
    Chart chart_;
    noise::NoiseField noise_;
    hud::Overlay hud_;

    int w_ = 0, h_ = 0;
    static const int kReadbackSlots = 4;
    GLuint pbo_[kReadbackSlots] = {};
    int pboW_ = 0, pboH_ = 0;
    gpu::FullscreenPass yuvPass_;
    gpu::RenderTarget yuvY_, yuvUV_;
    GLuint pboYuv_[kReadbackSlots][2] = {};
    int yuvW_ = 0, yuvH_ = 0;
    float aspect_ = 1.7777778f;
    Mat4 proj_;
    bool ready_ = false;
    // Cached debug switches (env lookups are too slow for the frame loop).
    bool debugClearOnly_ = false;
    bool debugHolds_ = false;
    bool noiseInited_ = false;
    std::string assetDir_;
    PackInfo packInfo_;
    std::vector<HitEffect> hitEffects_;
    std::vector<addon::BlockSpec> noiseBlocks_;
    std::vector<std::pair<float, float>> hideNoteRanges_;
    std::vector<std::pair<float, float>> hideLineRanges_;
    struct NoteHit { float time, x, y; };
    std::vector<NoteHit> noteHits_;  // sorted by time: add-on combo/hit uniforms
    float frameRate_ = 60.0f;
    int zoneCount_ = 0;
    float duration_ = 0.0f;
    bool hideLines_ = false, hideNotes_ = false, hideNoise_ = false;
    bool hudEnabled_ = false;
    std::vector<addon::Addon> addons_;
    int frameIndex_ = 0;
    ViewSettings view_;

    struct LineState {
        float beat = 0;
        float fp = 0;
        float cx = 0, cy = 0;
        float cosR = 1, sinR = 0;
        float rotDeg = 0;
    };

    bool loadSprite(int tex, const std::string& path, std::string* error);
    void resolvePack(const std::string& assetDir);

    void beginBatches();
    void pushSprite(int tex, const gpu::QuadBatch::Instance& inst);
    void flush(int tex, bool additive);

    void buildLineState(const JudgeLine& line, float time, LineState& out) const;
    void drawBackgroundLayer(float time);
    void loadBackground(const std::string& path, float radius);
    void drawJudgeLines(float time);
    void drawHoldNotes(float time);
    void drawTapNotes(float time);
    void drawEffects(float time);
    void drawHud(float time);
    void buildHitEffects();
    void buildNoteTimes();
    void loadDuration(const std::string& chartPath);
    void buildZones(float time, std::vector<noise::Zone>& zones);
};
