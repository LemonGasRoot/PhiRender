// chart.h - Phigros chart model (official phi format 3 / 4.0 with blockAreaList)
#pragma once

#include "common.h"

#include <string>
#include <vector>

// Official Phigros constants (from PhiVideo Constant.h / official decompilation).
static constexpr float PGRW = 0.05625f;    // note width unit relative to screen width
static constexpr float PGRH = 0.6f;        // note travel scale relative to screen height
static constexpr float PGRBEAT = 1.875f;   // seconds per beat divisor
static constexpr float LINEW = 0.0075f;    // judge line width relative to screen height
static constexpr float LINEH = 2.875f;     // judge line length relative to screen height
static constexpr float NOTE_SIZE = 0.1234375f;

struct MoveEvent {
    float startTime = 0, endTime = 0;
    float start = 0, end = 0;     // x
    float start2 = 0, end2 = 0;   // y
};

struct Event {
    float startTime = 0, endTime = 0;
    float start = 0, end = 0;
};

struct SpeedEvent {
    float startTime = 0, endTime = 0;
    float value = 1.0f;
    float floorPosition = 0.0f;
};

struct Note {
    int type = 1;              // 1 tap, 2 drag, 3 hold, 4 flick
    float time = 0.0f;         // beats
    float positionX = 0.0f;
    float holdTime = 0.0f;     // beats
    float speed = 1.0f;
    float floorPosition = 0.0f;
    bool officialHold = false;
    float officialHoldSpeed = 1.0f;
    // Body distance in screen-height units, excluding head and tail artwork.
    // It is independent of judge-line scroll speed and shrinks by elapsed time.
    float officialHoldBody(float now) const {
        return std::max(0.0f, secHoldEndTime - std::max(now, secTime)) * officialHoldSpeed * PGRH;
    }
    bool above = true;
    float secTime = 0.0f;      // seconds
    std::vector<float> holdEffectTimes; // half-beat ticks for RPE tempo changes
    float secHoldEndTime = 0.0f;
    float size = 1.0f;         // note width multiplier (RPE "size", 1 = default)
    float alpha = 1.0f;        // per-note opacity (RPE alpha / 255)
    bool isFake = false;       // RPE fake note: not drawn in autoplay videos
    bool multipleHint = false; // simultaneous with another note -> MH (multi-press hint) art
};

// Keyframe segment used by the RPE/RPE-next loader: value eases from v0 to v1
// over [t0, t1] (seconds) with RPE easing id `ease`, then holds until the next
// segment starts.
struct ValueSegment {
    double t0 = 0.0, t1 = 0.0;
    float v0 = 0.0f, v1 = 0.0f;
    int ease = 1;
};
float sampleSegments(const std::vector<ValueSegment>& segs, float t, float fallback);

struct JudgeLine {
    float bpm = 100.0f;
    std::vector<MoveEvent> moveEvents;
    std::vector<Event> rotateEvents;
    std::vector<Event> disappearEvents;
    std::vector<SpeedEvent> speedEvents;
    std::vector<Note> notesAbove;
    std::vector<Note> notesBelow;

    // --- RPE/RPE-next lines ---
    // When `rpe` is true the chart loader bakes a seconds timeline (bpm is set
    // so that one beat == one second) and these tracks are sampled instead of
    // the phi event arrays above.
    bool rpe = false;
    std::vector<ValueSegment> rpeX, rpeY, rpeRot, rpeAlpha, rpeSpeed;
    // Floor position (integral of line speed) for RPE lines, seconds units.
    float rpeFpAt(float t) const;

    float secPerBeat() const { return PGRBEAT / bpm; }
    float sec2beat(float sec, float offset) const { return (sec - offset) / secPerBeat(); }
    float beat2sec(float beat, float offset) const { return beat * secPerBeat() + offset; }
    float fpAt(float beat) const;
    float moveX(float beat) const;
    float moveY(float beat) const;
    float rotate(float beat) const;
    float alpha(float beat) const;
    float speedAt(float beat) const;
};

