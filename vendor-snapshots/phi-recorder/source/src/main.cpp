// main.cpp - phirender CLI: renders a Phigros chart (official 4.0 format with 噪域).
#include "common.h"
#include "gl.h"
#include "gpu.h"
#include "addon.h"
#include "audio.h"
#include "framesink.h"
#include "renderer.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

static const char* kVersion = "2.0.0";

struct Args {
    std::string chart;
    std::string background;
    std::string assets = "assets/default";
    std::string debugNoise;
    std::string out = "out/frame.png";
    std::string outDir;   // when set, render a PNG sequence
    std::string video;    // when set, encode an MP4 with ffmpeg
    std::string audio;    // optional audio track for the video
    std::string preset = "medium";
    bool nvenc = false, noAudio = false;
    int outputWidth=0, outputHeight=0;
    int crf = 18;
    bool version = false;  // --version: print and exit without touching GL
    int encoders = 0;  // parallel ffmpeg processes for --video (0 = pick from the machine)
    // Raw frame layout on the encoder pipe: "nv12" (converted on the GPU, the
    // default) or "rgba" (the old 8.29 MB/frame path, kept as a fallback).
    std::string pipeFormat = "nv12";
    std::vector<std::string> addons;   // post-process add-ons, applied in order
    bool listAddons = false;    // --list-addons
    std::vector<std::pair<std::string, float>> addonParams;
    float time = 0.0f;
    float from = 0.0f;
    float to = 0.0f;
    float fps = 60.0f;
    int width = 1920;
    int height = 1080;
    bool noLines = false, noNotes = false, noNoise = false;
    bool hud = false;
    std::string hudFont, hudFontFamily = "Saira";
    std::string infoFont, infoFontFamily = "Saira";
    bool noHits = false;
    float musicVolume = 1.0f;
    float notesVolume = 0.5f;
    std::string mixAudio;  // write the mixed hit track to this wav and exit
    int bench = 0;             // render N frames without saving, for benchmarking
    // View settings (official defaults).
    float bgDim = 0.60f;       // official background_dim: extra full-screen black layer
    float bgBaseDim = 0.30f;   // official chart background base dim (prpr scene.rs)
    float bgBlur = 10.0f;       // extra blur radius applied to the background texture, in source px
    float bgBrightness = -1.0f;  // <0 = derive the dim from --bg-dim instead
    float noteScale = 1.0f;      // global note-size multiplier
    bool mirror = false;         // mirror the chart left/right
    bool whiteLine = false;      // judge line colour: gold or white
    int mhMode = 0;              // 0 = follow the chart, 1 = always multi-press art, 2 = never
    float flowSpeed = 1.0f;    // official flow_speed: note scroll speed multiplier
    float fxScale = 1.0f;      // hit-effect size multiplier over the note-matched base
};

