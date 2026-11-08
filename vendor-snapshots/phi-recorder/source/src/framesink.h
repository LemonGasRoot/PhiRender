// Frame sink: turns the renderer's readback into finished video files.
//
// One renderer thread produces RGBA8 frames; N encoder processes consume them
// through bounded per-encoder queues, so rendering and H.264 encoding overlap
// and the encoder work spreads over every core instead of one x264 instance.
// The segments are joined without re-encoding, then the audio track is muxed.
//
// The sink never changes pixels: frames reach ffmpeg as the same rawvideo RGBA
// stream the single-process path used to write.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace framesink {

struct Options {
    std::string outPath;      // final .mp4
    std::string audioPath;    // mixed wav (or the raw track); empty = silent
    int width = 1920;
    int height = 1080;
    int fps = 60;
    std::string preset = "medium";
    int crf = 18;
    bool nvenc = false;
    int outputWidth = 0, outputHeight = 0;
    // Raw frame layout on the pipe: "rgba" (bottom-up, ffmpeg flips it) or
    // "yuv420p" (top-down planes produced by the renderer, no filter needed).
    std::string pipeFormat = "nv12";
    int totalFrames = 0;
    int segments = 0;          // 0 = derive from the machine
    int threadsPerSegment = 0; // 0 = derive from segments
};

class FrameSink {
public:
    FrameSink();
    ~FrameSink();
    FrameSink(const FrameSink&) = delete;
    FrameSink& operator=(const FrameSink&) = delete;

    // Spawns the encoder processes and their writer threads.
    bool start(const Options& opt, std::string* err);

    // Returns where frame `frame` should be read back to. Blocks until the
    // encoder that owns that frame has room. Returns null if it died.
    unsigned char* acquire(int frame, int* slot);

    // Hands a filled slot to its writer thread.
    void submit(int frame, int slot);

    // Joins the writers, concatenates the segments, muxes the audio.
    bool finish(std::string* err);

    // The mix may finish (or fail) after start(); this replaces the audio path
    // that finish() will mux.
    void setAudioPath(const std::string& path);
    int segments() const;
    // Interleaved schedule: render step s of every segment in turn. Segment k of
    // step s is frame k*stepsPerSegment()+s, or -1 past the last frame.
    int stepsPerSegment() const;
    int frameFor(int segment, int step) const;
    size_t frameBytes() const;
    long long framesSubmitted() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace framesink
