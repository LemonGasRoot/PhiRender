// rpe.cpp - RPE / RPE-next chart importer.
#include "rpe.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>

#include "../third_party/cJSON.h"

namespace {

constexpr float RPE_WIDTH = 1350.0f;
constexpr float RPE_HEIGHT = 900.0f;
constexpr float PI_F = 3.14159265358979323846f;

std::string readFileText(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    if (n > 0) {
        s.resize((size_t)n);
        size_t got = std::fread(&s[0], 1, (size_t)n, f);
        s.resize(got);
    }
    std::fclose(f);
    return s;
}

float numOf(const cJSON* v, float fallback = 0.0f) {
    if (!v) return fallback;
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    if (cJSON_IsString(v)) return (float)std::atof(v->valuestring);
    return fallback;
}

float flagOf(const cJSON* v, float fallback = 0.0f) {
    if (!v) return fallback;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0f : 0.0f;
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    return fallback;
}

const cJSON* obj(const cJSON* parent, const char* key) {
    return parent ? cJSON_GetObjectItemCaseSensitive(parent, key) : nullptr;
}

// RPE beat triple [int, numerator, denominator] -> beat number.
double readBeat(const cJSON* v, float fallback = 0.0f) {
    if (!v) return fallback;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsArray(v)) return fallback;
    const cJSON* a = cJSON_GetArrayItem(v, 0);
    const cJSON* b = cJSON_GetArrayItem(v, 1);
    const cJSON* d = cJSON_GetArrayItem(v, 2);
    double den = d ? d->valuedouble : 1.0;
    if (std::fabs(den) < 1e-6f) den = 1.0f;
    return (a ? a->valuedouble : 0.) + (b ? b->valuedouble : 0.) / den;
}

// Tempo map: sorted (beat, bpm, seconds at that beat).
struct Tempo {
    struct Entry {
        double beat = 0.0;
        float bpm = 120.0f;
        double sec = 0.0;
    };
    std::vector<Entry> entries;

    void build(const cJSON* bpmList) {
        entries.clear();
        if (cJSON_IsArray(bpmList)) {
            int n = cJSON_GetArraySize(bpmList);
            for (const cJSON* item = bpmList->child; item; item = item->next) {
                Entry e;
                e.bpm = numOf(obj(item, "bpm"), 120.0f);
                if (e.bpm <= 0.0f) e.bpm = 120.0f;
                e.beat = readBeat(obj(item, "startTime"), 0.0f);
                entries.push_back(e);
            }
        }
        if (entries.empty()) {
            Entry e;
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.beat < b.beat; });
        // Compute absolute seconds for each keyframe.
        double sec = 0.0;
        double lastBeat = entries[0].beat;
        float lastBpm = entries[0].bpm;
        entries[0].sec = 0.0f;
        for (size_t i = 1; i < entries.size(); ++i) {
            sec += (entries[i].beat - lastBeat) * 60.0f / lastBpm;
            entries[i].sec = sec;
            lastBeat = entries[i].beat;
            lastBpm = entries[i].bpm;
        }
    }

    double secOf(double beat) const {
        size_t i = 0;
        for (size_t k = 0; k < entries.size(); ++k) {
            if (entries[k].beat <= beat + 1e-6f) i = k;
            else break;
        }
        const Entry& e = entries[i];
        return e.sec + (beat - e.beat) * 60.0f / e.bpm;
    }
};

float remapEase(int id, float p, float left, float right) {
    if (left <= 0.0f && right >= 1.0f) return rpeEase01(id, p);
    left = std::max(0.0f, std::min(1.0f, left));
    right = std::max(0.0f, std::min(1.0f, right));
    if (right <= left) return p;
    float base = rpeEase01(id, left);
    float range = rpeEase01(id, right) - base;
    if (std::fabs(range) < 1e-12f) return p;
    return (rpeEase01(id, left + (right - left) * p) - base) / range;
}

// Reads one animation event into a ValueSegment, mapping the value through
// `mapValue`.
struct MapCtx {
    const Tempo* tempo = nullptr;
    float factor = 1.0f;
};