void usage() {
    std::printf(
        "phirender - Phigros 4.0 chart renderer (noise areas / 噪域 supported)\n"
        "usage: phirender --chart <chart.json> [options]\n"
        "  --bg <image>          background illustration (e.g. illustrationBlur.png)\n"
        "  --assets <dir>        asset pack root (default assets/default)\n"
        "  --time <sec>          time for single-frame render (default 0)\n"
        "  --out <file.png>      single-frame output path\n"
        "  --outdir <dir>        render a PNG sequence instead\n"
        "  --video <file.mp4>    encode a video with ffmpeg (uses --from/--to/--fps)\n"
        "  --audio <file>        audio track for --video\n"
        "  --crf <n>             x264 quality (default 18)\n"
        "  --preset <name>       x264 preset (default medium)\n"
        "  --addon <id|dir>      apply a post-process add-on from addons/<id>\n"
        "  --addon-param k=v     override an add-on parameter (repeatable)\n"
        "  --list-addons         list the add-ons found under addons/\n"
        "  --encoders <n>        parallel ffmpeg processes for --video (0 = auto)\n"
        "  --pipe <fmt>          raw frame layout: nv12 (default) or rgba\n"
        "  --mix-audio <wav>     write the mixed hit track and exit\n"
        "  --music-volume <x> --notes-volume <x> --no-hits   audio mixing\n"
        "  --bg-dim <0..1>       extra full-screen dim over the background (0.60)\n"
        "  --bg-blur <px>        extra blur on the background, in source texels (default 10)\n"
        "  --bg-brightness <0..1>  illustration brightness (0 = black, 0.28 = game default)\n"
        "  --bg-base-dim <0..1>  chart background base dim (0.30)\n"
        "  --flow-speed <x>      note scroll speed multiplier (1.0)\n"
        "  --note-size <x>       note size multiplier (1.0 = official)\n"
        "  --mh-hint <0|1|2>     multi-press hint art: 0 chart, 1 always, 2 never\n"
        "  --mirror              mirror the chart left/right\n"
        "  --line-color <c>      judge line colour: gold (default) or white\n"
        "  --hud                 draw pause, score, autoplay combo, song and level\n"
        "  --hud-font <ttf>      optional gameplay HUD font file\n"
        "  --hud-font-family <n> gameplay font family (default Saira)\n"
        "  --info-font <ttf>     optional song-info font file\n"
        "  --info-font-family <n> song-info font family (default Saira)\n"
        "  --fx-scale <x>        hit-effect size multiplier over note size (1.0)\n"
        "  --debug-noise <pfx>   dump noise render targets as <pfx>*.png\n"
        "  --no-lines --no-notes --no-noise   hide layers (debug)\n"
        "  --from <sec> --to <sec> --fps <n>   range (default 0..duration, 60)\n"
        "  --w <px> --h <px>     output size (default 1920x1080)\n"
        "  --bench <n>           render n frames without saving (perf check)\n"
        "  --version             print the version and exit\n");
}

