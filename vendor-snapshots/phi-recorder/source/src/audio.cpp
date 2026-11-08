// audio.cpp - offline audio mixing (music track + per-note hit sounds).
#include "audio.h"

#include "chart.h"
#include "common.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace audio {

namespace {

uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void wr32(std::vector<unsigned char>& v, uint32_t x) {
    v.push_back((unsigned char)(x & 0xff));
    v.push_back((unsigned char)((x >> 8) & 0xff));
    v.push_back((unsigned char)((x >> 16) & 0xff));
    v.push_back((unsigned char)((x >> 24) & 0xff));
}

void wr16(std::vector<unsigned char>& v, uint16_t x) {
    v.push_back((unsigned char)(x & 0xff));
    v.push_back((unsigned char)((x >> 8) & 0xff));
}

// The official UI.json note delays (seconds) for tap / drag / flick.
constexpr float kNoteDelay[3] = {0.00061239f, 0.03229180f, 0.00183684f};

}  // namespace

bool decodeWithFfmpeg(const std::string& path, int sampleRate, int channels,
                      std::vector<float>& out, std::string* error) {
    out.clear();
    std::string cmd = "ffmpeg -v error -i \"" + path + "\" -f f32le -acodec pcm_f32le -ar " +
                      std::to_string(sampleRate) + " -ac " + std::to_string(channels) + " - 2>NUL";
    FILE* pipe = _popen(cmd.c_str(), "rb");
    if (!pipe) {
        if (error) *error = "cannot spawn ffmpeg for " + path;
        return false;
    }
#ifdef _WIN32
    _setmode(_fileno(pipe), _O_BINARY);
#endif
    float buf[4096];
    while (true) {
        size_t got = std::fread(buf, sizeof(float), 4096, pipe);
        out.insert(out.end(), buf, buf + got);
        if (got < 4096) break;
    }
    int rc = _pclose(pipe);
    if (rc != 0 || out.empty()) {
        if (error) *error = "ffmpeg failed to decode " + path;
        return false;
    }
    // Interleaved samples must be a whole number of frames.
    size_t frames = out.size() / (size_t)channels;
    out.resize(frames * (size_t)channels);
    return true;
}

bool loadWav16(const std::string& path, std::vector<float>& out, int* channels, int* sampleRate,
               std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 44) {
        std::fclose(f);
        if (error) *error = "wav too short: " + path;
        return false;
    }
    std::vector<unsigned char> bytes((size_t)len);
    size_t rd = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (rd != bytes.size()) {
        if (error) *error = "wav read error: " + path;
        return false;
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        if (error) *error = "not a RIFF/WAVE file: " + path;
        return false;
    }
    int fmtChannels = 1, fmtRate = 44100, bits = 16;
    size_t dataOffset = 0, dataSize = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        uint32_t chunkSize = rd32(&bytes[pos + 4]);
        if (std::memcmp(&bytes[pos], "fmt ", 4) == 0) {
            if (pos + 8 + 16 <= bytes.size()) {
                fmtChannels = rd16(&bytes[pos + 8 + 2]);
                fmtRate = (int)rd32(&bytes[pos + 8 + 4]);
                bits = rd16(&bytes[pos + 8 + 14]);
            }
        } else if (std::memcmp(&bytes[pos], "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkSize;
        }
        pos += 8 + chunkSize + (chunkSize & 1u);
    }
    if (dataOffset == 0 || dataSize == 0) {
        if (error) *error = "wav has no data chunk: " + path;
        return false;
    }
    if (bits != 16) {
        if (error) *error = "only 16-bit wav supported: " + path;
        return false;
    }
    if (dataOffset + dataSize > bytes.size()) dataSize = bytes.size() - dataOffset;
    size_t samples = dataSize / 2;
    out.resize(samples);
    const unsigned char* p = bytes.data() + dataOffset;
    for (size_t i = 0; i < samples; ++i) {
        int16_t s = (int16_t)((uint16_t)p[i * 2] | ((uint16_t)p[i * 2 + 1] << 8));
        out[i] = (float)s / 32768.0f;
    }
    if (channels) *channels = fmtChannels;
    if (sampleRate) *sampleRate = fmtRate;
    return true;
}

