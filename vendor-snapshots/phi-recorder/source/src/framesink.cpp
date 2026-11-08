// The encoder threads use condition variables, which windows.h only declares
// for Vista and newer. _mingw.h pins _WIN32_WINNT to 0x502 as soon as any libc
// header is included, so the target has to be set before everything else.
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#endif

#include "framesink.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace framesink {
namespace {

// ---------------------------------------------------------------- threading
// This toolchain is MinGW built with the win32 thread model, so std::thread /
// std::mutex are not available; the Win32 primitives below are what the
// encoders need and nothing more.

struct Mutex {
    CRITICAL_SECTION cs;
    Mutex() { InitializeCriticalSection(&cs); }
    ~Mutex() { DeleteCriticalSection(&cs); }
    void lock() { EnterCriticalSection(&cs); }
    void unlock() { LeaveCriticalSection(&cs); }
};

struct Cond {
    CONDITION_VARIABLE cv;
    Cond() { InitializeConditionVariable(&cv); }
    void wait(Mutex& m) { SleepConditionVariableCS(&cv, &m.cs, INFINITE); }
    void notifyOne() { WakeConditionVariable(&cv); }
    void notifyAll() { WakeAllConditionVariable(&cv); }
};

struct Thread {
    HANDLE handle = nullptr;
    std::function<void()> body;
    static DWORD WINAPI trampoline(LPVOID self) {
        static_cast<Thread*>(self)->body();
        return 0;
    }
    bool start() {
        handle = CreateThread(nullptr, 0, &Thread::trampoline, this, 0, nullptr);
        return handle != nullptr;
    }
    void join() {
        if (!handle) return;
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        handle = nullptr;
    }
    ~Thread() { join(); }
};

int cpuCount() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

// Where the concat demuxer resolves relative entries: measured on both the 2013
// build (N-55702) and a 2020 4.2 build, a bare name is joined onto the *list
// file's own directory*, and a Windows absolute entry ('D:/...') is joined onto
// that directory too, which breaks. So entries must be bare names and the list
// must sit in the same directory as the segments -- which is where this writes
// it. (The 4.x+ source calls ff_make_absolute_url(), so the rule is documented
// upstream, but the 2013 build already behaved this way.)
std::string baseName(const std::string& p) {
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// Runs a command to completion and returns its exit code. ffmpeg writes to
// stderr, so the log is captured too: a silent "failed" is useless.
int runCommand(const std::string& cmd, std::string* log = nullptr) {
    FILE* p = _popen((cmd + " 2>&1").c_str(), "rb");
    if (!p) return -1;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        if (log) log->append(buf, n);
    }
    return _pclose(p);
}

// Audio encoders that can exist on the other side of the pipe, in the order this
// project prefers them. libvo_aacenc is what the 2013 build shipped and what
// earlier versions of this tool wrote; ffmpeg 3.0 removed it in favour of the
// native encoder, which on the old build is still marked experimental and needs
// -strict -2 while new builds ignore the flag. Probing with `ffmpeg -encoders`
// would cost a full process spawn (~1 s through the python launcher shim), so
// the list is tried in order and a codec that the binary does not know is
// skipped when the failure says so -- see retryable() below.
std::vector<std::string> audioCodecCandidates() {
    std::vector<std::string> cands;
    cands.push_back(" -c:a libvo_aacenc -b:a 192k -af apad");
    cands.push_back(" -c:a aac -strict -2 -b:a 192k -af apad");
    // For a build that finally dropped -strict.
    cands.push_back(" -c:a aac -b:a 192k -af apad");
    return cands;
}

// A mux that failed because this ffmpeg build does not carry the encoder (or
// wants -strict) is worth retrying with the next candidate; anything else (a
// missing input, a full disk) is a real error and must be reported as such.
bool codecMismatch(const std::string& log) {
    return log.find("Unknown encoder") != std::string::npos ||
           log.find("experimental") != std::string::npos ||
           log.find("Encoder (codec aac) not found") != std::string::npos ||
           log.find("Unrecognized option") != std::string::npos ||
           log.find("Unknown option") != std::string::npos;
}

// ---------------------------------------------------------------- encoders

// Slots per encoder: one being filled by the renderer, one being written by
// this encoder's thread. Two is enough to keep both sides busy.
// Must stay >= the producer's in-flight depth (main.cpp kDepth = 3): with one
// encoder a smaller ring deadlocks, because every slot can be "filling" before
// the producer is allowed to submit the oldest one back.
const int kSlots = 4;

struct Encoder {
    FILE* pipe = nullptr;
    Thread thread;
    Mutex mu;
    Cond cv;
    std::vector<std::vector<unsigned char>> bufs;
    std::vector<int> state;  // 0 free, 1 filling, 2 full, 3 writing
    int fillIdx = 0;
    int writeIdx = 0;
    bool closing = false;
    bool failed = false;
    std::string path;
    std::string logPath;
    long long framesWritten = 0;
};

void writerLoop(Encoder* e, size_t bytes) {
    for (;;) {
        int idx = -1;
        e->mu.lock();
        for (;;) {
            if (e->state[e->writeIdx] == 2) break;
            if (e->closing) {
                e->mu.unlock();
                return;
            }
            e->cv.wait(e->mu);
        }
        idx = e->writeIdx;
        e->state[idx] = 3;
        e->writeIdx = (idx + 1) % (int)e->state.size();
        e->mu.unlock();

        bool ok = std::fwrite(e->bufs[idx].data(), 1, bytes, e->pipe) == bytes;
        ++e->framesWritten;

        e->mu.lock();
        e->state[idx] = 0;
        if (!ok) e->failed = true;
        e->mu.unlock();
        e->cv.notifyAll();
        if (!ok) return;
    }
}

std::string readTail(const std::string& path, size_t maxBytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string all;
    char buf[1024];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    std::fclose(f);
    if (all.size() > maxBytes) all = all.substr(all.size() - maxBytes);
    return all;
}

int pickSegments(int totalFrames) {
    if (const char* env = std::getenv("PHI_SEGMENTS")) {
        int n = std::atoi(env);
        if (n >= 1) return std::min(n, std::max(1, totalFrames));
    }
    // x264 wants a few threads per process to fill its lookahead, so the box
    // saturates at roughly one encoder per 6 hardware threads: more processes
    // than that fight over cores and each one encodes slower (measured: 3-4
    // encoders beat 8 on a 20-thread machine).
    int n = std::max(1, cpuCount() / 6);
    n = std::min(n, 8);
    // Startup costs ~0.2 s per encoder; do not split shorter than a second.
    const int kMinFramesPerSegment = 24;
    n = std::min(n, std::max(1, totalFrames / kMinFramesPerSegment));
    return std::max(1, n);
}

}  // namespace

