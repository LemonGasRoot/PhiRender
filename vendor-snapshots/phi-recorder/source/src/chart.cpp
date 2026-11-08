// chart.cpp - official Phigros chart parser + evaluated event functions
#include "chart.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cJSON.h"
#include "rpe.h"

#include <algorithm>
#include <cmath>

namespace {

    float jnum(const cJSON* obj, const char* key, float def) {
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsNumber(it)) return (float)it->valuedouble;
        return def;
    }

    int jint(const cJSON* obj, const char* key, int def) {
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsNumber(it)) return it->valueint;
        return def;
    }

    bool jbool(const cJSON* obj, const char* key, bool def) {
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsBool(it)) return cJSON_IsTrue(it) != 0;
        return def;
    }

    Vec2 jvec2(const cJSON* obj, const char* key, Vec2 def) {
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (cJSON_IsObject(it)) {
            return Vec2(jnum(it, "x", def.x), jnum(it, "y", def.y));
        }
        return def;
    }

    const cJSON* jarr(const cJSON* obj, const char* key) {
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(obj, key);
        return cJSON_IsArray(it) ? it : nullptr;
    }

    // index of last element with time <= t, or -1
    template <typename T, typename F>
    int lastIndexLE(const std::vector<T>& v, float t, F timeOf) {
        int lo = 0, hi = (int)v.size() - 1, res = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (timeOf(v[mid]) <= t) {
                res = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return res;
    }

    std::string readFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::string();
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

} // namespace