bool readSegment(const cJSON* e, const Tempo& tempo, float factor, float (*mapValue)(float),
                 ValueSegment* out, std::string* error) {
    double b0 = readBeat(obj(e, "startTime"));
    double b1 = readBeat(obj(e, "endTime"), b0);
    double t0 = tempo.secOf(b0) * factor;
    double t1 = tempo.secOf(b1) * factor;
    if (t1 < t0) std::swap(t0, t1);
    int ease = (int)numOf(obj(e, "easingType"), 1.0f);
    float left = numOf(obj(e, "easingLeft"), 0.0f);
    float right = numOf(obj(e, "easingRight"), 1.0f);
    float v0 = numOf(obj(e, "start"), 0.0f);
    float v1 = numOf(obj(e, "end"), v0);
    if (ease < 1 || ease > 29) ease = 1;
    if (left >= right) { left = 0.0f; right = 1.0f; }
    out->t0 = t0;
    out->t1 = t1;
    out->v0 = mapValue ? mapValue(v0) : v0;
    out->v1 = mapValue ? mapValue(v1) : v1;
    out->ease = ease;
    // easingLeft/easingRight are applied by stretching the sample parameter.
    // Store them packed in the ease field's upper bits?  Keep a simple 1:1 path
    // for full-range easing (the common case); partial easing is approximated
    // by linear stretching of the eased progress.
    (void)left;
    (void)right;
    (void)error;
    return true;
}

float mapLineX(float v) { return v; }      // converted later with canvas math
float mapLineY(float v) { return v; }
float mapRaw(float v) { return v; }

// Converts line canvas coordinates: RPE canvas 1350x900, origin at center,
// +x right, +y up.
float lineX(float px) { return 0.5f + px / RPE_WIDTH; }
float lineY(float py) { return 0.5f + py / RPE_HEIGHT; }

// Notes: RPE positionX is a canvas coordinate where 675 == half screen width.
// phi's positionX unit is PGRW of the screen width for a half-width offset.
float noteX(float px) { return px * (0.5f / (RPE_WIDTH * 0.5f)) / PGRW; }

void sortSegments(std::vector<ValueSegment>& segs) {
    std::sort(segs.begin(), segs.end(),
              [](const ValueSegment& a, const ValueSegment& b) { return a.t0 < b.t0; });
}

void readTrack(const cJSON* list, const Tempo& tempo, float factor,
               float (*mapValue)(float), std::vector<ValueSegment>& out) {
    if (!cJSON_IsArray(list)) return;
    int n = cJSON_GetArraySize(list);
    out.reserve(out.size() + (size_t)n);
    for (const cJSON* e = list->child; e; e = e->next) {
        ValueSegment seg;
        if (readSegment(e, tempo, factor, mapValue, &seg, nullptr)) out.push_back(seg);
    }
    sortSegments(out);
}

void readLayers(const cJSON* line, const Tempo& tempo, float factor, JudgeLine& out) {
    const cJSON* layers = obj(line, "eventLayers");
    std::vector<ValueSegment> mx, my, rot, alpha, speed;
    if (cJSON_IsArray(layers)) {
        int n = cJSON_GetArraySize(layers);
        for (const cJSON* layer = layers->child; layer; layer = layer->next) {
            readTrack(obj(layer, "moveXEvents"), tempo, factor, mapLineX, mx);
            readTrack(obj(layer, "moveYEvents"), tempo, factor, mapLineY, my);
            readTrack(obj(layer, "rotateEvents"), tempo, factor, mapRaw, rot);
            readTrack(obj(layer, "alphaEvents"), tempo, factor, mapRaw, alpha);
            readTrack(obj(layer, "speedEvents"), tempo, factor, mapRaw, speed);
        }
    }
    // Convert to line states.
    for (auto& s : mx) {
        s.v0 = lineX(s.v0);
        s.v1 = lineX(s.v1);
    }
    for (auto& s : my) {
        s.v0 = lineY(s.v0);
        s.v1 = lineY(s.v1);
    }
    for (auto& s : alpha) {
        s.v0 = std::max(0.0f, std::min(1.0f, s.v0 / 255.0f));
        s.v1 = std::max(0.0f, std::min(1.0f, s.v1 / 255.0f));
    }
    for (auto& s : speed) {
        s.v0 /= 4.5f;  // Official -> RPE uses speed * 4.5; undo that unit conversion.
        s.v1 /= 4.5f;
    }
    for (auto& s : rot) { s.v0 = -s.v0; s.v1 = -s.v1; }
    out.rpe = true;
    out.bpm = PGRBEAT;  // one beat == one second for the baked timeline
    out.rpeX = std::move(mx);
    out.rpeY = std::move(my);
    out.rpeRot = std::move(rot);
    out.rpeAlpha = std::move(alpha);
    out.rpeSpeed = std::move(speed);
}