// ------------------------------------------------------------------- Impl

struct FrameSink::Impl {
    Options opt;
    size_t frameBytes = 0;
    int segments = 0;
    int stepSpan = 1;  // frames per segment; the last segment may be shorter
    std::vector<std::unique_ptr<Encoder>> encs;
    long long submitted = 0;

    // Frame f belongs to segment f / stepSpan. The producer walks the segments
    // step by step instead of frame by frame, which is what keeps every encoder
    // fed at once: each one still receives its own contiguous, ordered range,
    // so the concatenated result is in time order.
    int encoderFor(int frame) const {
        if (segments <= 1) return 0;
        int idx = frame / (stepSpan > 0 ? stepSpan : 1);
        if (idx < 0) idx = 0;
        if (idx >= segments) idx = segments - 1;
        return idx;
    }
};

FrameSink::FrameSink() : impl_(new Impl) {}

FrameSink::~FrameSink() {
    // Finish is the normal path; this only has to free processes and buffers.
    if (!impl_) return;
    for (auto& e : impl_->encs) {
        if (!e) continue;
        e->mu.lock();
        e->closing = true;
        e->mu.unlock();
        e->cv.notifyAll();
        e->thread.join();
        if (e->pipe) {
            _pclose(e->pipe);
            e->pipe = nullptr;
        }
    }
}

void FrameSink::setAudioPath(const std::string& path) {
    if (impl_) impl_->opt.audioPath = path;
}

int FrameSink::segments() const { return impl_ ? impl_->segments : 0; }
int FrameSink::stepsPerSegment() const { return impl_ ? impl_->stepSpan : 0; }
int FrameSink::frameFor(int segment, int step) const {
    if (!impl_) return -1;
    const Impl& im = *impl_;
    if (segment < 0 || segment >= im.segments || step < 0 || step >= im.stepSpan) return -1;
    const int frame = segment * im.stepSpan + step;
    return frame < im.opt.totalFrames ? frame : -1;
}
size_t FrameSink::frameBytes() const { return impl_ ? impl_->frameBytes : 0; }
long long FrameSink::framesSubmitted() const { return impl_ ? impl_->submitted : 0; }

