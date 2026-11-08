// rpe.h - RPE / RPE-next chart importer (Phigros community editor format).
//
// This lets phi-recorder render charts exported by the RPE editor, including
// RPE-next (format >= 220) noise block areas ("blockAreaList" with
// activeIntervals and per-axis animation tracks) which is the format used by
// Phira-Pro / Phira to author Phigros 4.0 style "noise areas".  The loader
// converts RPE's 1350x900 canvas and beat triples into the renderer's
// second-based timeline so the rest of the pipeline stays untouched.
#pragma once

#include "chart.h"

#include <string>

// RPE easing id (1..29) sampled at p in [0,1].  Ported from the official
// RPE-next curve table (linear / sine / quad..decuple in-out / back / elastic
// / bounce); ids outside 1..29 fall back to linear.
float rpeEase01(int id, float p);

// Loads an RPE or RPE-next chart.  Returns false and fills `error` on failure.
bool loadRpeChart(const std::string& path, Chart& out, std::string* error);