bool parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (s == "--chart") { if (!next(a.chart)) return false; }
        else if (s == "--bg") { if (!next(a.background)) return false; }
        else if (s == "--assets") { if (!next(a.assets)) return false; }
        else if (s == "--out") { if (!next(a.out)) return false; }
        else if (s == "--outdir") { if (!next(a.outDir)) return false; }
        else if (s == "--video") { if (!next(a.video)) return false; }
        else if (s == "--no-audio") { a.noAudio=true; }
        else if (s == "--nvenc") { a.nvenc=true; }
        else if (s == "--output-w") { if(i+1>=argc)return false; a.outputWidth=atoi(argv[++i]); }
        else if (s == "--output-h") { if(i+1>=argc)return false; a.outputHeight=atoi(argv[++i]); }
        else if (s == "--audio") { if (!next(a.audio)) return false; }
        else if (s == "--mix-audio") { if (!next(a.mixAudio)) return false; }
        else if (s == "--no-hits") { a.noHits = true; }
        else if (s == "--music-volume") { if (i + 1 >= argc) return false; a.musicVolume = (float)atof(argv[++i]); }
        else if (s == "--notes-volume") { if (i + 1 >= argc) return false; a.notesVolume = (float)atof(argv[++i]); }
        else if (s == "--preset") { if (!next(a.preset)) return false; }
        else if (s == "--debug-noise") { if (!next(a.debugNoise)) return false; }
        else if (s == "--time") { if (i + 1 >= argc) return false; a.time = (float)atof(argv[++i]); }
        else if (s == "--from") { if (i + 1 >= argc) return false; a.from = (float)atof(argv[++i]); }
        else if (s == "--to") { if (i + 1 >= argc) return false; a.to = (float)atof(argv[++i]); }
        else if (s == "--fps") { if (i + 1 >= argc) return false; a.fps = (float)atof(argv[++i]); }
        else if (s == "--bg-dim") { if (i + 1 >= argc) return false; a.bgDim = (float)atof(argv[++i]); }
        else if (s == "--bg-base-dim") { if (i + 1 >= argc) return false; a.bgBaseDim = (float)atof(argv[++i]); }
        else if (s == "--bg-blur") { if (i + 1 >= argc) return false; a.bgBlur = (float)atof(argv[++i]); }
        else if (s == "--note-size") { if (i + 1 >= argc) return false; a.noteScale = (float)atof(argv[++i]); }
        else if (s == "--mh-hint") { if (i + 1 >= argc) return false; a.mhMode = atoi(argv[++i]); }
        else if (s == "--mirror") { a.mirror = true; }
        else if (s == "--line-color") { if (i + 1 >= argc) return false; a.whiteLine = std::string(argv[++i]) == "white"; }
        else if (s == "--version") {
            std::printf("phirender %s\n", kVersion);
            a.version = true;
        }
        else if (s == "--bg-brightness") { if (i + 1 >= argc) return false; a.bgBrightness = (float)atof(argv[++i]); }
        else if (s == "--fx-scale") { if (i + 1 >= argc) return false; a.fxScale = (float)atof(argv[++i]); }
        else if (s == "--flow-speed") { if (i + 1 >= argc) return false; a.flowSpeed = (float)atof(argv[++i]); }
        else if (s == "--crf") { if (i + 1 >= argc) return false; a.crf = atoi(argv[++i]); }
        else if (s == "--list-addons") { a.listAddons = true; }
        else if (s == "--addon") { if (i + 1 >= argc) return false; a.addons.push_back(argv[++i]); }
        else if (s == "--addon-param") {
            if (i + 1 >= argc) return false;
            std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "--addon-param needs k=v\n"); return false; }
            a.addonParams.emplace_back(kv.substr(0, eq), (float)atof(kv.c_str() + eq + 1));
        }
        else if (s == "--encoders") { if (i + 1 >= argc) return false; a.encoders = atoi(argv[++i]); }
        else if (s == "--pipe") {
            if (i + 1 >= argc) return false;
            a.pipeFormat = argv[++i];
            if (a.pipeFormat == "yuv") a.pipeFormat = "nv12";
            if (a.pipeFormat != "rgba" && a.pipeFormat != "nv12") {
                std::fprintf(stderr, "unknown --pipe format: %s\n", a.pipeFormat.c_str());
                return false;
            }
        }
        else if (s == "--w") { if (i + 1 >= argc) return false; a.width = atoi(argv[++i]); }
        else if (s == "--h") { if (i + 1 >= argc) return false; a.height = atoi(argv[++i]); }
        else if (s == "--no-lines") { a.noLines = true; }
        else if (s == "--no-notes") { a.noNotes = true; }
        else if (s == "--no-noise") { a.noNoise = true; }
        else if (s == "--hud") { a.hud = true; }
        else if (s == "--hud-font") { if (!next(a.hudFont)) return false; }
        else if (s == "--hud-font-family") { if (!next(a.hudFontFamily)) return false; }
        else if (s == "--info-font") { if (!next(a.infoFont)) return false; }
        else if (s == "--info-font-family") { if (!next(a.infoFontFamily)) return false; }
        else if (s == "--bench") { if (i + 1 >= argc) return false; a.bench = atoi(argv[++i]); }
        else if (s == "-h" || s == "--help") { usage(); return false; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", s.c_str());
            return false;
        }
    }
    return true;
}

float targetDuration(const Renderer& r, float to) {
    if (to > 0.0f) return to;
    return r.duration() > 0.0f ? r.duration() : 175.0f;
}

