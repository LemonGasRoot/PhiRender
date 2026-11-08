#include <stdexcept>
// renderer.cpp - Phigros chart scene renderer.
#include "renderer.h"
#include "hitfx.h"

#include "jsonarena.h"
#include <cstdio>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "cJSON.h"

#include "stb_image.h"
#include "stb_image_write.h"

namespace {

const char* kSpriteFS = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUv);
    fragColor = c * vColor;
}
)";

const char* kAddSpriteFS = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUv);
    float k = c.a * vColor.a;
    fragColor = vec4(vColor.rgb * k, 1.0);
}
)";

// RGB -> YUV420p (BT.601, studio range) for the encoder pipe.  Three passes write
// one plane each: Y at full resolution, U and V at half resolution (a 2x2 box of
// scene texels per chroma sample, exactly what ffmpeg's swscale does for
// `-pix_fmt yuv420p`).  The fixed-point coefficients and the 16..235 / 16..240
// clamping match libswscale, so the encoder receives the same bytes it used to
// compute itself - only 3.11 MB/frame travel instead of 8.29 MB, and the rows are
// flipped here so ffmpeg no longer has to run `-vf vflip`.
const char* kYuvFS = R"(#version 330 core
out vec4 fragColor;
uniform sampler2D uTex;
uniform ivec2 uSize;
uniform int uPlane;
vec3 rgbAt(ivec2 p) { return texelFetch(uTex, p, 0).rgb * 255.0; }
void main() {
    ivec2 d = ivec2(gl_FragCoord.xy);
    vec3 c;
    if (uPlane == 0) {
        c = rgbAt(ivec2(d.x, uSize.y - 1 - d.y));
    } else {
        ivec2 s = ivec2(d.x * 2, uSize.y - 2 - d.y * 2);
        c = (rgbAt(s) + rgbAt(s + ivec2(1, 0)) + rgbAt(s + ivec2(0, 1)) + rgbAt(s + ivec2(1, 1))) * 0.25;
    }
    if (uPlane == 0) {
        float y = (8414.0 * c.r + 16519.0 * c.g + 3208.0 * c.b + 524288.0 + 16384.0) / 32768.0;
        fragColor = vec4(clamp(y, 16.0, 235.0) / 255.0, 0.0, 0.0, 1.0);
    } else {
        // nv12: both chroma samples live in one texel, so the pipe stays one
        // buffer per plane.
        float u = (-4868.0 * c.r - 9535.0 * c.g + 14392.0 * c.b + 4194304.0 + 16384.0) / 32768.0;
        float v = (14392.0 * c.r - 12051.0 * c.g - 2341.0 * c.b + 4194304.0 + 16384.0) / 32768.0;
        fragColor = vec4(clamp(u, 16.0, 240.0) / 255.0, clamp(v, 16.0, 240.0) / 255.0, 0.0, 1.0);
    }
}
)";

const char* kSpriteVS = nullptr;  // set to gpu::quadVertexShader()

// hint == true selects the "MH" (multi-press hint) art of the official pack.
int headTex(int type, bool hint) {
    switch (type) {
    case 1: return hint ? Renderer::TEX_TAP_B : Renderer::TEX_TAP_A;
    case 2: return hint ? Renderer::TEX_DRAG_B : Renderer::TEX_DRAG_A;
    case 3: return hint ? Renderer::TEX_HOLD_HEAD_B : Renderer::TEX_HOLD_HEAD_A;
    case 4: return hint ? Renderer::TEX_FLICK_B : Renderer::TEX_FLICK_A;
    default: return hint ? Renderer::TEX_TAP_B : Renderer::TEX_TAP_A;
    }
}

}  // namespace

bool Renderer::loadSprite(int tex, const std::string& path, std::string* error) {
    if (!sprites_[tex].tex.loadFile(path, true, false)) {
        if (error) *error = "cannot load texture: " + path;
        return false;
    }
    return true;
}