// ------------------------------------------------------------------ JudgeLine
float sampleSegments(const std::vector<ValueSegment>& segs, float t, float fallback) {
    int lo = 0, hi = (int)segs.size() - 1, idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (segs[(size_t)mid].t0 <= t + 1e-5f) { idx = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (idx < 0) return fallback;
    const ValueSegment& s = segs[(size_t)idx];
    float p = 1.0f;
    if (s.t1 > s.t0) p = std::max(0.0f, std::min(1.0f, float((t - s.t0) / (s.t1 - s.t0))));
    return s.v0 + (s.v1 - s.v0) * rpeEase01(s.ease, p);
}

float JudgeLine::rpeFpAt(float t) const {
    // Integrate the effective track, clipping each event at the next event.
    // Accumulate in double: dense official conversions otherwise drift by pixels.
    double fp=0, cursor=0;
    float last=0;
    for(size_t i=0;i<rpeSpeed.size();++i){
        const auto& s=rpeSpeed[i];
        double start=std::max(0.0,double(s.t0));
        if(start>t)break;
        if(start>cursor)fp+=(std::min(double(t),start)-cursor)*last;
        double end=std::min(double(t),double(s.t1));
        if(i+1<rpeSpeed.size())end=std::min(end,double(rpeSpeed[i+1].t0));
        if(end>start){
            auto value=[&](double at){return s.v0+(s.v1-s.v0)*(at-s.t0)/std::max(1e-12,double(s.t1-s.t0));};
            fp+=(end-start)*(value(start)+value(end))*0.5;
        }
        cursor=std::max(start,end);last=s.v1;
        if(t<=end)return float(fp);
    }
    if(t>cursor)fp+=(t-cursor)*last;
    return float(fp);
}

float JudgeLine::fpAt(float beat) const {
    if (rpe) return rpeFpAt(beat);
    int i = lastIndexLE(speedEvents, beat, [](const SpeedEvent& e) { return e.startTime; });
    if (i < 0) return 0.0f;
    const SpeedEvent& e = speedEvents[i];
    return e.floorPosition + (beat - e.startTime) * secPerBeat() * e.value;
}

float JudgeLine::moveX(float beat) const {
    if (rpe) return sampleSegments(rpeX, beat, 0.5f);
    int i = lastIndexLE(moveEvents, beat, [](const MoveEvent& e) { return e.startTime; });
    if (i < 0) return 0.5f;
    const MoveEvent& e = moveEvents[i];
    return easeValue(beat, e.startTime, e.endTime, e.start, e.end, 0);
}

float JudgeLine::moveY(float beat) const {
    if (rpe) return sampleSegments(rpeY, beat, 0.5f);
    int i = lastIndexLE(moveEvents, beat, [](const MoveEvent& e) { return e.startTime; });
    if (i < 0) return 0.5f;
    const MoveEvent& e = moveEvents[i];
    return easeValue(beat, e.startTime, e.endTime, e.start2, e.end2, 0);
}

float JudgeLine::rotate(float beat) const {
    if (rpe) return sampleSegments(rpeRot, beat, 0.0f);
    int i = lastIndexLE(rotateEvents, beat, [](const Event& e) { return e.startTime; });
    if (i < 0) return 0.0f;
    const Event& e = rotateEvents[i];
    return easeValue(beat, e.startTime, e.endTime, e.start, e.end, 0);
}

float JudgeLine::alpha(float beat) const {
    if (rpe) return sampleSegments(rpeAlpha, beat, 1.0f);
    int i = lastIndexLE(disappearEvents, beat, [](const Event& e) { return e.startTime; });
    if (i < 0) return 0.0f;
    const Event& e = disappearEvents[i];
    return easeValue(beat, e.startTime, e.endTime, e.start, e.end, 0);
}

float JudgeLine::speedAt(float beat) const {
    if (rpe) return sampleSegments(rpeSpeed, beat, 1.0f);
    int i = lastIndexLE(speedEvents, beat, [](const SpeedEvent& e) { return e.startTime; });
    if (i < 0) return 0.0f;
    return speedEvents[i].value;
}

// ------------------------------------------------------------------ BlockArea
BlockArea::Phase BlockArea::phase(float t) const {
    if (t < appearTime || t >= disappearTime) return Phase::Hidden;
    if (rpe) {
        // RPE-next: explicit activation windows, each with its own "ready"
        // flash lead time.
        int n = (int)rpeActStart.size();
        for (int i = 0; i < n; ++i) {
            if (t >= rpeActStart[i] && t < rpeActEnd[i]) return Phase::Active;
        }
        for (int i = 0; i < n; ++i) {
            float s = rpeActStart[i];
            float ready = (i < (int)rpeReadySec.size()) ? rpeReadySec[i] : 0.0f;
            if (ready > 0.0f && t < s && t >= s - ready) return Phase::Ready;
        }
        return Phase::Disabled;
    }
    if (enableTime <= t && t < disableTime) return Phase::Active;
    if (t < enableTime && t >= enableTime - 0.5f) return Phase::Ready;
    return Phase::Disabled;
}

float BlockArea::phaseStart(float t) const {
    if (rpe) {
        int n = (int)rpeActStart.size();
        for (int i = 0; i < n; ++i) {
            if (t >= rpeActStart[i] && t < rpeActEnd[i]) return rpeActStart[i];
        }
        for (int i = 0; i < n; ++i) {
            float ready = (i < (int)rpeReadySec.size()) ? rpeReadySec[i] : 0.0f;
            if (ready > 0.0f && t < rpeActStart[i] && t >= rpeActStart[i] - ready)
                return rpeActStart[i] - ready;
        }
    } else {
        if (enableTime <= t && t < disableTime) return enableTime;
        if (t < enableTime && t >= enableTime - 0.5f) return enableTime - 0.5f;
    }
    return appearTime;
}

namespace {
// Samples an RPE block track.  Returns the current value and, when the sample
// falls inside an event, that event (for anchored transforms).
struct RpeSample {
    float value = 0.0f;
    int index = -1;
    float progress = 1.0f;
};
RpeSample sampleRpeTrack(const std::vector<BlockArea::RpeBlockEvent>& tr, float t, float fallback) {
    RpeSample s;
    s.value = fallback;
    int lo = 0, hi = (int)tr.size() - 1, idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tr[(size_t)mid].time <= t + 1e-5f) { idx = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (idx < 0) return s;
    const BlockArea::RpeBlockEvent& e = tr[(size_t)idx];
    s.index = idx;
    if (e.endTime > e.time) {
        float p = clampf((t - e.time) / (e.endTime - e.time), 0.0f, 1.0f);
        s.progress = p;
        float eased;
        if (e.bezier) {
            auto cubic=[](float t,float a,float b){return 3*t*(1-t)*(1-t)*a+3*t*t*(1-t)*b+t*t*t;};
            float lo=0,hi=1;
            for(int i=0;i<30;++i){float m=(lo+hi)*0.5f;if(cubic(m,e.points[0],e.points[2])<p)lo=m;else hi=m;}
            eased=cubic((lo+hi)*0.5f,e.points[1],e.points[3]);
        } else {
            float base=rpeEase01(e.ease,e.left), range=rpeEase01(e.ease,e.right)-base;
            eased=std::fabs(range)<1e-12f?p:(rpeEase01(e.ease,e.left+(e.right-e.left)*p)-base)/range;
        }
        s.value = e.value + (e.endValue - e.value) * eased;
    } else {
        s.value = e.value;
    }
    return s;
}
}  // namespace

BlockArea::Transform BlockArea::transform(float t, float aspect) const {
    Transform out;
    out.yScale = 1.0f;
    if (rpe) {
        // Apply anchored history in RPE canvas space, then project Y once.
        Vec2 center = rpeCenterPx;
        auto apply = [&](const std::vector<RpeBlockEvent>& track, int axis, float fallback) {
            auto sample = sampleRpeTrack(track,t,fallback);
            auto relative = [&](float from,float to,Vec2 anchor) {
                Vec2 d=center-anchor;
                if(axis==2){float a=(to-from)*PI_OVER_180,c=std::cos(a),sn=std::sin(a);
                    center=anchor+Vec2(c*d.x+sn*d.y,-sn*d.x+c*d.y);
                } else if(std::fabs(from)>1e-12f) {
                    if(axis==0)center.x=anchor.x+d.x*to/from;
                    else center.y=anchor.y+d.y*to/from;
                }
            };
            if(sample.index>=0){
                for(int k=0;k<sample.index;++k){
                    const auto& prev=track[k];const auto& next=track[k+1];
                    relative(prev.transformStart,prev.transformEnd,prev.anchorPx);
                    relative(prev.transformEnd,next.transformStart,prev.anchorPx);
                }
                const auto& e=track[sample.index];
                relative(e.transformStart,sample.value,e.anchorPx);
            }
            return sample.value;
        };
        float sx=apply(rpeScaleX,0,1), sy=apply(rpeScaleY,1,1);
        float rotation=apply(rpeRot,2,0);
        center.x += sampleRpeTrack(rpeMoveX,t,rpeCenterPx.x).value-rpeCenterPx.x;
        center.y += sampleRpeTrack(rpeMoveY,t,rpeCenterPx.y).value-rpeCenterPx.y;
        const float unit=2.0f/1350.0f;
        out.center=Vec2(center.x*unit,center.y*unit*1.5f/aspect);
        // Transform::size is FULL size; RPE rectangle fields above store HALF size.
        out.size=Vec2(2*std::fabs(rpeHalfPx.x*sx)*unit,2*std::fabs(rpeHalfPx.y*sy)*unit);
        out.yScale=1.5f/aspect;
        out.rotation=-rotation;
        return out;
    }
    auto pctToChart = [aspect](const Vec2& p) {
        return Vec2(2.0f * p.x - 1.0f, (2.0f * p.y - 1.0f) / aspect);
    };
    Vec2 bl = pctToChart(bottomLeftPercentage);
    Vec2 tr = pctToChart(topRightPercentage);
    Vec2 baseSize = tr - bl;
    Vec2 mid = (bl + tr) * 0.5f;
    out.yScale = 1.0f;

    int nScale = (int)scaleEvents.size();
    int iScale = lastIndexLE(scaleEvents, t, [](const BlockAreaScaleEvent& e) { return e.time; });

    Vec2 scale(1, 1);
    if (iScale >= 0 && nScale > 0) {
        if (iScale >= nScale - 1) {
            scale = scaleEvents[nScale - 1].scale;
        } else {
            const auto& a = scaleEvents[iScale];
            const auto& b = scaleEvents[iScale + 1];
            float p = (t - a.time) / (b.time - a.time);
            p = clampf(p, 0.0f, 1.0f);
            scale.x = lerpf(a.scale.x, b.scale.x, ease01(p, a.easeTypeX));
            scale.y = lerpf(a.scale.y, b.scale.y, ease01(p, a.easeTypeY));
        }
    }

    // Center: cumulative scaling/rotation about anchors.
    Vec2 center = mid;
    if (iScale >= 0) {
        for (int k = 0; k < nScale - 1; ++k) {
            const auto& a = scaleEvents[k];
            Vec2 anchor = pctToChart(a.anchor);
            float ratioX = 1.0f, ratioY = 1.0f;
            if (k < iScale) {
                const auto& b = scaleEvents[k + 1];
                ratioX = (std::fabs(a.scale.x) > 1e-6f) ? b.scale.x / a.scale.x : 1.0f;
                ratioY = (std::fabs(a.scale.y) > 1e-6f) ? b.scale.y / a.scale.y : 1.0f;
            } else if (k == iScale) {
                ratioX = (std::fabs(a.scale.x) > 1e-6f) ? scale.x / a.scale.x : 1.0f;
                ratioY = (std::fabs(a.scale.y) > 1e-6f) ? scale.y / a.scale.y : 1.0f;
            } else {
                break;
            }
            center.x = anchor.x + ratioX * (center.x - anchor.x);
            center.y = anchor.y + ratioY * (center.y - anchor.y);
        }
    }

    // Rotation segments.
    int nRot = (int)rotateEvents.size();
    int iRot = lastIndexLE(rotateEvents, t, [](const BlockAreaRotateEvent& e) { return e.time; });
    float rotation = 0.0f;
    if (iRot >= 0) {
        if (iRot >= nRot - 1) {
            rotation = rotateEvents[nRot - 1].rotation;
        } else {
            const auto& a = rotateEvents[iRot];
            const auto& b = rotateEvents[iRot + 1];
            float p = (t - a.time) / (b.time - a.time);
            p = clampf(p, 0.0f, 1.0f);
            rotation = lerpf(a.rotation, b.rotation, ease01(p, a.easeType));
        }
        for (int k = 0; k < nRot - 1; ++k) {
            const auto& a = rotateEvents[k];
            float delta;
            if (k < iRot) {
                delta = rotateEvents[k + 1].rotation - a.rotation;
            } else if (k == iRot) {
                delta = rotation - a.rotation;
            } else {
                break;
            }
            Vec2 anchor = pctToChart(a.anchor);
            float rad = -delta * PI_OVER_180;
            float c = std::cos(rad), s = std::sin(rad);
            Vec2 d = center - anchor;
            center.x = anchor.x + c * d.x + s * d.y;
            center.y = anchor.y - s * d.x + c * d.y;
        }
    }

    // Move segments.
    if (!moveEvents.empty()) {
        int iMove = lastIndexLE(moveEvents, t, [](const BlockAreaMoveEvent& e) { return e.time; });
        if (iMove >= 0) {
            Vec2 target;
            bool hasTarget = true;
            if (iMove >= (int)moveEvents.size() - 1) {
                target = moveEvents[moveEvents.size() - 1].endPosition;
            } else {
                const auto& a = moveEvents[iMove];
                const auto& b = moveEvents[iMove + 1];
                float p = (t - a.time) / (b.time - a.time);
                p = clampf(p, 0.0f, 1.0f);
                target.x = lerpf(a.endPosition.x, b.endPosition.x, ease01(p, a.easeTypeX));
                target.y = lerpf(a.endPosition.y, b.endPosition.y, ease01(p, a.easeTypeY));
            }
            if (hasTarget) {
                center += pctToChart(target) - mid;
            }
        }
    }

    out.center = center;
    out.size = Vec2(std::fabs(baseSize.x * scale.x), std::fabs(baseSize.y * scale.y));
    out.rotation = rotation;
    out.yScale = 1.0f;
    return out;
}

// -------------------------------------------------------------------- Chart
bool Chart::loadChart(const std::string& path, std::string* error) {
    // Format sniffing: RPE charts are JSON objects carrying META/BPMList keys,
    // official Phigros charts carry judgeLineList/blockAreaList instead.
    std::string text = readFile(path);
    if (text.empty()) {
        if (error) *error = "cannot open chart file: " + path;
        return false;
    }
    bool looksRpe = text.find("\"META\"") != std::string::npos &&
                    text.find("\"BPMList\"") != std::string::npos;
    if (looksRpe) {
        sourceFormat = "rpe";
        if (!loadRpeChart(path, *this, error)) return false;
        markMultipleHints();
        return true;
    }
    sourceFormat = "official";
    if (!loadOfficialJson(path, error)) return false;
    markMultipleHints();
    return true;
}

bool Chart::loadOfficialJson(const std::string& path, std::string* error) {
    std::string text = readFile(path);
    if (text.empty()) {
        if (error) *error = "cannot read chart: " + path;
        return false;
    }
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        if (error) *error = "invalid JSON in " + path;
        return false;
    }
    formatVersion = jint(root, "formatVersion", 3);
    offset = jnum(root, "offset", 0.0f);
    lines.clear();
    blocks.clear();
    noteCount = 0;

    const cJSON* jlines = jarr(root, "judgeLineList");
    if (jlines) {
        int n = cJSON_GetArraySize(jlines);
        lines.reserve(n);
        for (const cJSON* jl = jlines->child; jl; jl = jl->next) {
            JudgeLine line;
            line.bpm = jnum(jl, "bpm", 100.0f);
            if (line.bpm <= 0.0f) line.bpm = 100.0f;

            if (const cJSON* arr = jarr(jl, "judgeLineMoveEvents")) {
                int m = cJSON_GetArraySize(arr);
                line.moveEvents.reserve(m);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    MoveEvent ev;
                    ev.startTime = jnum(e, "startTime", 0);
                    ev.endTime = jnum(e, "endTime", 0);
                    ev.start = jnum(e, "start", 0);
                    ev.end = jnum(e, "end", 0);
                    ev.start2 = jnum(e, "start2", ev.start);
                    ev.end2 = jnum(e, "end2", ev.end);
                    if (formatVersion < 3) {
                        // Legacy packed representation: x = start/1000, y = (start%1000)/880*530
                        ev.start2 = std::fmod(ev.start, 1000.0f) / 880.0f * 530.0f;
                        ev.end2 = std::fmod(ev.end, 1000.0f) / 880.0f * 530.0f;
                        ev.start = ev.start / 1000.0f;
                        ev.end = ev.end / 1000.0f;
                    }
                    if (ev.endTime < ev.startTime) continue;
                    line.moveEvents.push_back(ev);
                }
            }
            auto parseEventArr = [&](const char* key, std::vector<Event>& out) {
                const cJSON* arr = jarr(jl, key);
                if (!arr) return;
                int m = cJSON_GetArraySize(arr);
                out.reserve(m);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    Event ev;
                    ev.startTime = jnum(e, "startTime", 0);
                    ev.endTime = jnum(e, "endTime", 0);
                    ev.start = jnum(e, "start", 0);
                    ev.end = jnum(e, "end", 0);
                    if (ev.endTime < ev.startTime) continue;
                    out.push_back(ev);
                }
            };
            parseEventArr("judgeLineRotateEvents", line.rotateEvents);
            parseEventArr("judgeLineDisappearEvents", line.disappearEvents);

            if (const cJSON* arr = jarr(jl, "speedEvents")) {
                int m = cJSON_GetArraySize(arr);
                line.speedEvents.reserve(m);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    SpeedEvent ev;
                    ev.startTime = jnum(e, "startTime", 0);
                    ev.endTime = jnum(e, "endTime", 0);
                    ev.value = jnum(e, "value", 1.0f);
                    if (ev.endTime < ev.startTime) continue;
                    line.speedEvents.push_back(ev);
                }
            }

            auto sortAndChain = [](auto& events, auto getStart, auto setStart, auto getEnd) {
                if (events.empty()) return;
                std::stable_sort(events.begin(), events.end(),
                                 [&](const auto& a, const auto& b) { return getStart(a) < getStart(b); });
                for (size_t k = 1; k < events.size(); ++k) {
                    if (getStart(events[k]) < getEnd(events[k - 1])) setStart(events[k], getEnd(events[k - 1]));
                }
            };
            sortAndChain(
                line.moveEvents, [](const MoveEvent& e) { return e.startTime; },
                [](MoveEvent& e, float v) { e.startTime = v; }, [](const MoveEvent& e) { return e.endTime; });
            sortAndChain(
                line.rotateEvents, [](const Event& e) { return e.startTime; },
                [](Event& e, float v) { e.startTime = v; }, [](const Event& e) { return e.endTime; });
            sortAndChain(
                line.disappearEvents, [](const Event& e) { return e.startTime; },
                [](Event& e, float v) { e.startTime = v; }, [](const Event& e) { return e.endTime; });
            sortAndChain(
                line.speedEvents, [](const SpeedEvent& e) { return e.startTime; },
                [](SpeedEvent& e, float v) { e.startTime = v; }, [](const SpeedEvent& e) { return e.endTime; });

            // Speed events -> floorPosition (seconds-distance units).
            float fp = 0.0f;
            for (auto& e : line.speedEvents) {
                e.floorPosition = fp;
                fp += (e.endTime - e.startTime) * line.secPerBeat() * e.value;
            }

            auto parseNotes = [&](const char* key, bool above) {
                const cJSON* arr = jarr(jl, key);
                if (!arr) return;
                int m = cJSON_GetArraySize(arr);
                auto& out = above ? line.notesAbove : line.notesBelow;
                out.reserve(m);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    Note note;
                    note.type = jint(e, "type", 1);
                    note.time = jnum(e, "time", 0);
                    note.positionX = jnum(e, "positionX", 0);
                    note.holdTime = jnum(e, "holdTime", 0);
                    note.speed = jnum(e, "speed", 1.0f);
                    note.officialHold = note.type == 3;
                    note.officialHoldSpeed = note.speed;
                    note.floorPosition = jnum(e, "floorPosition", 0.0f);
                    note.above = above;
                    note.secTime = line.beat2sec(note.time, offset);
                    note.secHoldEndTime = line.beat2sec(note.time + note.holdTime, offset);
                    out.push_back(note);
                }
            };
            parseNotes("notesAbove", true);
            parseNotes("notesBelow", false);
            noteCount += (int)(line.notesAbove.size() + line.notesBelow.size());
            lines.push_back(std::move(line));
        }
    }

    const cJSON* jblocks = jarr(root, "blockAreaList");
    if (jblocks) {
        int n = cJSON_GetArraySize(jblocks);
        blocks.reserve(n);
        for (const cJSON* jb = jblocks->child; jb; jb = jb->next) {
            BlockArea b;
            b.topRightPercentage = jvec2(jb, "topRightPercentage", Vec2(1, 1));
            b.bottomLeftPercentage = jvec2(jb, "bottomLeftPercentage", Vec2(0, 0));
            b.appearTime = jnum(jb, "appearTime", 0) + offset;
            b.disappearTime = jnum(jb, "disappearTime", 0) + offset;
            b.enableTime = jnum(jb, "enableTime", 0) + offset;
            b.disableTime = jnum(jb, "disableTime", 0) + offset;
            b.isSubtract = jbool(jb, "isSubtract", false);
            if (const cJSON* arr = jarr(jb, "rotateEvents")) {
                int m = cJSON_GetArraySize(arr);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    BlockAreaRotateEvent ev;
                    ev.anchor = jvec2(e, "anchor", Vec2(0.5f, 0.5f));
                    ev.time = jnum(e, "time", 0) + offset;
                    ev.easeType = jint(e, "easeType", 0);
                    ev.rotation = jnum(e, "rotation", 0);
                    b.rotateEvents.push_back(ev);
                }
            }
            if (const cJSON* arr = jarr(jb, "moveEvents")) {
                int m = cJSON_GetArraySize(arr);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    BlockAreaMoveEvent ev;
                    ev.endPosition = jvec2(e, "endPosition", Vec2(0.5f, 0.5f));
                    ev.time = jnum(e, "time", 0) + offset;
                    ev.easeTypeX = jint(e, "easeTypeX", 0);
                    ev.easeTypeY = jint(e, "easeTypeY", 0);
                    b.moveEvents.push_back(ev);
                }
            }
            if (const cJSON* arr = jarr(jb, "scaleEvents")) {
                int m = cJSON_GetArraySize(arr);
                for (const cJSON* e = arr->child; e; e = e->next) {
                    BlockAreaScaleEvent ev;
                    ev.anchor = jvec2(e, "anchor", Vec2(0.5f, 0.5f));
                    ev.time = jnum(e, "time", 0) + offset;
                    ev.easeTypeX = jint(e, "easeTypeX", 0);
                    ev.easeTypeY = jint(e, "easeTypeY", 0);
                    ev.scale = jvec2(e, "scale", Vec2(1, 1));
                    b.scaleEvents.push_back(ev);
                }
            }
            blocks.push_back(std::move(b));
        }
    }

    cJSON_Delete(root);
    return true;
}