bool mixChartAudio(const Chart& chart, const std::string& musicPath, const std::string& outWavPath,
                   const MixOptions& opts, std::string* error) {
    const int rate = opts.sampleRate;
    const int ch = 2;
    std::vector<float> music;
    if (!musicPath.empty()) {
        if (!decodeWithFfmpeg(musicPath, rate, ch, music, error)) return false;
    }
    if (music.empty()) {
        // No music track: use the chart duration so the video still has a track.
        size_t frames = (size_t)(chart.duration > 0.0f ? chart.duration : 175.0f) * (size_t)rate;
        music.assign(frames * (size_t)ch, 0.0f);
    }

    // Export only the requested window. A 10 s excerpt must not mix and write
    // three minutes of audio, and the clip's track has to start at its own first
    // frame instead of always at 0:00 of the song.
    {
        const size_t haveFrames = music.size() / (size_t)ch;
        const long long startFrame =
            opts.startSec > 0.0f ? (long long)std::llround((double)opts.startSec * (double)rate) : 0;
        const size_t first = startFrame > 0 ? (size_t)startFrame : 0;
        size_t want = 0;
        if (opts.durationSec > 0.0f) {
            const long long dur = (long long)std::llround((double)opts.durationSec * (double)rate);
            want = dur > 0 ? (size_t)dur : 0;
        } else {
            want = first < haveFrames ? haveFrames - first : 0;
        }
        const size_t avail = first < haveFrames ? haveFrames - first : 0;
        const size_t take = avail < want ? avail : want;
        std::vector<float> window;
        window.reserve(want * (size_t)ch);
        if (take > 0) {
            window.assign(music.begin() + (std::ptrdiff_t)(first * (size_t)ch),
                          music.begin() + (std::ptrdiff_t)((first + take) * (size_t)ch));
        }
        if (want > take) window.resize(want * (size_t)ch, 0.0f);  // pad past the end
        music.swap(window);
    }

    const std::string hitDir = opts.hitDir;
    std::vector<float> tap, drag, flick;
    int c = 0, r = 0;
    std::string ignored;
    bool haveTap = loadWav16(hitDir + "/Tap.wav", tap, &c, &r, &ignored);
    bool haveDrag = loadWav16(hitDir + "/Drag.wav", drag, &c, &r, &ignored);
    bool haveFlick = loadWav16(hitDir + "/Flick.wav", flick, &c, &r, &ignored);
    if (!haveTap && !haveDrag && !haveFlick) {
        if (error) *error = "no hit sounds found in " + hitDir;
        return false;
    }

    // Mix the song track and collect the hit layer separately so that the note
    // sounds can be balanced against the music without attenuating the song.
    for (size_t i = 0; i < music.size(); ++i) music[i] *= opts.musicVolume;
    std::vector<float> hits(music.size(), 0.0f);

    auto addSound = [&](const std::vector<float>& src, int srcChannels, float atSeconds, int type) {
        if (src.empty() || srcChannels <= 0) return;
        float delay = kNoteDelay[type >= 1 && type <= 3 ? type - 1 : 0];
        long startFrame = (long)((atSeconds - opts.startSec - delay) * (double)rate);
        size_t frames = src.size() / (size_t)srcChannels;
        // 2.5 ms fade-in: the raw hit clips start at full amplitude, and that
        // near-instant attack makes the AAC encoder ring well above full scale.
        const size_t fade = (size_t)(0.0025 * (double)rate);
        for (size_t i = 0; i < frames; ++i) {
            float gain = (i < fade) ? (float)i / (float)fade : 1.0f;
            long dstFrame = startFrame + (long)i;
            if (dstFrame < 0) continue;
            size_t dst = (size_t)dstFrame * (size_t)ch;
            if (dst + 1 >= hits.size()) break;
            for (int cc = 0; cc < ch; ++cc) {
                int scc = cc < srcChannels ? cc : srcChannels - 1;
                hits[dst + cc] +=
                    src[i * (size_t)srcChannels + (size_t)scc] * opts.notesVolume * gain;
            }
        }
    };

    int noteCount = 0;
    for (const JudgeLine& line : chart.lines) {
        const std::vector<Note>* lists[2] = {&line.notesAbove, &line.notesBelow};
        for (int side = 0; side < 2; ++side) {
            for (const Note& n : *lists[side]) {
                const float t = n.secTime;
                // Notes outside the export window contribute nothing; counting
                // them would also make the "notes mixed" line a lie.
                if (t < opts.startSec - 0.5f) continue;
                if (opts.durationSec > 0.0f && t > opts.startSec + opts.durationSec + 0.5f) continue;
                if (n.type == 3) {
                    if (haveTap) addSound(tap, 2, t, 1);
                } else if (n.type == 2) {
                    if (haveDrag) addSound(drag, 2, t, 2);
                } else if (n.type == 4) {
                    if (haveFlick) addSound(flick, 2, t, 4);
                } else {
                    if (haveTap) addSound(tap, 2, t, 1);
                }
                ++noteCount;
            }
        }
    }

    // Balance the hit layer against the music by RMS.  The old code also
    // clamped the hit gain so that `hitPeak * hitGain <= 0.5`; on dense AT
    // charts the hit layer peaks near 4.4 (chords), so that clamp pushed the
    // whole hit layer down to ~0.11 and made every note sound inaudible.
    // Instead the mix goes through a peak limiter, which only touches the few
    // loudest moments and keeps single hits at full level.
    double musicSq = 0.0, hitSq = 0.0, musicPeak = 0.0, hitPeak = 0.0;
    for (size_t i = 0; i < music.size(); ++i) {
        double a = music[i] < 0 ? -(double)music[i] : (double)music[i];
        double b = hits[i] < 0 ? -(double)hits[i] : (double)hits[i];
        musicSq += a * a;
        hitSq += b * b;
        if (a > musicPeak) musicPeak = a;
        if (b > hitPeak) hitPeak = b;
    }
    const double n = (double)music.size() > 0 ? (double)music.size() : 1.0;
    double musicRms = std::sqrt(musicSq / n);
    double hitRms = std::sqrt(hitSq / n);
    float musicGain = musicPeak > 0.0001 ? (float)(0.86 / musicPeak) : 1.0f;
    if (musicGain > 8.0f) musicGain = 8.0f;
    // Hits sit a few dB under the music's RMS so single notes are clearly audible
    // without burying the song.
    float hitGain = 1.0f;
    if (hitRms > 1e-6) {
        double targetRms = 0.8 * musicRms * musicGain;
        hitGain = (float)(targetRms / hitRms);
        if (hitGain < 0.02f) hitGain = 0.02f;
        if (hitGain > 2.5f) hitGain = 2.5f;
    }
    // Peak limiter (instant attack, 80 ms release) replaces the flat master trim.
    const float kCeil = 0.72f;
    const float release = std::exp(-1.0f / (0.080f * (float)rate));
    float env = 0.0f, peak = 0.0f;
    for (size_t i = 0; i + (size_t)ch <= music.size(); i += (size_t)ch) {
        for (int cc = 0; cc < ch; ++cc) {
            float v = music[i + cc] * musicGain + hits[i + cc] * hitGain;
            float a = std::fabs(v);
            env = a > env ? a : env * release;
            float g = env > kCeil ? kCeil / env : 1.0f;
            float o = v * g;
            music[i + cc] = o;
            if (std::fabs(o) > peak) peak = std::fabs(o);
        }
    }
    std::printf("audio: music rms=%.3f peak=%.3f gain=%.2f | hits rms=%.3f peak=%.3f gain=%.2f | mix peak=%.2f\n",
                musicRms, musicPeak, musicGain, hitRms, hitPeak, hitGain, peak);

    // Optional intro lead-in: silence so the intro showcase screen is quiet.
    if (opts.leadIn > 0.0f) {
        size_t leadFrames = (size_t)(opts.leadIn * (float)rate);
        music.insert(music.begin(), leadFrames * (size_t)ch, 0.0f);
    }

    // Write float32 WAV.
    std::vector<unsigned char> wav;
    uint32_t dataBytes = (uint32_t)(music.size() * sizeof(float));
    wav.reserve(44 + dataBytes);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    wr32(wav, 36 + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    wr32(wav, 16);
    wr16(wav, 3);  // IEEE float
    wr16(wav, (uint16_t)ch);
    wr32(wav, (uint32_t)rate);
    wr32(wav, (uint32_t)(rate * ch * (int)sizeof(float)));
    wr16(wav, (uint16_t)(ch * sizeof(float)));
    wr16(wav, 32);  // bits per sample
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    wr32(wav, dataBytes);
    size_t base = wav.size();
    wav.resize(base + dataBytes);
    float* dst = (float*)(void*)(wav.data() + base);
    for (size_t i = 0; i < music.size(); ++i) dst[i] = music[i];

    FILE* f = std::fopen(outWavPath.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot write " + outWavPath;
        return false;
    }
    bool ok = std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
    std::fclose(f);
    if (!ok) {
        if (error) *error = "failed to write " + outWavPath;
        return false;
    }
    std::printf("audio: %d notes mixed, %d frames -> %s\n", noteCount, (int)(music.size() / 2),
                outWavPath.c_str());
    return true;
}

}  // namespace audio