bool FrameSink::start(const Options& opt, std::string* err) {
    Impl& im = *impl_;
    im.opt = opt;
    const bool yuv = opt.pipeFormat != "rgba";
    im.frameBytes = yuv ? (size_t)opt.width * (size_t)opt.height * 3u / 2u
                        : (size_t)opt.width * (size_t)opt.height * 4u;
    im.segments = pickSegments(opt.totalFrames);
    if (opt.segments > 0) im.segments = std::min(opt.segments, std::max(1, opt.totalFrames));
    // Every segment but the last holds stepSpan frames; frame f belongs to
    // segment f / stepSpan. Main renders the segments interleaved (one frame of
    // each per round) so all encoders run at the same time while each still
    // receives its own contiguous, in-order range.
    im.stepSpan = (opt.totalFrames + im.segments - 1) / std::max(1, im.segments);
    if (im.stepSpan < 1) im.stepSpan = 1;

    int threadsPerSeg = opt.threadsPerSegment;
    if (threadsPerSeg <= 0) {
        // Total encoder threads should stay near the core count: more just adds
        // context switching, fewer leaves cores idle.
        threadsPerSeg = std::max(2, std::min(8, cpuCount() / std::max(1, im.segments)));
        // Escape hatch: a few libx264 builds stall with -threads larger than the
        // lookahead can feed (sync-lookahead=0 makes that queue small), so the
        // thread count must stay tunable without a rebuild.
        if (const char* env = std::getenv("PHI_ENCODER_THREADS")) {
            int v = std::atoi(env);
            if (v > 0) threadsPerSeg = v;
        }
    }

    for (int s = 0; s < im.segments; ++s) {
        std::unique_ptr<Encoder> e(new Encoder);
        for (int k = 0; k < kSlots; ++k) e->bufs.emplace_back(im.frameBytes);
        e->state.assign(kSlots, 0);
        // Always encode into a segment file: the audio mux must never read and
        // write the same path, and a failed run cannot leave a half mp4 behind.
        e->path = opt.outPath + ".seg" + std::to_string(s) + ".mp4";

        std::string cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt " + opt.pipeFormat + " -s " +
                          std::to_string(opt.width) + "x" + std::to_string(opt.height) +
                          " -r " + std::to_string(opt.fps) + " -i - -an";
        // The renderer already delivers the YUV planes top-down; only the RGBA
        // path needs ffmpeg to flip the GL-style bottom-up image.
        std::string filter = yuv ? "" : "vflip";
        if(opt.outputWidth>0 && opt.outputHeight>0 && (opt.outputWidth!=opt.width||opt.outputHeight!=opt.height)){
            if(!filter.empty())filter+=",";
            filter+="scale="+std::to_string(opt.outputWidth)+":"+std::to_string(opt.outputHeight)+":flags=lanczos";
        }
        if(!filter.empty())cmd+=" -vf "+filter;
        if(opt.nvenc) cmd+=" -c:v h264_nvenc -preset p4 -tune hq -rc vbr -cq 19 -b:v 0";
        else cmd+=" -c:v libx264 -preset "+opt.preset+" -crf "+std::to_string(opt.crf)+
            " -threads "+std::to_string(threadsPerSeg)+" -x264-params rc-lookahead=10:sync-lookahead=0";
        cmd+=" -pix_fmt yuv420p";
        // ffmpeg reports failures on stderr, which the data pipe cannot carry:
        // keep it in a side log so a dead encoder explains itself.
        e->logPath = e->path + ".log";
        cmd += " " + quote(e->path) + " 2>" + quote(e->logPath);

        e->pipe = _popen(cmd.c_str(), "wb");
        if (!e->pipe) {
            if (err) *err = "cannot spawn ffmpeg";
            return false;
        }
#ifdef _WIN32
        _setmode(_fileno(e->pipe), _O_BINARY);
#endif
        Encoder* raw = e.get();
        e->thread.body = [raw, bytes = im.frameBytes]() { writerLoop(raw, bytes); };
        if (!e->thread.start()) {
            if (err) *err = "cannot start encoder thread";
            _pclose(e->pipe);
            e->pipe = nullptr;
            return false;
        }
        im.encs.push_back(std::move(e));
    }
    return true;
}

unsigned char* FrameSink::acquire(int frame, int* slot) {
    Impl& im = *impl_;
    Encoder& e = *im.encs[im.encoderFor(frame)];
    e.mu.lock();
    for (;;) {
        if (e.state[e.fillIdx] == 0) break;
        if (e.failed || e.closing) {
            e.mu.unlock();
            return nullptr;
        }
        e.cv.wait(e.mu);
    }
    const int idx = e.fillIdx;
    e.state[idx] = 1;
    e.fillIdx = (idx + 1) % (int)e.state.size();
    e.mu.unlock();
    if (slot) *slot = idx;
    return e.bufs[idx].data();
}