void Chart::markMultipleHints() {
    // Official multi-press hint: every note struck at the *same time* gets the
    // MH sprites, regardless of which judge line it belongs to.  The condition
    // is purely temporal, so collect the whole chart before grouping.
    const float eps = 1.0e-4f;
    std::vector<Note*> all;
    {
        size_t total = 0;
        for (auto& line : lines) total += line.notesAbove.size() + line.notesBelow.size();
        all.reserve(total);
        for (auto& line : lines) {
            for (auto& n : line.notesAbove) all.push_back(&n);
            for (auto& n : line.notesBelow) all.push_back(&n);
        }
    }
    std::sort(all.begin(), all.end(), [](const Note* a, const Note* b) {
        return a->secTime < b->secTime;
    });
    size_t i = 0;
    while (i < all.size()) {
        size_t j = i + 1;
        while (j < all.size() && all[j]->secTime - all[i]->secTime <= eps) ++j;
        if (j - i >= 2) {
            for (size_t k = i; k < j; ++k) all[k]->multipleHint = true;
        }
        i = j;
    }
    if (std::getenv("PHI_DEBUG_HINTS")) {
        int hints = 0, total = 0;
        for (auto& line : lines) {
            for (auto& n : line.notesAbove) { ++total; hints += n.multipleHint; }
            for (auto& n : line.notesBelow) { ++total; hints += n.multipleHint; }
        }
        std::printf("[hints] %d/%d notes flagged as multi-press\n", hints, total);
        int shown = 0;
        for (auto& line : lines) {
            for (auto& n : line.notesAbove) {
                if (n.multipleHint && shown < 8) {
                    std::printf("[hints] t=%.3f type=%d\n", n.secTime, n.type);
                    ++shown;
                }
            }
        }
    }
}

void Chart::finalize(float musicDuration) {
    duration = musicDuration > 0.0f ? musicDuration : 0.0f;
}

void blockCorners(const BlockArea::Transform& tr, Vec2 out[4]) {
    float halfW = tr.size.x * 0.5f;
    float halfH = tr.size.y * 0.5f;
    float rad = -tr.rotation * PI_OVER_180;
    float c = std::cos(rad), s = std::sin(rad);
    Vec2 local[4] = {Vec2(-halfW, -halfH), Vec2(halfW, -halfH), Vec2(halfW, halfH), Vec2(-halfW, halfH)};
    for (int i = 0; i < 4; ++i) {
        Vec2 l = local[i];
        Vec2 p(tr.center.x + c * l.x + s * l.y, tr.center.y + (-s * l.x + c * l.y) * tr.yScale);
        out[i] = p;
    }
}