namespace {
bool fileExists(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    return f.good();
}
std::string readAllText(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
int parseIntAt(const std::string& s, size_t& i, int fallback) {
    while (i < s.size() && !std::isdigit((unsigned char)s[i]) && s[i] != '-') ++i;
    if (i >= s.size()) return fallback;
    char* end = nullptr;
    long v = std::strtol(s.c_str() + (int)i, &end, 10);
    if (end == s.c_str() + (int)i) return fallback;
    i = (size_t)(end - s.c_str());
    return (int)v;
}
std::string trimStr(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
std::string unquoteStr(const std::string& s) {
    size_t a = s.find('"');
    size_t b = s.rfind('"');
    if (a != std::string::npos && b != std::string::npos && b > a) return s.substr(a + 1, b - a - 1);
    return trimStr(s);
}
}  // namespace

// Chooses the note/fx texture layout.  Default = PhiVideo official-style pack
// (Notes/*.png + UI.json slicing).  A Phira-Pro style respack/ sub-directory
// (click.png/hold.png/hit_fx.png + info.yml) overrides it, which is how the
// renderer fuses both ecosystems' resource packs.
void Renderer::resolvePack(const std::string& assetDir) {
    PackInfo p;
    p.notesDir = assetDir + "/Notes";

    std::string ui = readAllText(assetDir + "/UI.json");
    if (!ui.empty()) {
        cJSON* root = cJSON_Parse(ui.c_str());
        if (root) {
            auto pair = [&](const char* key, int& a, int& b) {
                cJSON* v = cJSON_GetObjectItemCaseSensitive(root, key);
                if (v && cJSON_IsArray(v) && cJSON_GetArraySize(v) >= 2) {
                    cJSON* x = cJSON_GetArrayItem(v, 0);
                    cJSON* y = cJSON_GetArrayItem(v, 1);
                    if (x && cJSON_IsNumber(x)) a = (int)x->valuedouble;
                    if (y && cJSON_IsNumber(y)) b = (int)y->valuedouble;
                }
            };
            pair("HitFx", p.hitFxCols, p.hitFxRows);
            pair("HoldAtlas", p.holdTail, p.holdHead);
            pair("HoldAtlasMH", p.holdTailMH, p.holdHeadMH);
            cJSON_Delete(root);
        }
    }

    std::string rp = assetDir + "/respack";
    std::string yml = readAllText(rp + "/info.yml");
    bool hasRespack = !yml.empty() && fileExists(rp + "/click.png") && fileExists(rp + "/hold.png") &&
                      fileExists(rp + "/hit_fx.png");
    if (hasRespack) {
        p.notesDir = rp;
        p.tap = "click.png";
        p.tapMH = "click_mh.png";
        p.drag = "drag.png";
        p.dragMH = "drag_mh.png";
        p.flick = "flick.png";
        p.flickMH = "flick_mh.png";
        p.hold = "hold.png";
        p.holdMH = "hold_mh.png";
        p.hitFx = "hit_fx.png";
        p.custom = true;
        std::istringstream in(yml);
        std::string line;
        while (std::getline(in, line)) {
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = trimStr(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            auto readPair = [&](int& x, int& y) {
                size_t i = 0;
                int a = parseIntAt(val, i, x);
                int b = parseIntAt(val, i, y);
                x = a;
                y = b;
            };
            if (key == "hitFx") readPair(p.hitFxCols, p.hitFxRows);
            else if (key == "holdAtlas") readPair(p.holdTail, p.holdHead);
            else if (key == "holdAtlasMH") readPair(p.holdTailMH, p.holdHeadMH);
            else if (key == "hitFxScale") p.hitFxScale = (float)std::atof(unquoteStr(val).c_str());
            else if (key == "name") p.name = unquoteStr(val);
            else if (key == "author") p.author = unquoteStr(val);
        }
    }
    packInfo_ = p;
    if (p.custom) {
        std::printf("resource pack: Phira-Pro respack (%s by %s), hitFx %dx%d, holdAtlas %d/%d, scale %.2f\n",
                    p.name.c_str(), p.author.c_str(), p.hitFxCols, p.hitFxRows, p.holdHead, p.holdTail,
                    p.hitFxScale);
    }
}

bool Renderer::init(const std::string& assetDir, const std::string& chartPath,
                    const std::string& backgroundPath, std::string* error) {
    assetDir_ = assetDir;
    jsonarena::install();
    if (!chart_.loadChart(chartPath, error)) return false;

    resolvePack(assetDir);
    const std::string notes = packInfo_.notesDir + "/";
    struct SpriteFile {
        int tex;
        const std::string* path;
    };
    const SpriteFile files[] = {
        {TEX_TAP_A, &packInfo_.tap},     {TEX_TAP_B, &packInfo_.tapMH},
        {TEX_DRAG_A, &packInfo_.drag},   {TEX_DRAG_B, &packInfo_.dragMH},
        {TEX_FLICK_A, &packInfo_.flick}, {TEX_FLICK_B, &packInfo_.flickMH},
    };
    for (const auto& f : files) {
        if (!loadSprite(f.tex, notes + *f.path, error)) return false;
    }

    // Hold textures: vertical atlas with head/body/tail bands (HoldAtlas /
    // HoldAtlasMH from UI.json, or info.yml for a Phira-Pro respack).
    // Official atlas layout (prpr/core/resource.rs): the *head* cap is the
    // bottom band of the texture, the *tail* cap is the top band.
    // Each atlas is cut into three bands, so decode it once instead of once per
    // band: the two hold atlases are ~1.2 MB PNGs and decoding is the slow part.
    struct Decoded {
        int w = 0, h = 0;
        std::vector<unsigned char> px;
    };
    auto decodePng = [](const std::string& path, Decoded* out) {
        int iw = 0, ih = 0, comp = 0;
        unsigned char* data = stbi_load(path.c_str(), &iw, &ih, &comp, 4);
        if (!data) return false;
        out->w = iw;
        out->h = ih;
        out->px.assign(data, data + (size_t)iw * (size_t)ih * 4u);
        stbi_image_free(data);
        return true;
    };
    auto slice = [&](int tex, const Decoded& src, int y0, int y1, const std::string& path) {
        if (y0 < 0) y0 = src.h + y0;  // negative head: offset from the bottom edge
        if (y1 < 0) y1 = src.h + y1;  // negative tail: offset from the bottom edge
        if (y0 < 0) y0 = 0;
        if (y1 > src.h) y1 = src.h;
        if (y1 <= y0) {
            if (error) *error = "bad slice range for " + path;
            return false;
        }
        const size_t stride = (size_t)src.w * 4u;
        const int h = y1 - y0;
        std::vector<unsigned char> sub(stride * (size_t)h);
        for (int y = 0; y < h; ++y)
            std::memcpy(sub.data() + (size_t)y * stride, src.px.data() + (size_t)(y0 + y) * stride,
                        stride);
        sprites_[tex].tex.create(src.w, h, GL_RGBA8, sub.data(), GL_RGBA, GL_UNSIGNED_BYTE, true,
                                 false);
        return true;
    };

    Decoded holdA, holdB;
    if (!decodePng(notes + packInfo_.hold, &holdA)) {
        if (error) *error = "cannot load texture: " + notes + packInfo_.hold;
        return false;
    }
    if (!decodePng(notes + packInfo_.holdMH, &holdB)) {
        if (error) *error = "cannot load texture: " + notes + packInfo_.holdMH;
        return false;
    }
    if (!slice(TEX_HOLD_TAIL_A, holdA, 0, packInfo_.holdTail, notes + packInfo_.hold)) return false;
    if (!slice(TEX_HOLD_BODY_A, holdA, packInfo_.holdTail, -packInfo_.holdHead, notes + packInfo_.hold))
        return false;
    if (!slice(TEX_HOLD_HEAD_A, holdA, -packInfo_.holdHead, 0x7FFFFFFF, notes + packInfo_.hold))
        return false;
    if (!slice(TEX_HOLD_TAIL_B, holdB, 0, packInfo_.holdTailMH, notes + packInfo_.holdMH)) return false;
    if (!slice(TEX_HOLD_BODY_B, holdB, packInfo_.holdTailMH, -packInfo_.holdHeadMH,
               notes + packInfo_.holdMH))
        return false;
    if (!slice(TEX_HOLD_HEAD_B, holdB, -packInfo_.holdHeadMH, 0x7FFFFFFF, notes + packInfo_.holdMH))
        return false;

    if (!loadSprite(TEX_HITFX, notes + packInfo_.hitFx, error)) return false;

    // falls back to drawn shapes when a sprite is missing.
    loadSprite(TEX_LINE, assetDir + "/hud/JudgeLine.png", nullptr);

    if (!backgroundPath.empty()) {
        // Optional; a missing background is not fatal.  The game's blur texture is
        // tiny (256x135) and gets bilinearly stretched to the screen; running a
        // box blur over the source first removes the linear-interpolation facets
        // and lands closer to the game's soft backdrop.
        loadBackground(backgroundPath, view_.bgBlur);
    }

    // 1x1 white texture for solid quads / lines.
    {
        unsigned char px[4] = {255, 255, 255, 255};
        sprites_[TEX_WHITE].tex.create(1, 1, GL_RGBA8, px, GL_RGBA, GL_UNSIGNED_BYTE, true, false);
    }

    if (!shSprite_.build(gpu::quadVertexShader(), kSpriteFS, "sprite", error)) return false;
    if (!shAddSprite_.build(gpu::quadVertexShader(), kAddSpriteFS, "spriteAdd", error)) return false;
    if (!yuvPass_.build(gpu::fullscreenVertexShader(), kYuvFS, "nv12", error)) return false;

    for (auto& s : sprites_) s.batch.init(8192);

    debugClearOnly_ = std::getenv("PHI_CLEAR_ONLY") != nullptr;
    debugHolds_ = std::getenv("PHI_DEBUG_HOLDS") != nullptr;

    chart_.finalize(0.0f);
    loadDuration(chartPath);
    return true;
}

bool Renderer::resize(int w, int h, std::string* error) {
    if (w <= 0 || h <= 0) {
        if (error) *error = "invalid render size";
        return false;
    }
    w_ = w;
    h_ = h;
    aspect_ = (float)w / (float)h;
    proj_ = Mat4::ortho(0.0f, (float)w, 0.0f, (float)h, true);

    scene_.create(w, h, GL_RGBA8, false);
    if (!scene_.fbo) {
        if (error) *error = "cannot create scene render target";
        return false;
    }

    // Planes for the nv12 pipe (see kYuvFS): luma is full size, the two chroma
    // samples of a 2x2 block are packed into one RG texel.
    const int cw = std::max(1, w / 2), ch = std::max(1, h / 2);
    yuvY_.create(w, h, GL_R8, false);
    yuvUV_.create(cw, ch, GL_RG, false);
    if (!yuvY_.fbo || !yuvUV_.fbo) {
        if (error) *error = "cannot create nv12 planes";
        return false;
    }

    if (!noiseInited_) {
        if (!noise_.init(assetDir_, w, h, aspect_, error)) return false;
        noiseInited_ = true;
    } else if (!noise_.resize(w, h, aspect_, error)) {
        return false;
    }
    if (hudEnabled_) {
        if (!hud_.resize(w, h, error)) return false;
        sprites_[TEX_HUD].tex.create(w, h, GL_RGBA8, nullptr, GL_RGBA, GL_UNSIGNED_BYTE, true, false);
    }
    buildHitEffects();
    buildNoteTimes();
    ready_ = true;
    return true;
}

bool Renderer::enableAddon(const std::string& idOrPath, std::string* error) {
    const std::string dir = addon::resolveDir(idOrPath, assetDir_);
    addons_.emplace_back();
    if (!addons_.back().load(dir, error)) {
        addons_.pop_back();
        return false;
    }
    addNoiseBlocks(addons_.back().blocks);
    applyAddonChart(addons_.back());
    return true;
}

bool inAnyRange(const std::vector<std::pair<float, float>>& ranges, float t) {
    for (const auto& r : ranges) {
        if (t >= r.first && t < r.second) return true;
    }
    return false;
}

void Renderer::applyAddonChart(const addon::Addon& a) {
    if (a.hideNotes.empty() && a.hideLines.empty() && a.notes.empty() && a.lines.empty()) return;
    hideNoteRanges_.insert(hideNoteRanges_.end(), a.hideNotes.begin(), a.hideNotes.end());
    hideLineRanges_.insert(hideLineRanges_.end(), a.hideLines.begin(), a.hideLines.end());

    // Hidden notes leave the chart model itself: the mixer reads the same chart,
    // so a note hidden by an add-on must not produce a hit sound either.
    if (!a.hideNotes.empty()) {
        size_t kept = 0;
        for (auto& line : chart_.lines) {
            for (std::vector<Note>* v : {&line.notesAbove, &line.notesBelow}) {
                v->erase(std::remove_if(v->begin(), v->end(),
                                        [&](const Note& n) {
                                            return inAnyRange(a.hideNotes, n.secTime);
                                        }),
                         v->end());
                kept += v->size();
            }
        }
        chart_.noteCount = (int)kept;
    }

    const float bpm = chart_.lines.empty() ? 120.0f : chart_.lines.front().bpm;
    // Judge line events are sampled in beats, not seconds.
    const float spb = PGRBEAT / (bpm > 1e-3f ? bpm : 120.0f);
    // Extra static lines: one event spanning "forever" so the existing sampling
    // code returns a constant transform; alpha gates the visible window.
    for (const auto& ls : a.lines) {
        if (!std::isfinite(ls.appear) || !std::isfinite(ls.disappear) ||
            !std::isfinite(ls.x) || !std::isfinite(ls.y) || !std::isfinite(ls.rot)) {
            std::fprintf(stderr, "addon '%s': judge line with non-finite fields skipped\n",
                         a.id.c_str());
            continue;
        }
        JudgeLine line;
        line.bpm = bpm;
        const float bAppear = ls.appear / spb;
        const float bDisappear = ls.disappear / spb;
        line.disappearEvents.push_back(Event{0.0f, bAppear, 0.0f, 0.0f});
        line.disappearEvents.push_back(Event{bAppear, bDisappear, 1.0f, 1.0f});
        line.disappearEvents.push_back(Event{bDisappear, 1e9f, 0.0f, 0.0f});
        line.moveEvents.push_back(MoveEvent{0.0f, 1e9f, ls.x, ls.x, ls.y, ls.y});
        line.rotateEvents.push_back(Event{0.0f, 1e9f, ls.rot, ls.rot});
        line.speedEvents.push_back(SpeedEvent{0.0f, 1e9f, 1.0f, 0.0f});
        chart_.lines.push_back(line);
    }

    // Extra notes are appended to the chart model, so scrolling, hold bodies and
    // hit effects all work exactly as for real notes.
    if (!chart_.lines.empty()) {
        for (const auto& ns : a.notes) {
            if (ns.line < 0 || ns.line >= (int)chart_.lines.size()) {
                std::fprintf(stderr, "addon '%s': note on missing judge line %d skipped\n",
                             a.id.c_str(), ns.line);
                continue;
            }
            if (!std::isfinite(ns.time) || ns.time < 0.0f || !std::isfinite(ns.x) ||
                !std::isfinite(ns.size) || ns.size <= 0.0f || ns.type < 1 || ns.type > 4) {
                std::fprintf(stderr, "addon '%s': note with invalid fields skipped\n",
                             a.id.c_str());
                continue;
            }
            JudgeLine& line = chart_.lines[(size_t)ns.line];
            Note n;
            n.type = ns.type;
            n.secTime = ns.time;
            n.secHoldEndTime = ns.time;
            n.positionX = ns.x;
            n.size = ns.size;
            n.speed = 1.0f;
            n.above = true;
            const float beat = line.sec2beat(ns.time, chart_.offset);
            n.time = beat;
            n.floorPosition = line.fpAt(beat);
            line.notesAbove.push_back(n);
        }
        chart_.markMultipleHints();
    }
    {
        size_t total = 0;
        for (const auto& line : chart_.lines) total += line.notesAbove.size() + line.notesBelow.size();
        chart_.noteCount = (int)total;
    }
    buildHitEffects();
    buildNoteTimes();
}

namespace {
}

int Renderer::loadChartAddons(const std::string& chartPath, std::string* error) {
    std::string dir = chartPath;
    const size_t slash = dir.find_last_of("/\\");
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    const std::string root = dir + "/addons";
    const std::vector<std::string> ids = addon::list(root);
    if (ids.empty()) return 0;

    std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>> cfg;
    std::string cfgErr;
    if (!addon::readChartConfig(root + "/config.json", &cfg, &cfgErr)) {
        // A broken config must not take the render down: report and carry on with
        // the manifest defaults.
        std::fprintf(stderr, "chart addons: %s (using manifest defaults)\n", cfgErr.c_str());
    }

    int loaded = 0;
    for (const std::string& id : ids) {
        const std::string path = root + "/" + id;
        addons_.emplace_back();
        std::string err;
        if (!addons_.back().load(path, &err)) {
            addons_.pop_back();
            std::fprintf(stderr, "chart addon '%s' skipped: %s\n", id.c_str(), err.c_str());
            continue;
        }
        addons_.back().fromChart = true;
        for (const auto& entry : cfg) {
            if (entry.first != id) continue;
            // Chart parameters are authoritative: the command line cannot change
            // an add-on that ships with the chart.
            for (const auto& kv : entry.second) addons_.back().setParam(kv.first, kv.second);
        }
        addNoiseBlocks(addons_.back().blocks);
        applyAddonChart(addons_.back());
        std::printf("chart addon: %s (%s %s)\n", addons_.back().name.c_str(),
                    addons_.back().author.c_str(), addons_.back().version.c_str());
        ++loaded;
    }
    if (error) error->clear();
    return loaded;
}

void Renderer::destroy() {
    ready_ = false;
    hud_.destroy();
    scene_.destroy();
    yuvY_.destroy();
    yuvUV_.destroy();
    yuvPass_.shader.destroy();
    for (int i = 0; i < kReadbackSlots; ++i) {
        if (pbo_[i]) glDeleteBuffers(1, &pbo_[i]);
        pbo_[i] = 0;
        for (int k = 0; k < 2; ++k) {
            if (pboYuv_[i][k]) glDeleteBuffers(1, &pboYuv_[i][k]);
            pboYuv_[i][k] = 0;
        }
    }
    for (auto& s : sprites_) {
        s.tex.destroy();
        s.batch.destroy();
    }
    shSprite_.destroy();
    noise_.destroy();
}

void Renderer::beginBatches() {
    for (auto& s : sprites_) s.batch.begin();
}

void Renderer::pushSprite(int tex, const gpu::QuadBatch::Instance& inst) {
    sprites_[tex].batch.push(inst);
}

void Renderer::flush(int tex, bool additive) {
    Sprite& s = sprites_[tex];
    if (s.batch.count <= 0) return;
    s.batch.end();
    // Never trust state left behind by earlier passes (fullscreen noise passes
    // disable blending and may mask color writes).
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (additive) {
        shAddSprite_.use();
        shAddSprite_.setMat4("uProj", proj_);
        shAddSprite_.setTex("uTex", 0, s.tex.id);
        gpu::setBlendAdditive();
    } else {
        shSprite_.use();
        shSprite_.setMat4("uProj", proj_);
        shSprite_.setTex("uTex", 0, s.tex.id);
        gpu::setBlendAlpha();
    }
    gpu::drawQuads(s.batch);
    s.batch.begin();
}

void Renderer::buildLineState(const JudgeLine& line, float time, LineState& out) const {
    float beat = line.sec2beat(time, chart_.offset);
    out.beat = beat;
    out.fp = line.fpAt(beat);
    float x = line.moveX(beat);
    float y = line.moveY(beat);
    out.cx = x * (float)w_;
    out.cy = (1.0f - y) * (float)h_;
    out.rotDeg = line.rotate(beat);
    if (view_.mirror) {
        // Mirroring the plane maps a line at screen x to W - x and its direction
        // angle to -angle; notes keep their chart-space offsets because the
        // offset is measured along the (already mirrored) line direction.
        out.cx = (float)w_ - out.cx;
        out.rotDeg = -out.rotDeg;
    }
    float rad = out.rotDeg * PI_OVER_180;
    out.cosR = std::cos(rad);
    out.sinR = std::sin(rad);
}

// Collects the block areas visible at `time` into chart-space zones.  The touch
// (red contact point) pipeline was removed: it only reacts to real input, which
// an autoplay render never produces.
void Renderer::buildZones(float time, std::vector<noise::Zone>& zones) {
    zones.clear();
    dbgZonesA = dbgZonesD = dbgZonesR = 0;
    for (const auto& b : chart_.blocks) {
        auto phase = b.phase(time);
        if (phase == BlockArea::Phase::Hidden) continue;
        int state = (phase == BlockArea::Phase::Active) ? 0
                    : (phase == BlockArea::Phase::Ready ? 2 : 1);
        auto tr = b.transform(time, aspect_);
        if (view_.mirror) { tr.center.x = -tr.center.x; tr.rotation = -tr.rotation; }
        noise::Zone z = noise::makeZone(tr, b.isSubtract, state);
        // Official 噪域 appear flash: the area whitens the instant it becomes
        // visible (start of its Ready window) and falls back quickly.  Using
        // the activation start here would flash the area long after it showed
        // up, which is the timing bug this replaces.
        const float born = b.phaseStart(time);
        const float dt = time - born;
        if (dt >= 0.0f && dt < noise_.settingsRef().flashTime) {
            const float f = 1.0f - dt / noise_.settingsRef().flashTime;
            z.flash = f * f;
        }
        zones.push_back(z);
        if (state == 0) ++dbgZonesA; else if (state == 1) ++dbgZonesD; else ++dbgZonesR;
    }

    // Add-on noise areas: same pipeline, same flash, just sourced from the
    // add-on manifest instead of the chart.  This is what lets a plug-in create
    // a real "unsolvable" stretch where the noise covers the judgement area.
    for (const auto& b : noiseBlocks_) {
        if (time < b.appear || time >= b.disappear) continue;
        int state = 1;                                    // disabled
        if (time >= b.enable && time < b.disable) state = 0;   // active
        else if (time >= b.enable - 0.5f && time < b.enable) state = 2;  // ready
        BlockArea::Transform tr;
        tr.center = Vec2(b.x, b.y);
        tr.size = Vec2(b.w, b.h);
        tr.rotation = b.rot;
        tr.yScale = 1.0f;
        if (view_.mirror) { tr.center.x = -tr.center.x; tr.rotation = -tr.rotation; }
        noise::Zone z = noise::makeZone(tr, b.subtract, state);
        const float dt = time - b.appear;
        if (dt >= 0.0f && dt < noise_.settingsRef().flashTime) {
            const float f = 1.0f - dt / noise_.settingsRef().flashTime;
            z.flash = f * f;
        }
        zones.push_back(z);
        if (state == 0) ++dbgZonesA; else if (state == 1) ++dbgZonesD; else ++dbgZonesR;
    }
}

void Renderer::drawBackgroundLayer(float time) {
    (void)time;
    Sprite& bg = sprites_[TEX_BG];
    if (bg.tex.valid()) {
        float iw = (float)bg.tex.width;
        float ih = (float)bg.tex.height;
        if (iw > 0 && ih > 0) {
            float imgAspect = iw / ih;
            float screenAspect = (float)w_ / (float)h_;
            float qw, qh;
            if (imgAspect > screenAspect) {
                qh = (float)h_;
                qw = (float)h_ * imgAspect;
            } else {
                qw = (float)w_;
                qh = (float)w_ / imgAspect;
            }
            pushSprite(TEX_BG, gpu::makeSprite(Vec2(w_ * 0.5f, h_ * 0.5f), qw, qh, 0.0f));
        }
    }
    flush(TEX_BG, false);

    // Official chart background dim: full-screen black (prpr scene.rs: dim 0.3)
    // plus the user-configurable background_dim (official default 0.60), which
    // together leave ~28% of the illustration visible.
    {
        pushSprite(TEX_WHITE, gpu::makeSprite(Vec2(w_ * 0.5f, h_ * 0.5f), (float)w_, (float)h_, 0.0f,
                                              Vec2(0, 0), Vec2(1, 1), Vec4(0, 0, 0, view_.bgBaseDim)));
        pushSprite(TEX_WHITE, gpu::makeSprite(Vec2(w_ * 0.5f, h_ * 0.5f), (float)w_, (float)h_, 0.0f,
                                              Vec2(0, 0), Vec2(1, 1), Vec4(0, 0, 0, view_.bgDim)));
    }
    flush(TEX_WHITE, false);
}

// Load the background, optionally blurring the source texels (three box passes
// approximate a gaussian).  `radius` is in source texels.
void Renderer::loadBackground(const std::string& path, float radius) {
    int iw = 0, ih = 0, comp = 0;
    unsigned char* data = stbi_load(path.c_str(), &iw, &ih, &comp, 4);
    if (!data) {
        sprites_[TEX_BG].tex.loadFile(path, true, false);
        return;
    }
    // The background is always blurred automatically: charts only ship the sharp
    // illustration, and the game's own backdrop is a 256x135 texture stretched
    // over the screen.  There is no "is this already a *Blur file?" probe and no
    // unblurred fallback; only an explicit --bg-blur 0 turns it off.
    const float autoRadius = (float)iw / 40.0f;
    const float effective = radius <= 0.0f ? 0.0f : std::max(radius, autoRadius);
    const int r = (int)(effective + 0.5f);
    if (r > 0 && iw > 2 * r && ih > 2 * r) {
        std::vector<unsigned char> tmp((size_t)iw * ih * 4), acc((size_t)iw * ih * 4);
        std::memcpy(acc.data(), data, (size_t)iw * ih * 4);
        for (int pass = 0; pass < 3; ++pass) {
            // horizontal
            for (int y = 0; y < ih; ++y) {
                const unsigned char* row = acc.data() + (size_t)y * iw * 4;
                unsigned char* out = tmp.data() + (size_t)y * iw * 4;
                for (int x = 0; x < iw; ++x) {
                    const int x0 = x - r < 0 ? 0 : x - r;
                    const int x1 = x + r >= iw ? iw - 1 : x + r;
                    unsigned int sum[4] = {0, 0, 0, 0};
                    for (int k = x0; k <= x1; ++k)
                        for (int c = 0; c < 4; ++c) sum[c] += row[(size_t)k * 4 + c];
                    const unsigned int n = (unsigned int)(x1 - x0 + 1);
                    for (int c = 0; c < 4; ++c) out[(size_t)x * 4 + c] = (unsigned char)(sum[c] / n);
                }
            }
            // vertical
            for (int x = 0; x < iw; ++x) {
                for (int y = 0; y < ih; ++y) {
                    const int y0 = y - r < 0 ? 0 : y - r;
                    const int y1 = y + r >= ih ? ih - 1 : y + r;
                    unsigned int sum[4] = {0, 0, 0, 0};
                    for (int k = y0; k <= y1; ++k)
                        for (int c = 0; c < 4; ++c) sum[c] += tmp[((size_t)k * iw + x) * 4 + c];
                    const unsigned int n = (unsigned int)(y1 - y0 + 1);
                    for (int c = 0; c < 4; ++c)
                        acc[((size_t)y * iw + x) * 4 + c] = (unsigned char)(sum[c] / n);
                }
            }
        }
        sprites_[TEX_BG].tex.create(iw, ih, GL_RGBA8, acc.data(), GL_RGBA, GL_UNSIGNED_BYTE, true,
                                    false);
    } else {
        sprites_[TEX_BG].tex.create(iw, ih, GL_RGBA8, data, GL_RGBA, GL_UNSIGNED_BYTE, true, false);
    }
    stbi_image_free(data);
}

Vec4 Renderer::lineColor() const {
    return view_.whiteLine ? Vec4(1.0f, 1.0f, 1.0f, 1.0f)
                           : Vec4(1.0f, 236.0f / 255.0f, 159.0f / 255.0f, 1.0f);
}

void Renderer::drawJudgeLines(float time) {
    dbgLines = 0;
    const Vec4 pcolor = lineColor();
    const float r = (float)h_ * LINEH;
    const float th = (float)h_ * LINEW + 0.5f;

    int n = (int)chart_.lines.size();
    const bool hideAll = inAnyRange(hideLineRanges_, time);
    for (int i = 0; i < n && !hideAll; ++i) {
        const JudgeLine& line = chart_.lines[i];
        LineState ls;
        buildLineState(line, time, ls);
        float alpha = clampf(line.alpha(ls.beat), 0.0f, 1.0f);
        if (alpha <= 0.002f) continue;
        ++dbgLines;

        Vec2 c(ls.cx, ls.cy);
        Vec2 ax(ls.cosR * r, -ls.sinR * r);
        Vec2 ay(ls.sinR * th * 0.5f, ls.cosR * th * 0.5f);

        // The official JudgeLine.png is a flat 1920x3 white strip: the line is a
        // plain solid bar with no halo.  Draw exactly that, tinted by the line
        // colour.
        if (sprites_[TEX_LINE].tex.valid()) {
            pushSprite(TEX_LINE, gpu::makeSpriteAxes(c, ax, ay, Vec2(0, 0), Vec2(1, 1),
                                                     Vec4(pcolor.x, pcolor.y, pcolor.z, alpha)));
        } else {
            pushSprite(TEX_WHITE, gpu::makeSpriteAxes(c, ax, ay, Vec2(0, 0), Vec2(1, 1),
                                                      Vec4(pcolor.x, pcolor.y, pcolor.z, alpha)));
        }
    }
    flush(TEX_LINE, false);
    flush(TEX_WHITE, false);
}

void Renderer::drawHoldNotes(float time) {
    dbgHoldQuads = 0;
    const float viewFp = (float)h_ * 2.0f;
    // Official MH (multi-press hint) textures are wider than the normal set.
    const float mhRatio = (float)sprites_[TEX_HOLD_HEAD_B].tex.width /
                          (float)sprites_[TEX_HOLD_HEAD_A].tex.width;
    int n = (int)chart_.lines.size();
    for (int li = 0; li < n; ++li) {
        const JudgeLine& line = chart_.lines[li];
        LineState ls;
        buildLineState(line, time, ls);

        const std::vector<Note>* lists[2] = {&line.notesAbove, &line.notesBelow};
        for (int side = 0; side < 2; ++side) {
            bool above = (side == 0);
            for (const Note& note : *lists[side]) {
                if (note.type != 3) continue;
                if (!hideNoteRanges_.empty() && inAnyRange(hideNoteRanges_, note.secTime)) continue;
                if (note.secHoldEndTime <= time) continue;

                // Native official holds have independent head scrolling and
                // duration-based body length. Pure RPE holds retain integral tails.
                float travelSpeed = line.rpe && !note.officialHold ? note.speed : 1.0f;
                float headDist = (note.floorPosition - ls.fp) * PGRH * h_ * view_.flowSpeed * travelSpeed;
                float beatEnd = line.sec2beat(note.secHoldEndTime, chart_.offset);
                float endDist = note.officialHold
                    ? (note.secTime <= time ? 0.0f : headDist) + note.officialHoldBody(time) * h_ * view_.flowSpeed
                    : (line.fpAt(beatEnd) - ls.fp) * PGRH * h_ * view_.flowSpeed * travelSpeed;
                bool clicked = note.secTime <= time;
                if (headDist > viewFp && endDist > viewFp) continue;

                bool hint = view_.mhMode == 0 ? note.multipleHint : (view_.mhMode == 1);
                int headIdx = hint ? TEX_HOLD_HEAD_B : TEX_HOLD_HEAD_A;
                int bodyIdx = hint ? TEX_HOLD_BODY_B : TEX_HOLD_BODY_A;
                int tailIdx = hint ? TEX_HOLD_TAIL_B : TEX_HOLD_TAIL_A;
                float wpx = NOTE_SIZE * (float)w_ * note.size * view_.noteScale * (hint ? mhRatio : 1.0f);
                float headH = wpx / (float)sprites_[headIdx].tex.width * (float)sprites_[headIdx].tex.height;
                float tailH = wpx / (float)sprites_[tailIdx].tex.width * (float)sprites_[tailIdx].tex.height;

                // A hold whose head is already behind the judgement line has been
                // passed.  Charts with negative or zero speed segments make
                // floorPosition non-monotonic, so `secTime > time` alone is not
                // enough: without this test the bar collapses to bodyLen 0 and
                // leaves its 24 px head/tail caps sitting on the line (the "tiny
                // holds in odd places" artefact).
                if (!clicked && headDist < -headH) continue;
                if (endDist < -tailH) continue;

                // While not pressed the whole bar approaches as a rigid body;
                // once pressed the head sticks to the line and the bar shrinks
                // as the tail scrolls past (official behaviour).
                float bodyBottom = clicked ? 0.0f : headDist;
                if (bodyBottom < 0.0f) bodyBottom = 0.0f;
                float bodyTop = endDist;
                if (bodyTop < bodyBottom) bodyTop = bodyBottom;

                float flip = above ? 1.0f : -1.0f;
                Vec2 u(-flip * ls.sinR, -flip * ls.cosR);  // away from the line
                Vec2 uDown = Vec2(-u.x, -u.y);
                Vec2 perp(-u.y, u.x);                      // local texture +x
                float posX = note.positionX * PGRW * (float)w_ * (view_.mirror ? -1.0f : 1.0f);
                Vec2 origin(ls.cx + posX * ls.cosR, ls.cy - posX * ls.sinR);

                if (debugHolds_) {
                    std::printf("[hold] li=%d above=%d headDist=%.1f endDist=%.1f clicked=%d "
                                "bodyBottom=%.1f bodyTop=%.1f wpx=%.1f headH=%.1f tailH=%.1f hint=%d\n",
                                li, (int)above, headDist, endDist, (int)clicked, bodyBottom, bodyTop,
                                wpx, headH, tailH, (int)hint);
                }
                float bodyLen = bodyTop - bodyBottom;
                if (bodyLen > 0.5f) {
                    Vec2 c = origin + u * ((bodyBottom + bodyTop) * 0.5f);
                    pushSprite(bodyIdx, gpu::makeSpriteAxes(c, perp * (wpx * 0.5f), uDown * (bodyLen * 0.5f)));
                    ++dbgHoldQuads;
                }
                if (tailH > 0.0f) {
                    Vec2 c = origin + u * (bodyTop + tailH * 0.5f);
                    pushSprite(tailIdx, gpu::makeSpriteAxes(c, perp * (wpx * 0.5f), uDown * (tailH * 0.5f)));
                }
                if (!clicked && headH > 0.0f) {
                    // Head cap hangs toward the line from the body's leading edge.
                    Vec2 c = origin + u * (bodyBottom - headH * 0.5f);
                    pushSprite(headIdx, gpu::makeSpriteAxes(c, perp * (wpx * 0.5f), uDown * (headH * 0.5f)));
                }
            }
        }
    }
    // Draw order: body/tail behind head.
    flush(TEX_HOLD_BODY_A, false);
    flush(TEX_HOLD_BODY_B, false);
    flush(TEX_HOLD_TAIL_A, false);
    flush(TEX_HOLD_TAIL_B, false);
    flush(TEX_HOLD_HEAD_A, false);
    flush(TEX_HOLD_HEAD_B, false);
}

void Renderer::drawTapNotes(float time) {
    dbgTapQuads = 0;
    const float viewFp = (float)h_ * 2.0f;
    int n = (int)chart_.lines.size();
    for (int li = 0; li < n; ++li) {
        const JudgeLine& line = chart_.lines[li];
        LineState ls;
        buildLineState(line, time, ls);

        const std::vector<Note>* lists[2] = {&line.notesAbove, &line.notesBelow};
        for (int side = 0; side < 2; ++side) {
            bool above = (side == 0);
            for (const Note& note : *lists[side]) {
                if (note.type == 3) continue;
                if (!hideNoteRanges_.empty() && inAnyRange(hideNoteRanges_, note.secTime)) continue;
                if (note.secTime <= time) continue;
                float noteFp = (note.floorPosition - ls.fp) * PGRH * (float)h_ * note.speed * view_.flowSpeed;
                if (noteFp > viewFp) continue;
                if (noteFp < 0.0f) continue;

                // Multi-press hint: the official MH art is wider than the base art.
                const bool hint = view_.mhMode == 0 ? note.multipleHint : (view_.mhMode == 1);
                int idx = headTex(note.type, hint);
                float wpx = NOTE_SIZE * (float)w_ * note.size * view_.noteScale;
                if (hint) {
                    int normalIdx = headTex(note.type, false);
                    float normalW = (float)sprites_[normalIdx].tex.width;
                    if (normalW > 0.0f) wpx *= (float)sprites_[idx].tex.width / normalW;
                }
                float hpx = wpx / (float)sprites_[idx].tex.width * (float)sprites_[idx].tex.height;

                float flip = above ? 1.0f : -1.0f;
                float cosDraw = flip * ls.cosR;
                float sinDraw = flip * ls.sinR;
                float cosL2n = flip * (-ls.sinR);
                float sinL2n = flip * (-ls.cosR);
                float posX = note.positionX * PGRW * (float)w_ * (view_.mirror ? -1.0f : 1.0f);
                float atX = ls.cx + posX * ls.cosR;
                float atY = ls.cy - posX * ls.sinR;
                float headX = atX + noteFp * cosL2n;
                float headY = atY + noteFp * sinL2n;

                float rotDeg = ls.rotDeg - (above ? 0.0f : 180.0f);
                (void)cosDraw;
                (void)sinDraw;
                pushSprite(idx, gpu::makeSprite(Vec2(headX, headY), wpx, hpx, rotDeg * PI_OVER_180));
                ++dbgTapQuads;
            }
        }
    }
    flush(TEX_TAP_A, false);
    flush(TEX_TAP_B, false);
    flush(TEX_DRAG_A, false);
    flush(TEX_DRAG_B, false);
    flush(TEX_FLICK_A, false);
    flush(TEX_FLICK_B, false);
}

void Renderer::render(float time) {
    if (!ready_) return;

    scene_.bind();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    gpu::setBlendAlpha();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (debugClearOnly_) return;
    beginBatches();
    drawBackgroundLayer(time);
    flush(TEX_WHITE, false);

    // 噪域 disabled/ready layer goes behind the judgement lines (native sorting
    // layer order 2 vs judge lines 3).
    std::vector<noise::Zone> zones;
    buildZones(time, zones);
    noise_.setTime(time);
    zoneCount_ = hideNoise_ ? 0 : (int)zones.size();
    noise_.buildMask(hideNoise_ ? std::vector<noise::Zone>() : zones);
    // buildMask leaves blending disabled for its fullscreen passes: restore the
    // scene's 2D blend state before drawing any sprites.
    scene_.bind();
    glEnable(GL_BLEND);
    gpu::setBlendAlpha();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!hideNoise_) noise_.drawBackground(scene_);

    // drawBackground() disables blending for its fullscreen additive passes.
    scene_.bind();
    glEnable(GL_BLEND);
    gpu::setBlendAlpha();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!hideLines_) drawJudgeLines(time);
    if (!hideNotes_) { drawHoldNotes(time); drawTapNotes(time); }
    drawEffects(time);

    // 噪域 active layer is a post-process over notes and the illustration; the
    // HUD is drawn last so the noise never washes it out (official ordering).
    scene_.bind();
    if (!hideNoise_) noise_.drawActive(scene_);
    scene_.bind();
    glEnable(GL_BLEND);
    gpu::setBlendAlpha();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (!addons_.empty()) {
        addon::Context ctx;
        ctx.time = time;
        ctx.frame = frameIndex_;
        ctx.duration = duration_ > 0.0f ? duration_
                                        : (noteHits_.empty() ? 1.0f : noteHits_.back().time + 2.0f);
        ctx.progress = ctx.duration > 0.0f ? clampf(time / ctx.duration, 0.0f, 1.0f) : 0.0f;
        ctx.notes = (float)noteHits_.size();
        ctx.fps = frameRate_;
        ctx.lines = (float)chart_.lines.size();
        ctx.zones = (float)zoneCount_;

        // Binary search the note table once: combo and "since last hit" both
        // fall out of the same index.
        const auto it = std::upper_bound(noteHits_.begin(), noteHits_.end(), time,
                                         [](float t, const NoteHit& h) { return t < h.time; });
        ctx.combo = (float)(it - noteHits_.begin());
        if (it != noteHits_.begin()) {
            const NoteHit& last = *(it - 1);
            ctx.hit = time - last.time;
            ctx.lastX = last.x;
            ctx.lastY = last.y;
        }

        if (!chart_.lines.empty()) {
            const JudgeLine& l0 = chart_.lines[0];
            ctx.bpm = l0.bpm;
            // Quarter-note beats, the unit musicians mean by "beat".
            const float secPerQuarter = l0.bpm > 0.0f ? 60.0f / l0.bpm : 0.0f;
            ctx.beat = secPerQuarter > 0.0f ? time / secPerQuarter : 0.0f;
        }
        // Fastest judge line right now (a few dozen lines at most, and only when
        // an add-on is actually enabled).
        for (const auto& l : chart_.lines) {
            LineState ls;
            buildLineState(l, time, ls);
            const float sp = std::fabs(l.speedAt(ls.beat));
            if (sp > ctx.speed) ctx.speed = sp;
        }
        for (auto& a : addons_) a.apply(scene_, ctx);
    }
    drawHud(time);
    ++frameIndex_;
}

void Renderer::drawHud(float time) {
    if (!hudEnabled_ || w_ <= 0 || h_ <= 0) return;
    if (sprites_[TEX_HUD].tex.width != w_ || sprites_[TEX_HUD].tex.height != h_) {
        std::string error;
        if (!hud_.resize(w_, h_, &error)) {
            std::fprintf(stderr, "HUD: %s\n", error.c_str());
            throw std::runtime_error("HUD initialization failed: " + error);
        }
        sprites_[TEX_HUD].tex.create(w_, h_, GL_RGBA8, nullptr, GL_RGBA, GL_UNSIGNED_BYTE, true,
                                     false);
    }

    // Absolute timestamp counting keeps still previews, seeks and split exports
    // consistent with continuous video (including completed hold tails).
    auto it = std::upper_bound(noteHits_.begin(), noteHits_.end(), time,
        [](float t, const NoteHit& hit) { return t < hit.time; });
    int combo = int(it - noteHits_.begin());
    const unsigned int score = noteHits_.empty()
                                   ? 0u
                                   : (unsigned int)std::lround(1000000.0 * (double)combo /
                                                               (double)noteHits_.size());
    std::string error;
    if (!hud_.render(chart_, combo, score, &error)) {
        std::fprintf(stderr, "HUD: %s\n", error.c_str());
        hudEnabled_ = false;
        return;
    }

    glBindTexture(GL_TEXTURE_2D, sprites_[TEX_HUD].tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const auto& pixels = hud_.pixels();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    scene_.bind();
    glEnable(GL_BLEND);
    gpu::setBlendAlpha();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    pushSprite(TEX_HUD, gpu::makeSprite(Vec2(w_ * 0.5f, h_ * 0.5f), (float)w_, (float)h_, 0.0f,
                                        Vec2(0, 0), Vec2(1, 1), Vec4(1, 1, 1, 1)));
    flush(TEX_HUD, false);
}

bool Renderer::readPixelsRaw(std::vector<unsigned char>& rgba) const {
    if (!ready_) return false;
    rgba.resize((size_t)w_ * (size_t)h_ * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return true;
}

void Renderer::startReadback(int slot) {
    if (!ready_ || slot < 0 || slot >= kReadbackSlots) return;
    const size_t bytes = (size_t)w_ * (size_t)h_ * 4;
    if (!pbo_[0] || pboW_ != w_ || pboH_ != h_) {
        for (int i = 0; i < kReadbackSlots; ++i) {
            if (pbo_[i]) glDeleteBuffers(1, &pbo_[i]);
            glGenBuffers(1, &pbo_[i]);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        pboW_ = w_;
        pboH_ = h_;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, scene_.fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

bool Renderer::collectReadback(int slot, unsigned char* dst, size_t bytes) {
    if (!ready_ || !dst || slot < 0 || slot >= kReadbackSlots || !pbo_[slot]) return false;
    const size_t need = (size_t)w_ * (size_t)h_ * 4;
    if (bytes < need) return false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[slot]);
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)need, dst);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void Renderer::startReadbackYuv(int slot) {
    if (!ready_ || slot < 0 || slot >= kReadbackSlots) return;
    const size_t luma = (size_t)w_ * (size_t)h_;
    const size_t chroma = (size_t)yuvUV_.width * (size_t)yuvUV_.height * 2;
    if (!pboYuv_[0][0] || yuvW_ != w_ || yuvH_ != h_) {
        for (int i = 0; i < kReadbackSlots; ++i) {
            const size_t sizes[2] = {luma, chroma};
            for (int k = 0; k < 2; ++k) {
                if (pboYuv_[i][k]) glDeleteBuffers(1, &pboYuv_[i][k]);
                glGenBuffers(1, &pboYuv_[i][k]);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, pboYuv_[i][k]);
                glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)sizes[k], nullptr, GL_STREAM_READ);
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        yuvW_ = w_;
        yuvH_ = h_;
    }

    // Convert the scene into the two nv12 planes, then queue one pack copy per
    // plane. The passes are cheap (one sample per output pixel) and they let the
    // pipe carry 3.11 MB instead of 8.29 MB per frame.
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    yuvPass_.shader.use();
    yuvPass_.shader.set2i("uSize", w_, h_);
    yuvPass_.shader.setTex("uTex", 0, scene_.color.id);
    const gpu::RenderTarget* targets[2] = {&yuvY_, &yuvUV_};
    for (int k = 0; k < 2; ++k) {
        targets[k]->bind();
        yuvPass_.shader.set("uPlane", k);
        yuvPass_.draw();
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    for (int k = 0; k < 2; ++k) {
        targets[k]->bind();
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pboYuv_[slot][k]);
        glReadPixels(0, 0, targets[k]->width, targets[k]->height,
                     k == 0 ? GL_RED : GL_RG, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
}

bool Renderer::collectReadbackYuv(int slot, unsigned char* dst, size_t bytes) {
    if (!ready_ || !dst || slot < 0 || slot >= kReadbackSlots || !pboYuv_[slot][0]) return false;
    const size_t luma = (size_t)w_ * (size_t)h_;
    const size_t chroma = (size_t)yuvUV_.width * (size_t)yuvUV_.height * 2;
    if (bytes < luma + chroma) return false;
    const size_t sizes[2] = {luma, chroma};
    unsigned char* out = dst;
    for (int k = 0; k < 2; ++k) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pboYuv_[slot][k]);
        glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)sizes[k], out);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        out += sizes[k];
    }
    return true;
}

bool Renderer::readPixels(std::vector<unsigned char>& rgba) const {
    if (!readPixelsRaw(rgba)) return false;
    // Flip rows (GL origin is bottom-left, PNG is top-down).
    size_t rowBytes = (size_t)w_ * 4;
    std::vector<unsigned char> tmp(rowBytes);
    for (int y = 0; y < h_ / 2; ++y) {
        unsigned char* a = rgba.data() + (size_t)y * rowBytes;
        unsigned char* b = rgba.data() + (size_t)(h_ - 1 - y) * rowBytes;
        std::memcpy(tmp.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, tmp.data(), rowBytes);
    }
    return true;
}

bool Renderer::savePng(const std::string& path) const {
    std::vector<unsigned char> px;
    if (!readPixels(px)) return false;
    return stbi_write_png(path.c_str(), w_, h_, 4, px.data(), w_ * 4) != 0;
}

void Renderer::loadDuration(const std::string& chartPath) {
    size_t slash = chartPath.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string() : chartPath.substr(0, slash + 1);
    std::string data;
    std::ifstream f((dir + "info.json").c_str(), std::ios::binary);
    if (!f) return;
    data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) return;
    cJSON* root = cJSON_Parse(data.c_str());
    if (!root) return;
    cJSON* dur = cJSON_GetObjectItemCaseSensitive(root, "duration");
    if (dur && cJSON_IsNumber(dur) && dur->valuedouble > 0.0) duration_ = (float)dur->valuedouble;
    cJSON_Delete(root);
}