void FrameSink::submit(int frame, int slot) {
    Impl& im = *impl_;
    Encoder& e = *im.encs[im.encoderFor(frame)];
    e.mu.lock();
    e.state[slot] = 2;
    e.mu.unlock();
    ++im.submitted;
    e.cv.notifyAll();
}

bool FrameSink::finish(std::string* err) {
    Impl& im = *impl_;
    for (auto& e : im.encs) {
        e->mu.lock();
        e->closing = true;
        e->mu.unlock();
        e->cv.notifyAll();
    }
    bool ok = true;
    for (auto& e : im.encs) {
        e->thread.join();
        if (e->pipe) {
            int rc = _pclose(e->pipe);
            e->pipe = nullptr;
            if (rc != 0 || e->failed) {
                if (err) *err = "ffmpeg exited with " + std::to_string(rc) + ": " + readTail(e->logPath, 600);
                ok = false;
            }
        }
    }
    if (!ok) {
        for (auto& e : im.encs) std::remove(e->path.c_str());
        return false;
    }

    // One ffmpeg pass does both jobs: concat the segments with a stream copy and,
    // when there is music, mux the audio in. Two passes used to write and reread
    // a full intermediate mp4 for no reason.
    std::string listPath;
    std::string input;
    if (im.segments > 1) {
        // Bare names resolved against the list's directory (see baseName), so
        // the list is written next to the segments.
        listPath = im.opt.outPath + ".concat.txt";
        FILE* list = std::fopen(listPath.c_str(), "wb");
        if (!list) {
            if (err) *err = "cannot write concat list";
            return false;
        }
        for (auto& e : im.encs)
            std::fprintf(list, "file '%s'\n", baseName(e->path).c_str());
        std::fclose(list);
        input = "-f concat -safe 0 -i " + quote(listPath);
    } else {
        input = "-i " + quote(im.encs[0]->path);
    }

    const bool haveAudio = !im.opt.audioPath.empty();
    if (!haveAudio && im.segments == 1) {
        // Nothing to join and nothing to mux: the segment is the result.
        std::remove(im.opt.outPath.c_str());
        if (std::rename(im.encs[0]->path.c_str(), im.opt.outPath.c_str()) != 0) {
            if (err) *err = "cannot move the encoded video into place";
            return false;
        }
        std::remove(im.encs[0]->logPath.c_str());
        return true;
    }

    // The video length is whatever was encoded; asking for it explicitly keeps
    // ffmpeg from running forever: `-af apad` never ends on its own and
    // `-shortest` cannot help when the concat demuxer reports no duration
    // (observed as a mux that hangs after writing the video track).
    const double seconds = im.opt.fps > 0.0f
                               ? (double)im.opt.totalFrames / (double)im.opt.fps
                               : 0.0;
    std::string cmd = "ffmpeg -y -loglevel error " + input;
    if (haveAudio) {
        char len[32];
        std::snprintf(len, sizeof(len), " -t %.3f", seconds > 0.0 ? seconds : 0.0);
        cmd += " -i " + quote(im.opt.audioPath) + " -map 0:v:0 -map 1:a:0 -c:v copy";
        if (seconds > 0.0) cmd += len;
    } else {
        cmd += " -map 0:v:0 -c:v copy";
    }

    // The audio encoder is the one piece that moved between ffmpeg generations,
    // so start with the one this project has always used and step down only when
    // the binary says it does not carry it.
    std::string log;
    int rc = -1;
    if (haveAudio) {
        static int first = 0;  // the candidate that worked last time
        std::vector<std::string> cands = audioCodecCandidates();
        for (size_t i = (size_t)first; i < cands.size(); ++i) {
            log.clear();
            rc = runCommand(cmd + cands[i] + " " + quote(im.opt.outPath), &log);
            if (rc == 0) {
                first = (int)i;
                break;
            }
            if (!codecMismatch(log)) break;
        }
    } else {
        rc = runCommand(cmd + " " + quote(im.opt.outPath), &log);
    }
    if (!listPath.empty()) std::remove(listPath.c_str());
    if (rc != 0) {
        // Keep the segments: they are the only way to retry by hand.
        if (err) *err = (haveAudio ? "audio mux failed: " : "segment concat failed: ") + log;
        std::fprintf(stderr, "[mux] rc=%d cmd=%s\n", rc, cmd.c_str());
        return false;
    }
    for (auto& e : im.encs) {
        std::remove(e->path.c_str());
        std::remove(e->logPath.c_str());
    }
    return true;
}

}  // namespace framesink
