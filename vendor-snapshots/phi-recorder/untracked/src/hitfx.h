// PhiVideo RenderEffects / HitFx.cpp formulas, isolated for regression tests.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
namespace hitfx {
constexpr float duration = .5f;
inline int frame(float age, int count) { return std::max(0,std::min(count-1,int(std::floor(age/duration*count)))); }
inline float width(float noteWidth) { return noteWidth*1.375f*1.12f; }
inline float progress(float age) { return std::max(0.f,std::min(1.f,age/duration)); }
inline float particleTravel(float p) { return 9*p/(8*p+1); }
inline float particleScale(float width) { return width/4040.f*3; }
inline float particleSize(float width) { return 33.f*.75f*particleScale(width); }
inline float random(float lo,float hi,uint32_t index) {
    // PhiVideo randf(seed): float multiplication followed by a 32-bit seed.
    uint32_t seed=uint32_t(uint64_t(float(index)*999983.f));
    std::mt19937 rng(seed);
    return std::uniform_real_distribution<float>(lo,hi)(rng);
}
}