void readNotes(const cJSON* line, const Tempo& tempo, JudgeLine& out) {
    const cJSON* notes = obj(line, "notes");
    if (!cJSON_IsArray(notes)) return;
    int n = cJSON_GetArraySize(notes);
    for (const cJSON* jn = notes->child; jn; jn = jn->next) {
        Note note;
        const int rpeType = (int)numOf(obj(jn, "type"), 1.0f);
        // Internal renderer order follows official charts (1=Tap, 2=Drag,
        // 3=Hold, 4=Flick). RPE stores (1=Tap, 2=Hold, 3=Flick, 4=Drag).
        switch (rpeType) {
        case 1: note.type = 1; break;
        case 2: note.type = 3; break;
        case 3: note.type = 4; break;
        case 4: note.type = 2; break;
        default: note.type = 1; break;
        }
        if (note.type < 1 || note.type > 4) note.type = 1;
        double startBeat = readBeat(obj(jn, "startTime"));
        double endBeat = readBeat(obj(jn, "endTime"), startBeat);
        double t0 = tempo.secOf(startBeat);
        double t1 = tempo.secOf(endBeat);
        if (t1 < t0) t1 = t0;
        note.secTime = t0;
        note.secHoldEndTime = t1;
        note.time = t0;  // seconds timeline: beat == second
        note.holdTime = (note.type == 3) ? (t1 - t0) : 0.0f;
        note.positionX = noteX(numOf(obj(jn, "positionX"), 0.0f));
        note.speed = numOf(obj(jn, "speed"), 1.0f);
        if (note.speed <= 0.0f) note.speed = 1.0f;
        const cJSON* official = obj(jn, "phiRenderOfficialHold");
        if (official && note.type == 3) {
            const cJSON* speed = obj(official, "speed");
            if (numOf(obj(official, "version")) != 1 || !cJSON_IsNumber(speed) ||
                !std::isfinite(speed->valuedouble) || speed->valuedouble < 0)
                throw std::runtime_error("invalid phiRenderOfficialHold compatibility data");
            note.officialHold = true;
            note.officialHoldSpeed = float(speed->valuedouble);
        }
        note.size = numOf(obj(jn, "size"), 1.0f);
        if (note.size <= 0.0f) note.size = 1.0f;
        note.alpha = std::max(0.0f, std::min(1.0f, numOf(obj(jn, "alpha"), 255.0f) / 255.0f));
        note.above = flagOf(obj(jn, "above"), 1.0f) > 0.5f;
        note.isFake = flagOf(obj(jn, "isFake"), 0.0f) > 0.5f;
        note.floorPosition = out.rpeFpAt(t0);
        if(note.type==3) for(double beat=startBeat;beat<endBeat-1e-9;beat+=0.5)
            note.holdEffectTimes.push_back(float(tempo.secOf(beat)));
        if (note.above) out.notesAbove.push_back(note);
        else out.notesBelow.push_back(note);
    }
}

float mapBlockTrack(const cJSON* e, float fallback) {
    (void)e;
    return fallback;
}