void Renderer::buildNoteTimes() {
    // One entry per note: its hit time plus where it sits on screen at that
    // moment (normalised to -1..1).  Built once, used by the add-on uniforms.
    noteHits_.clear();
    for (const auto& line : chart_.lines) {
        for (int side = 0; side < 2; ++side) {
            const auto& list = side == 0 ? line.notesAbove : line.notesBelow;
            for (const auto& n : list) {
                if (n.isFake) continue;
                LineState ls;
                buildLineState(line, n.secTime, ls);
                const float posX = n.positionX * PGRW * (float)w_;
                const float ox = ls.cx + posX * ls.cosR;
                const float oy = ls.cy - posX * ls.sinR;
                NoteHit h;
                // Phigros/Phira counts a Hold when its tail is completed.
                h.time = n.type == 3 ? n.secHoldEndTime : n.secTime;
                h.x = w_ > 0 ? (ox / (float)w_) * 2.0f - 1.0f : 0.0f;
                h.y = h_ > 0 ? 1.0f - (oy / (float)h_) * 2.0f : 0.0f;
                noteHits_.push_back(h);
            }
        }
    }
    std::sort(noteHits_.begin(), noteHits_.end(),
              [](const NoteHit& a, const NoteHit& b) { return a.time < b.time; });
}