// Finds a music file next to the chart (music.ogg / music.mp3 / music.wav).
std::string autoMusicPath(const std::string& chartPath) {
    size_t slash = chartPath.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "." : chartPath.substr(0, slash);
    const char* names[] = {"/music.ogg", "/music.mp3", "/music.wav", "/audio.ogg"};
    for (const char* n : names) {
        std::string p = dir + n;
        FILE* f = std::fopen(p.c_str(), "rb");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return std::string();
}

// One background thread is enough for the audio mix, and it must not be a
// blocked-on-GL std::thread (MinGW here is a win32-threads build without them).
struct BgTask {
    void (*fn)(void*) = nullptr;
    void* arg = nullptr;
    HANDLE handle = nullptr;
    static DWORD WINAPI trampoline(LPVOID p) {
        BgTask* t = (BgTask*)p;
        t->fn(t->arg);
        return 0;
    }
    bool start(void (*f)(void*), void* a) {
        fn = f;
        arg = a;
        handle = CreateThread(nullptr, 0, trampoline, this, 0, nullptr);
        return handle != nullptr;
    }
    void join() {
        if (!handle) return;
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        handle = nullptr;
    }
    ~BgTask() { join(); }
};

struct MixJob {
    const Chart* chart = nullptr;
    std::string music;
    std::string out;
    audio::MixOptions opts;
    bool ok = true;
    std::string err;
};

void runMix(void* p) {
    MixJob* j = (MixJob*)p;
    j->ok = audio::mixChartAudio(*j->chart, j->music, j->out, j->opts, &j->err);
}

// Encodes frames straight from GL readback into ffmpeg's stdin (rawvideo RGBA).
bool renderVideo(Renderer& renderer, const Args& a) {
    float to = targetDuration(renderer, a.to);
    if (to <= a.from) to = a.from + 1.0f;
    int frames = (int)std::lround((double)(to - a.from) * (double)a.fps);
    if (frames <= 0) frames = 1;

    // Resolve the music track: explicit --audio wins, otherwise look next to the chart.
    std::string music = a.audio;
    if (music.empty()) music = autoMusicPath(a.chart);
    if(a.noAudio)music.clear();
    std::string audioPath = music;
    std::string mixedPath;
    // RAII: the mixed wav is a temp file and must vanish on every exit path,
    // including the early returns below (a leaked 61 MB wav used to linger).
    struct TempFile {
        std::string path;
        ~TempFile() {
            if (!path.empty()) std::remove(path.c_str());
        }
    } mixedGuard;
    // The mix only has to exist by the time the video is muxed, so it runs on a
    // background thread while the GPU renders: it is seconds of DSP work and
    // touches nothing the render loop uses.
    MixJob mixJob;
    BgTask mixTask;
    bool mixing = false;
    if (!music.empty() && !a.noHits) {
        mixedPath = a.video + ".mix.wav";
        mixJob.chart = &renderer.chart();
        mixJob.music = music;
        mixJob.out = mixedPath;
        mixJob.opts.musicVolume = a.musicVolume;
        mixJob.opts.notesVolume = a.notesVolume;
        mixJob.opts.hitDir = a.assets + "/Notes";
        mixJob.opts.startSec = a.from;
        mixJob.opts.durationSec = to - a.from;
        mixing = mixTask.start(runMix, &mixJob);
        if (mixing) audioPath = mixedPath;
    }
    mixedGuard.path = mixedPath;

    const int totalFrames = frames;
    std::printf("video: %d frames %.2fs..%.2fs @ %.2f fps -> %s\n", totalFrames, a.from, to,
                a.fps, a.video.c_str());

    framesink::Options so;
    so.outPath = a.video;
    so.audioPath = audioPath;
    so.width = a.width;
    so.height = a.height;
    so.fps = a.fps;
    so.preset = a.preset;
    so.nvenc = a.nvenc;
    so.outputWidth=a.outputWidth;so.outputHeight=a.outputHeight;
    so.crf = a.crf;
    so.totalFrames = totalFrames;
    so.segments = a.encoders;
    so.pipeFormat = a.pipeFormat;

    framesink::FrameSink sink;
    std::string sinkErr;
    if (!sink.start(so, &sinkErr)) {
        std::fprintf(stderr, "video: %s\n", sinkErr.c_str());
        return false;
    }
    if (sink.segments() > 1) std::printf("video: %d parallel encoders\n", sink.segments());
    const bool yuvPipe = so.pipeFormat != "rgba";
    auto startCopy = [&](int pbo) {
        if (yuvPipe) renderer.startReadbackYuv(pbo);
        else renderer.startReadback(pbo);
    };
    auto collectCopy = [&](int pbo, unsigned char* dst) {
        return yuvPipe ? renderer.collectReadbackYuv(pbo, dst, sink.frameBytes())
                       : renderer.collectReadback(pbo, dst, sink.frameBytes());
    };

    auto tStart = std::chrono::high_resolution_clock::now();
    long long done = 0;
    // One frame is always in flight: its GPU->PBO copy has been queued but not
    // collected. That copy overlaps the next render and the encoder wait.
    struct InFlight {
        int frame;
        int slot;
        unsigned char* dst;
        int pbo;
    };
    std::vector<InFlight> inflight;
    const int kDepth = 3;  // copies in flight; deeper hides more of the transfer
    int curPbo = 0;
    // Walk the schedule segment by segment, one frame each per round: every
    // encoder is fed from the first round on, instead of encoder 0 taking its
    // whole range before encoder 1 sees a frame. Rendering a time out of order
    // is safe, render(t) depends on nothing but t.
    const int steps = sink.stepsPerSegment();
    const int segs = sink.segments();
    for (int step = 0; step < steps; ++step) {
        for (int seg = 0; seg < segs; ++seg) {
            const int i = sink.frameFor(seg, step);
            if (i < 0) continue;
            const float t = a.from + (float)i / a.fps;
            renderer.render(t);
            // A slot may only be reused after its copy has been collected, so the
            // oldest copy must leave the ring before this frame lands in it.
            if ((int)inflight.size() >= kDepth) {
                const InFlight& f = inflight.front();
                if (!collectCopy(f.pbo, f.dst)) {
                    std::fprintf(stderr, "readback failed\n");
                    return false;
                }
                sink.submit(f.frame, f.slot);
                inflight.erase(inflight.begin());
            }
            // Queue this frame's copy into a pixel-pack buffer; it runs on the GPU
            // while the CPU waits for encoder room.
            startCopy(curPbo);
            int slot = 0;
            unsigned char* dst = sink.acquire(i, &slot);
            if (!dst) {
                std::fprintf(stderr, "ffmpeg pipe closed early\n");
                return false;
            }
            inflight.push_back(InFlight{i, slot, dst, curPbo});
            curPbo = (curPbo + 1) % kDepth;
            ++done;
            if ((done % 30) == 0 || done == totalFrames) {
                auto tNow = std::chrono::high_resolution_clock::now();
                double sec = std::chrono::duration<double>(tNow - tStart).count();
                std::printf("  frame %lld/%d t=%.2f (%.1f fps wall)\n", done, totalFrames, t,
                            done / (sec > 1e-6 ? sec : 1e-6));
                std::fflush(stdout);
            }
        }
    }
    for (const InFlight& f : inflight) {
        if (!collectCopy(f.pbo, f.dst)) {
            std::fprintf(stderr, "readback failed\n");
            return false;
        }
        sink.submit(f.frame, f.slot);
    }
    inflight.clear();
    if (mixing) {
        mixTask.join();
        if (!mixJob.ok) {
            std::fprintf(stderr, "audio mix failed: %s\n", mixJob.err.c_str());
            std::fprintf(stderr, "continuing without audio\n");
            sink.setAudioPath(music);  // fall back to the untouched track
        }
    }
    if (!sink.finish(&sinkErr)) {
        std::fprintf(stderr, "video: %s\n", sinkErr.c_str());
        return false;
    }
    std::printf("video done -> %s\n", a.video.c_str());
    return true;
}

}  // namespace