bool readBlock(const cJSON* jb, const Tempo& tempo, BlockArea& out) {
    const cJSON* bl = obj(jb, "bottomLeft");
    const cJSON* tr = obj(jb, "topRight");
    if (!bl || !tr) return false;
    float blx = numOf(obj(bl, "x"), 0.0f);
    float bly = numOf(obj(bl, "y"), 0.0f);
    float trx = numOf(obj(tr, "x"), 0.0f);
    float try_ = numOf(obj(tr, "y"), 0.0f);
    out.rpe = true;
    out.topRightPercentage = Vec2(1, 1);
    out.bottomLeftPercentage = Vec2(0, 0);
    out.rpeCenterPx = Vec2((blx + trx) * 0.5f, (bly + try_) * 0.5f);
    out.rpeHalfPx = Vec2(std::fabs(trx - blx) * 0.5f, std::fabs(try_ - bly) * 0.5f);
    out.appearTime = tempo.secOf(readBeat(obj(jb, "appearTime")));
    out.disappearTime = tempo.secOf(readBeat(obj(jb, "disappearTime"), readBeat(obj(jb, "appearTime"))));
    if (out.disappearTime < out.appearTime) out.disappearTime = out.appearTime;
    out.isSubtract = flagOf(obj(jb, "isInvert"), 0.0f) > 0.5f;
    out.enableTime = out.appearTime;
    out.disableTime = out.disappearTime;

    // Active intervals with per-interval "ready flash" durations.
    const cJSON* intervals = obj(jb, "activeIntervals");
    if (cJSON_IsArray(intervals)) {
        int n = cJSON_GetArraySize(intervals);
        for (const cJSON* iv = intervals->child; iv; iv = iv->next) {
            float s = tempo.secOf(readBeat(obj(iv, "startTime")));
            float e = tempo.secOf(readBeat(obj(iv, "endTime"), readBeat(obj(iv, "startTime"))));
            s = std::max(s, out.appearTime);
            e = std::min(e, out.disappearTime);
            if (e < s) continue;
            float ready = 0.5f;
            const cJSON* rf = obj(iv, "readyFlash");
            if (rf) {
                if (cJSON_IsBool(rf)) ready = cJSON_IsTrue(rf) ? 0.5f : 0.0f;
                else if (cJSON_IsNumber(rf)) ready = (float)rf->valuedouble;
            }
            ready = std::max(0.0f, std::min(4.0f, ready));
            out.rpeActStart.push_back(s);
            out.rpeActEnd.push_back(e);
            out.rpeReadySec.push_back(ready);
        }
    }

    struct TrackRef {
        const char* name;
        float* dummy;
    };
    auto readBlockTrack = [&](const char* key, std::vector<BlockArea::RpeBlockEvent>& dst, bool anchored) {
        const cJSON* list = obj(jb, key);
        if (!cJSON_IsArray(list)) return;
        int n = cJSON_GetArraySize(list);
        dst.reserve((size_t)n);
        for (const cJSON* e = list->child; e; e = e->next) {
            BlockArea::RpeBlockEvent ev;
            double b0 = readBeat(obj(e, "startTime"));
            double b1 = readBeat(obj(e, "endTime"), b0);
            ev.time = tempo.secOf(b0);
            ev.endTime = tempo.secOf(b1);
            if (ev.endTime < ev.time) std::swap(ev.time, ev.endTime);
            ev.value = numOf(obj(e, "start"), 0.0f);
            ev.endValue = numOf(obj(e, "end"), ev.value);
            ev.ease = (int)numOf(obj(e, "easingType"), 1.0f);
            if (ev.ease < 1 || ev.ease > 29) ev.ease = 1;
            if (anchored) {
                const cJSON* a = obj(e, "anchor");
                ev.anchorPx = Vec2(numOf(obj(a, "x"), 0.0f), numOf(obj(a, "y"), 0.0f));
            }
            ev.transformStart = numOf(obj(e, "transformStart"), ev.value);
            ev.transformEnd = numOf(obj(e, "transformEnd"), ev.endValue);
            ev.transformStep = flagOf(obj(e, "transformStep")) > 0.5f;
            ev.left = numOf(obj(e, "easingLeft"), 0);
            ev.right = numOf(obj(e, "easingRight"), 1);
            ev.bezier = flagOf(obj(e, "bezier")) > 0.5f;
            const cJSON* pts = obj(e, "bezierPoints");
            for (int i=0;i<4;++i) ev.points[i] = numOf(cJSON_GetArrayItem(pts,i), i<2?0:1);
            dst.push_back(ev);
        }
        std::stable_sort(dst.begin(), dst.end(), [](const BlockArea::RpeBlockEvent& a, const BlockArea::RpeBlockEvent& b) {
            return a.time < b.time;
        });
    };
    readBlockTrack("moveXEvents", out.rpeMoveX, false);
    readBlockTrack("moveYEvents", out.rpeMoveY, false);
    readBlockTrack("rotateEvents", out.rpeRot, true);
    readBlockTrack("scaleXEvents", out.rpeScaleX, true);
    readBlockTrack("scaleYEvents", out.rpeScaleY, true);
    return true;
}

}  // namespace