void Renderer::buildHitEffects() {
    hitEffects_.clear();
    uint32_t randomIndex=0;
    const float W=float(w_),H=float(h_);
    for(const auto& line:chart_.lines){
        std::vector<const Note*> notes;
        for(const auto& n:line.notesAbove)notes.push_back(&n);
        for(const auto& n:line.notesBelow)notes.push_back(&n);
        std::stable_sort(notes.begin(),notes.end(),[](const Note* a,const Note* b){return a->secTime<b->secTime;});
        auto emit=[&](const Note& n,float t){
            HitEffect e;e.time=t;e.size=n.size;e.type=n.type;
            LineState ls;buildLineState(line,t,ls);
            float x=n.positionX*PGRW*W*(view_.mirror?-1.f:1.f);
            e.pos=Vec2(ls.cx+x*ls.cosR,ls.cy-x*ls.sinR);
            // PhiVideo's flash is screen aligned, independent of line rotation.
            e.rot=0;
            for(auto& par:e.particles){
                par.x=hitfx::random(0,360,randomIndex++)*PI_OVER_180;
                par.y=hitfx::random(185,265,randomIndex++);
            }
            hitEffects_.push_back(e);
        };
        for(const Note* ptr:notes){const auto& n=*ptr;
            if(n.isFake||n.secTime<0||(!hideNoteRanges_.empty()&&inAnyRange(hideNoteRanges_,n.secTime)))continue;
            emit(n,n.secTime);
            // PhiVideo also creates a clickCollection particle set (not drawn
            // here). Advance the seed sequence to preserve the effect stream.
            randomIndex+=8;
            if(n.type==3){
                if(line.rpe){for(float t:n.holdEffectTimes)emit(n,t);}
                else {for(float beat=n.time;beat<n.time+n.holdTime;beat+=16.f)
                    emit(n,line.beat2sec(beat,chart_.offset));}
                // The original source emits both the initial hit and the
                // first hold tick at the head. Retain that behavior explicitly.
            }
        }
    }
    std::stable_sort(hitEffects_.begin(),hitEffects_.end(),[](const HitEffect&a,const HitEffect&b){return a.time<b.time;});
}