struct BlockAreaRotateEvent {
    Vec2 anchor;
    float time = 0.0f;
    int easeType = 0;
    float rotation = 0.0f;
};

struct BlockAreaMoveEvent {
    Vec2 endPosition;
    float time = 0.0f;
    int easeTypeX = 0;
    int easeTypeY = 0;
};

struct BlockAreaScaleEvent {
    Vec2 anchor;
    float time = 0.0f;
    int easeTypeX = 0;
    int easeTypeY = 0;
    Vec2 scale = Vec2(1, 1);
};

struct BlockArea {
    Vec2 topRightPercentage = Vec2(1, 1);
    Vec2 bottomLeftPercentage = Vec2(0, 0);
    float appearTime = 0.0f;
    float disappearTime = 0.0f;
    float enableTime = 0.0f;
    float disableTime = 0.0f;
    bool isSubtract = false;
    std::vector<BlockAreaRotateEvent> rotateEvents;
    std::vector<BlockAreaMoveEvent> moveEvents;
    std::vector<BlockAreaScaleEvent> scaleEvents;

    // --- RPE/RPE-next block areas ---
    // Coordinates are kept in RPE canvas pixels (1350x900) and times in
    // seconds; the transform sampler converts them to chart space.
    struct RpeBlockEvent {
        float time = 0.0f, endTime = 0.0f;
        float value = 0.0f, endValue = 0.0f;
        int ease = 1;
        Vec2 anchorPx = Vec2(0, 0);
        float transformStart = 0, transformEnd = 0;
        bool transformStep = false, bezier = false;
        float left = 0, right = 1;
        float points[4] = {0,0,1,1};
    };
    bool rpe = false;
    Vec2 rpeCenterPx = Vec2(0, 0);
    Vec2 rpeHalfPx = Vec2(0, 0);
    std::vector<RpeBlockEvent> rpeMoveX, rpeMoveY, rpeRot, rpeScaleX, rpeScaleY;
    std::vector<float> rpeActStart, rpeActEnd, rpeReadySec;

    // --- evaluated state ---
    enum class Phase { Hidden = 0, Disabled = 1, Ready = 2, Active = 3 };
    Phase phase(float t) const;
    // Start of the visibility window that contains `t` (Ready or Active); the
    // appear flash is timed off this instead of the first activation.
    float phaseStart(float t) const;
    // Full transform in chart space; see block_renderer for usage.
    struct Transform {
        Vec2 center;   // chart space: x in [-1,1], y in [-1/aspect, 1/aspect]
        Vec2 size;     // chart space size
        float rotation = 0.0f; // degrees
        float yScale = 1.0f;
    };
    Transform transform(float t, float aspect) const;
};

struct Chart {
    int formatVersion = 3;
    float offset = 0.0f;
    std::vector<JudgeLine> lines;
    std::vector<BlockArea> blocks;
    int noteCount = 0;
    float duration = 0.0f; // music duration (seconds), set by the app
    std::string songName;
    std::string difficulty;
    std::string composer;
    std::string charter;
    std::string illustrator;
    bool rpe = false;              // loaded from an RPE/RPE-next chart
    std::string sourceFormat;      // "phi", "rpe", ...

    bool loadOfficialJson(const std::string& path, std::string* error);
    // Flags notes that appear at the same time as another note on the same judge
    // line (multi-press hint). Official Phigros ships a dedicated "MH" set of
    // note artwork for these; the flag itself is not part of the chart format.
    void markMultipleHints();
    // Auto-detects the chart format (official phi JSON or RPE/RPE-next JSON).
    bool loadChart(const std::string& path, std::string* error);
    void finalize(float musicDuration);
};

// Utility used by the renderer for blocks: chart-space rect corners.
void blockCorners(const BlockArea::Transform& tr, Vec2 out[4]);