float rpeEase01(int id, float p) {
    p = std::max(0.0f, std::min(1.0f, p));
    auto inside = [](int family, float x) -> float {
        switch (family) {
            case 1: return 1.0f - std::cos(x * PI_F * 0.5f);
            case 2:
            case 3:
            case 4:
            case 5: {
                float r = 1.0f;
                for (int i = 0; i < family; ++i) r *= x;
                return r;
            }
            case 6: return (x <= 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * (x - 1.0f));
            case 7: return 1.0f - std::sqrt(std::max(0.0f, 1.0f - x * x));
            case 8: {
                const float s = 1.70158f;
                return x * x * ((s + 1.0f) * x - s);
            }
            case 9: {
                if (x <= 0.0f || x >= 1.0f) return x;
                return -std::pow(2.0f, 10.0f * x - 10.0f) *
                       std::sin((10.0f * x - 10.75f) * 2.0f * PI_F / 3.0f);
            }
            case 10: {
                float y = 1.0f - x;
                float shift, offset;
                if (y < 4.0f / 11.0f) { shift = 0.0f; offset = 0.0f; }
                else if (y < 8.0f / 11.0f) { shift = 6.0f / 11.0f; offset = 0.75f; }
                else if (y < 10.0f / 11.0f) { shift = 9.0f / 11.0f; offset = 0.9375f; }
                else { shift = 21.0f / 22.0f; offset = 0.984375f; }
                float b = 121.0f / 16.0f * (y - shift) * (y - shift) + offset;
                return 1.0f - b;
            }
            default: return x;
        }
    };
    int family = 9, direction = 2;
    switch (id) {
        case 1: return p;
        case 2: family = 1; direction = 1; break;
        case 3: family = 1; direction = 0; break;
        case 4: family = 2; direction = 1; break;
        case 5: family = 2; direction = 0; break;
        case 6: family = 1; direction = 2; break;
        case 7: family = 2; direction = 2; break;
        case 8: family = 3; direction = 1; break;
        case 9: family = 3; direction = 0; break;
        case 10: family = 4; direction = 1; break;
        case 11: family = 4; direction = 0; break;
        case 12: family = 3; direction = 2; break;
        case 13: family = 4; direction = 2; break;
        case 14: family = 5; direction = 1; break;
        case 15: family = 5; direction = 0; break;
        case 16: family = 6; direction = 1; break;
        case 17: family = 6; direction = 0; break;
        case 18: family = 7; direction = 1; break;
        case 19: family = 7; direction = 0; break;
        case 20: family = 8; direction = 1; break;
        case 21: family = 8; direction = 0; break;
        case 22: family = 7; direction = 2; break;
        case 23: family = 8; direction = 2; break;
        case 24: family = 9; direction = 1; break;
        case 25: family = 9; direction = 0; break;
        case 26: family = 10; direction = 1; break;
        case 27: family = 10; direction = 0; break;
        case 28: family = 10; direction = 2; break;
        case 29: family = 9; direction = 2; break;
        default: return p;
    }
    if (direction == 1) return 1.0f - inside(family, 1.0f - p);
    if (direction == 0) return inside(family, p);
    if (family == 8) {
        const float k = 1.70158f * 1.525f;
        if (p < 0.5f) {
            float x = 2.0f * p;
            return x * x * ((k + 1.0f) * x - k) * 0.5f;
        }
        float x = 2.0f * p - 2.0f;
        return (x * x * ((k + 1.0f) * x + k) + 2.0f) * 0.5f;
    }
    if (family == 9) {
        if (p <= 0.0f || p >= 1.0f) return p;
        float wave = std::sin((20.0f * p - 11.125f) * 2.0f * PI_F / 4.5f) * 0.5f;
        if (p < 0.5f) return -std::pow(2.0f, 20.0f * p - 10.0f) * wave;
        return std::pow(2.0f, 10.0f - 20.0f * p) * wave + 1.0f;
    }
    if (p < 0.5f) return inside(family, 2.0f * p) * 0.5f;
    return 1.0f - inside(family, 2.0f - 2.0f * p) * 0.5f;
}