namespace {
int runMain(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) return 1;
    if (args.version) return 0;
    if (args.listAddons) {
        const std::vector<std::string> ids = addon::list("addons");
        if (ids.empty()) std::printf("addons: none found under ./addons\n");
        for (const std::string& id : ids) std::printf("addon: %s\n", id.c_str());
        return 0;
    }
    if (args.chart.empty()) {
        usage();
        return 1;
    }
    if (args.width < 16 || args.height < 16 || (args.width & 1) || (args.height & 1)) {
        std::fprintf(stderr, "bad output size (must be even and >= 16)\n");
        return 1;
    }

    std::string err;
    gl::ContextInfo info;
    if (!gl::createContext(args.width, args.height, &info, &err)) {
        std::fprintf(stderr, "OpenGL init failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> missing;
    gl::loadFunctions(&missing);
    if (!missing.empty()) {
        std::fprintf(stderr, "missing GL functions:");
        for (auto& m : missing) std::fprintf(stderr, " %s", m.c_str());
        std::fprintf(stderr, "\n");
    }
    std::printf("GL  : %s | %s | %s | GLSL %s\n", info.vendor.c_str(), info.renderer.c_str(),
                info.version.c_str(), info.glsl.c_str());

    Renderer renderer;
    // The background texture is loaded during init(), so its blur radius has to
    // be in place first; the other view settings are read at render time.
    renderer.viewSettings().bgBlur = args.bgBlur;
    if (args.background.empty()) {
        // Auto background: prefer the sharp art, since loadBackground blurs it
        // automatically; a pre-blurred asset is only a fallback.
        std::string dir = args.chart;
        size_t slash = dir.find_last_of("/\\");
        if (slash != std::string::npos) dir = dir.substr(0, slash + 1); else dir.clear();
        // The sharp illustration wins: the background is blurred automatically, so
        // a pre-blurred asset would only be blurred twice.
        const char* candidates[] = {"illustration.png", "illustrationBlur.png", "bg.png", "bg.jpg"};
        for (const char* c : candidates) {
            std::string p = dir + c;
            FILE* f = std::fopen(p.c_str(), "rb");
            if (f) { std::fclose(f); args.background = p; break; }
        }
    }
    renderer.viewSettings().noteScale = args.noteScale;
    renderer.viewSettings().mhMode = args.mhMode;
    renderer.viewSettings().mirror = args.mirror;
    renderer.viewSettings().whiteLine = args.whiteLine;
    if (!renderer.init(args.assets, args.chart, args.background, &err)) {
        std::fprintf(stderr, "init failed: %s\n", err.c_str());
        return 1;
    }
    if (args.bgBrightness >= 0.0f) {
        // Single knob: 0 = black, 1 = the raw illustration.  The chart's own 0.30
        // base dim always applies, so the extra layer takes up the remainder.
        const float b = args.bgBrightness > 1.0f ? 1.0f : args.bgBrightness;
        args.bgDim = 1.0f - b / (1.0f - args.bgBaseDim);
        if (args.bgDim < 0.0f) args.bgDim = 0.0f;
        if (args.bgDim > 1.0f) args.bgDim = 1.0f;
    }
    renderer.viewSettings().bgDim = args.bgDim;
    renderer.viewSettings().bgBaseDim = args.bgBaseDim;
    renderer.viewSettings().flowSpeed = args.flowSpeed;
    renderer.setFrameRate(args.fps);
    renderer.viewSettings().fxScale = args.fxScale;
    if (!renderer.resize(args.width, args.height, &err)) {
        std::fprintf(stderr, "resize failed: %s\n", err.c_str());
        return 1;
    }
    // Optional post-process add-on.  A broken add-on warns instead of aborting:
    // the render must still produce frames without it.
    for (size_t ai = 0; ai < args.addons.size(); ++ai) {
        std::string addonErr;
        if (!renderer.enableAddon(args.addons[ai], &addonErr)) {
            std::fprintf(stderr, "addon: %s (ignored)\n", addonErr.c_str());
            continue;
        }
        addon::Addon* ad = renderer.lastAddon();
        // --addon-param values target the most recently loaded add-on.
        if (ai == args.addons.size() - 1) {
            for (const auto& kv : args.addonParams) ad->setParam(kv.first, kv.second);
        }
        std::printf("addon %zu: %s (%s %s)\n", ai + 1, ad->name.c_str(),
                    ad->author.c_str(), ad->version.c_str());
    }
    // Add-ons that ship with the chart: <chartdir>/addons/<id>/ each, with their
    // parameters fixed by <chartdir>/addons/config.json (the command line cannot
    // override those).  No folder means nothing is loaded.
    if (const int n = renderer.loadChartAddons(args.chart, &err); n > 0) {
        std::printf("chart addons: %d loaded from the chart folder\n", n);
    } else if (!err.empty()) {
        std::fprintf(stderr, "chart addons: %s\n", err.c_str());
    }
    renderer.setHideLayers(args.noLines, args.noNotes, args.noNoise);
    renderer.setHudEnabled(args.hud);
    if (args.hud) {
        const std::string hudFont = args.hudFont.empty() ? args.assets + "/hud/saira-regular.ttf"
                                                         : args.hudFont;
        const std::string infoFont = args.infoFont.empty() ? args.assets + "/hud/saira-regular.ttf"
                                                           : args.infoFont;
        renderer.setHudFonts(hudFont, args.hudFontFamily, infoFont, args.infoFontFamily);
    }
    std::printf("chart: %d lines, %d blocks, %d notes\n", (int)renderer.chart().lines.size(),
                (int)renderer.chart().blocks.size(), renderer.chart().noteCount);

    if (args.bench > 0) {
        // Bench a window: --from/--to if given, otherwise the whole chart. A fixed
        // window makes runs comparable (noise zones are only visible at some times).
        float bFrom = args.from;
        float bTo = args.to > bFrom ? args.to : targetDuration(renderer, args.to);
        if (bTo <= bFrom) bTo = bFrom + 2.0f;
        renderer.render(bFrom);  // warm-up
        glFinish();
        auto b0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < args.bench; ++i) {
            float t = bFrom + (bTo - bFrom) * (float)i / (float)args.bench;
            renderer.render(t);
            glFinish();  // GL is async: wait for the frame to actually finish
        }
        auto b1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(b1 - b0).count() / (double)args.bench;
        std::printf("bench: %d frames, %.2f ms/frame (%.1f fps) at %dx%d, %.2fs..%.2fs\n", args.bench,
                    ms, 1000.0 / (ms > 1e-6 ? ms : 1e-6), args.width, args.height, bFrom, bTo);
        gl::destroyContext();
        return 0;
    }

    if (!args.mixAudio.empty()) {
        std::string music = args.audio;
        if (music.empty()) music = autoMusicPath(args.chart);
        audio::MixOptions mo;
        mo.musicVolume = args.musicVolume;
        mo.notesVolume = args.notesVolume;
        mo.hitDir = args.assets + "/Notes";
        bool ok = audio::mixChartAudio(renderer.chart(), music, args.mixAudio, mo, &err);
        if (!ok) std::fprintf(stderr, "audio mix failed: %s\n", err.c_str());
        gl::destroyContext();
        return ok ? 0 : 1;
    }

    if (!args.video.empty()) {
        bool ok = renderVideo(renderer, args);
        gl::destroyContext();
        return ok ? 0 : 1;
    }

    if (!args.outDir.empty()) {
        float to = targetDuration(renderer, args.to);
        if (to <= args.from) to = args.from + 1.0f;
        int frames = (int)std::lround((double)(to - args.from) * (double)args.fps);
        std::printf("sequence: %d frames from %.2fs to %.2fs @ %.1f fps -> %s\n", frames, args.from,
                    to, args.fps, args.outDir.c_str());
        double renderMs = 0.0;
        for (int i = 0; i < frames; ++i) {
            float t = args.from + (float)i / args.fps;
            auto t0 = std::chrono::high_resolution_clock::now();
            renderer.render(t);
            auto t1 = std::chrono::high_resolution_clock::now();
            renderMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            char path[512];
            std::snprintf(path, sizeof(path), "%s/frame_%05d.png", args.outDir.c_str(), i);
            if (!renderer.savePng(path)) {
                std::fprintf(stderr, "failed to save %s\n", path);
                return 1;
            }
            if ((i % 30) == 0) std::printf("  frame %d/%d t=%.2f\n", i, frames, t);
        }
        std::printf("done: avg render %.2f ms/frame\n", renderMs / (double)frames);
    } else {
        renderer.render(args.time);
        if (!renderer.savePng(args.out)) {
            std::fprintf(stderr, "failed to save %s\n", args.out.c_str());
            return 1;
        }
        std::printf("saved %s (%.2fs) lines=%d taps=%d holds=%d zones=%d/%d/%d\n", args.out.c_str(),
                    args.time, renderer.dbgLines, renderer.dbgTapQuads, renderer.dbgHoldQuads,
                    renderer.dbgZonesA, renderer.dbgZonesD, renderer.dbgZonesR);
        if (!args.debugNoise.empty()) {
            std::string derr;
            if (!renderer.debugNoiseDump(args.debugNoise, &derr))
                std::fprintf(stderr, "debug dump failed: %s\n", derr.c_str());
            else
                std::printf("debug noise dump -> %s*.png\n", args.debugNoise.c_str());
        }
    }

    gl::destroyContext();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    // Every failure path already reports through `error`; this is the last line of
    // defence for anything that still throws (bad_alloc, filesystem, ...).  It
    // costs nothing per frame: it runs once per process.
    try {
        return runMain(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "fatal: unknown exception\n");
        return 1;
    }
}