void Renderer::drawEffects(float time) {
    if(hitEffects_.empty())return;
    auto lo=std::lower_bound(hitEffects_.begin(),hitEffects_.end(),time-hitfx::duration,
        [](const HitEffect&e,float t){return e.time<t;});
    const int cols=packInfo_.hitFxCols,rows=packInfo_.hitFxRows,count=cols*rows;
    const float frameW=float(sprites_[TEX_HITFX].tex.width)/cols;
    const float frameH=float(sprites_[TEX_HITFX].tex.height)/rows;
    const float scale=packInfo_.hitFxScale*view_.fxScale;
    const Vec4 gold(1,236.f/255,159.f/255,1);
    for(auto it=lo;it!=hitEffects_.end()&&it->time<=time;++it){
        float age=time-it->time;
        if(age<0||age>=hitfx::duration)continue;
        float progress=hitfx::progress(age);
        int index=hitfx::frame(age,count),col=index%cols,row=index/cols;
        float width=hitfx::width(NOTE_SIZE*w_)*it->size*scale;
        Vec2 uv0(float(col)/cols,float(row)/rows),uv1(float(col+1)/cols,float(row+1)/rows);
        // Normal source-over blending, texture RGB and its baked alpha. There
        // is no additional fade, rotation or additive ring in PhiVideo.
        pushSprite(TEX_HITFX,gpu::makeSprite(it->pos,width,width*frameH/frameW,0,uv0,uv1,gold));
        flush(TEX_HITFX,false);
        float pixelScale=hitfx::particleScale(float(w_))*scale*it->size;
        float particleSize=33.f*.75f*pixelScale;
        for(const auto& par:it->particles){
            float distance=par.y*pixelScale*hitfx::particleTravel(progress);
            Vec2 center(it->pos.x+std::cos(par.x)*distance,it->pos.y+std::sin(par.x)*distance);
            pushSprite(TEX_WHITE,gpu::makeSprite(center,particleSize,particleSize,0,Vec2(0,0),Vec2(1,1),
                Vec4(gold.x,gold.y,gold.z,1-progress)));
        }
        // Preserve event order: each flash is followed by its own particles.
        flush(TEX_WHITE,false);
    }
}