bool loadRpeChart(const std::string& path, Chart& out, std::string* error) {
    std::string text = readFileText(path);
    if (text.empty()) {
        if (error) *error = "cannot read RPE chart: " + path;
        return false;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        if (error) *error = "invalid RPE JSON: " + path;
        return false;
    }
    const cJSON* meta = obj(root, "META");
    Tempo tempo;
    tempo.build(obj(root, "BPMList"));

    out = Chart();
    out.rpe = true;
    out.formatVersion = 3;
    out.sourceFormat = "rpe";
    out.offset = -numOf(obj(meta, "offset")) / 1000.0f;
    if (meta) {
        const cJSON* name = obj(meta, "name");
        if (name && cJSON_IsString(name)) out.songName = name->valuestring;
        const cJSON* composer = obj(meta, "composer");
        if (composer && cJSON_IsString(composer)) out.composer = composer->valuestring;
        const cJSON* charter = obj(meta, "charter");
        if (charter && cJSON_IsString(charter)) out.charter = charter->valuestring;
        const cJSON* level = obj(meta, "level");
        if (level && cJSON_IsString(level)) out.difficulty = level->valuestring;
    }

    const cJSON* lines = obj(root, "judgeLineList");
    int nLines = cJSON_IsArray(lines) ? cJSON_GetArraySize(lines) : 0;
    out.lines.resize((size_t)nLines);
    size_t lineIndex = 0;
    for (const cJSON* jl = lines->child; jl; jl = jl->next) {
        JudgeLine& line = out.lines[lineIndex++];
        float factor = numOf(obj(jl, "bpmfactor"), 1.0f);
        if (factor <= 0.0f) factor = 1.0f;
        readLayers(jl, tempo, factor, line);
        readNotes(jl, tempo, line);
        for (auto* list : {&line.notesAbove, &line.notesBelow}) for(auto& note : *list) {
            note.secTime += out.offset;
            note.secHoldEndTime += out.offset;
            for(auto& t:note.holdEffectTimes)t+=out.offset;
        }
        out.noteCount += (int)(line.notesAbove.size() + line.notesBelow.size());
    }

    const cJSON* blocks = obj(root, "blockAreaList");
    int nBlocks = cJSON_IsArray(blocks) ? cJSON_GetArraySize(blocks) : 0;
    out.blocks.reserve((size_t)nBlocks);
    for (int i = 0; i < nBlocks; ++i) {
        BlockArea area;
        if (readBlock(cJSON_GetArrayItem(blocks, i), tempo, area)) {
            area.appearTime+=out.offset; area.disappearTime+=out.offset;
            for(auto& t:area.rpeActStart)t+=out.offset;
            for(auto& t:area.rpeActEnd)t+=out.offset;
            for(auto* track:{&area.rpeMoveX,&area.rpeMoveY,&area.rpeRot,&area.rpeScaleX,&area.rpeScaleY})
                for(auto& e:*track){e.time+=out.offset;e.endTime+=out.offset;}
            out.blocks.push_back(std::move(area));
        }
    }

    cJSON_Delete(root);
    out.finalize(0.0f);
    return true;
}
