// audio.h - offline audio mixing (music track + per-note hit sounds).
#pragma once

#include <string>
#include <vector>

struct Chart;

namespace audio {

struct MixOptions {
    float musicVolume = 1.0f;
    float notesVolume = 0.5f;
    std::string hitDir = "assets/default/Notes";  // Tap.wav / Drag.wav / Flick.wav
    int sampleRate = 44100;
    float leadIn = 0.0f;  // silence prepended for the intro showcase screen
    // Export window in chart seconds. 0 duration means "to the end of the music".
    // The window is what gets mixed and written, so a clip's audio starts at its
    // own first frame instead of at 0:00 of the song.
    float startSec = 0.0f;
    float durationSec = 0.0f;
};

// Decodes any audio file to interleaved float32 PCM through ffmpeg.
bool decodeWithFfmpeg(const std::string& path, int sampleRate, int channels,
                      std::vector<float>& out, std::string* error);

// Loads a RIFF/WAVE PCM16 file (what the official hit sounds use).
bool loadWav16(const std::string& path, std::vector<float>& out, int* channels, int* sampleRate,
               std::string* error);

// Mixes the song track with every note's hit sound and writes a float32 WAV.
bool mixChartAudio(const Chart& chart, const std::string& musicPath, const std::string& outWavPath,
                   const MixOptions& opts, std::string* error);

}  // namespace audio
