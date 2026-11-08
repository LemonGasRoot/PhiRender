// addon.h - optional post-process add-ons.
//
// An add-on is a directory with an `addon.json` manifest and a fragment shader.
// It is applied as one extra fullscreen pass over the finished scene, with
// uniforms uScene/uTime/uResolution plus one uParam_<name> per manifest param.
//
// When no add-on is selected nothing is allocated and nothing is drawn: the
// renderer only checks a bool at the end of render().
#pragma once

#include "gpu.h"

#include <string>
#include <utility>
#include <vector>

namespace addon {

    // Per-frame song state handed to the shader.  Everything here is cheap to
    // obtain (precomputed tables or a binary search) and is only filled in when
    // at least one add-on is enabled.
    struct Context {
        float time = 0.0f;      // seconds since the chart start
        float progress = 0.0f;  // 0..1 through the chart
        float duration = 0.0f;  // chart length in seconds
        float combo = 0.0f;     // notes passed so far
        float notes = 0.0f;     // total notes in the chart
        float beat = 0.0f;      // beat number on the first judge line
        float bpm = 0.0f;       // BPM of the first judge line
        float speed = 0.0f;     // fastest judge line speed right now
        float hit = -1.0f;      // seconds since the last note (negative = none yet)
        float lastX = 0.0f;     // last note position, -1..1 (x right, y up)
        float lastY = 0.0f;
        float fps = 60.0f;      // output frame rate
        float lines = 0.0f;     // judge line count
        float zones = 0.0f;     // noise areas visible this frame
        int frame = 0;
    };

    // A noise area the add-on injects into the scene itself (not a post effect):
    // the renderer feeds it through the same NoiseField pipeline as the chart's
    // own blockAreaList, so it occludes notes like a real noise area.
    struct BlockSpec {
        float appear = 0.0f, disappear = 0.0f;
        float enable = 0.0f, disable = 0.0f;
        float x = 0.0f, y = 0.0f;   // chart space centre
        float w = 1.0f, h = 0.4f;   // chart space half-size
        float rot = 0.0f;           // degrees
        bool subtract = false;
    };

    // A note the add-on adds to the chart.  `line` indexes the chart's judge
    // lines (0 = first); the renderer derives the beat and floor position so the
    // note scrolls like a real one and even gets a hit effect.
    struct NoteSpec {
        float time = 0.0f;   // seconds
        float x = 0.0f;      // positionX
        int type = 1;        // 1 tap, 2 drag, 3 hold, 4 flick
        int line = 0;
        float size = 1.0f;
    };

    // A static judge line (no events) that exists between appear and disappear.
    struct LineSpec {
        float appear = 0.0f, disappear = 1e9f;
        float x = 0.0f, y = 0.0f;   // chart space
        float rot = 0.0f;           // degrees
    };

    struct Addon {
        struct TextureParam {
            std::string name;
            gpu::Texture tex;
        };

        bool active = false;
        bool fromChart = false;  // loaded from <chart dir>/addons (params are fixed)
        std::string id;       // directory name
        std::string name;     // display name from the manifest
        std::string author;
        std::string version;
        std::vector<std::pair<std::string, float>> params;
        std::vector<TextureParam> textures;
        std::vector<BlockSpec> blocks;   // noise areas injected into the scene
        std::vector<NoteSpec> notes;     // extra notes appended to the chart
        std::vector<LineSpec> lines;     // extra static judge lines
        std::vector<std::pair<float, float>> hideNotes;  // time ranges without notes
        std::vector<std::pair<float, float>> hideLines;  // time ranges without lines
        gpu::FullscreenPass pass;
        gpu::RenderTarget scratch;
        int w = 0, h = 0;

        // Loads <dir>/addon.json and its shader.  Returns false with `error` set;
        // the caller decides whether that is fatal (it is not: the renderer just
        // keeps going without the add-on).
        bool load(const std::string& dir, std::string* error);
        void setParam(const std::string& key, float value);
        // Blits the scene into an internal target, then draws the add-on shader
        // back over it.  Creates the scratch target on first use.
        void apply(gpu::RenderTarget& scene, const Context& ctx);
        void destroy();
    };

    // Directory resolution: an explicit path is used as is, otherwise
    // "<assets>/../addons/<id>" and "./addons/<id>" are tried.
    std::string resolveDir(const std::string& idOrPath, const std::string& assetDir);
    // Lists the add-on ids found under ./addons (for --list-addons).
    std::vector<std::string> list(const std::string& root);
    // Chart-local add-ons: <chartdir>/addons/config.json maps an add-on folder
    // name to its parameter overrides.  Unknown keys are ignored, a missing file
    // is not an error.
    bool readChartConfig(const std::string& path,
                         std::vector<std::pair<std::string, std::vector<std::pair<std::string, float>>>>
                             * out,
                         std::string* error);

}  // namespace addon
