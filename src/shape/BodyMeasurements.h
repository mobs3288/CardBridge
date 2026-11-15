#pragma once
// The maker has no BWH slider: a body measurement is a set of the shapeValueBody sliders the game multiplies into the
// body skeleton. One measurement therefore has to move several sliders together, or it would not be a measurement.
//
//   Bust  -> "Bust > Size" (body slider 4: the three cf_a_bust.._size bones, the nipple bones, the hit sphere)
//   Waist -> "Waist > Waist width" (22, x-scale of cf_a_waist01) and "Waist > Waist depth" (23, z-scale of it)
//   Hip   -> "Hips > Hip width" (24, x-scale of cf_a_waist02 and the thigh aids) and "Hips > Hip depth" (25, z-scale of it)
//
// ("Butt size" and "Butt angle" are cf_a_siri's own sliders and are deliberately left out: a hip measurement is the
// waist02 ring, not a change of the buttock shape, and nothing about the bust's shape sliders is a measurement either.)
//
// The rows move the sliders they drive by the same delta, keeping the card's own width against its depth and clamping
// each slider by the same range the plain rows use (-1 .. 2, or the card's own value when it is outside that). The value
// is the percentage of the nominal 0..1 range: 0 = the bottom, 100 = the top, -100..200 for SliderUnlocker cards.

#include "Shape.h"   // SliderInfo (the sliders this maps onto are shapeValueBody indices)

#include <algorithm>
#include <cmath>
#include <vector>

namespace cardbridge::shape {

enum BodyMeasure : int { kBodyMeasureBust = 0, kBodyMeasureWaist = 1, kBodyMeasureHip = 2, kBodyMeasureCount = 3 };

struct BodyMeasureSpec {
    const char* label = "";   // "Bust" / "Waist" / "Hip"
    int sliders[2] = {-1, -1};
    int sliderCount = 0;
};

inline const BodyMeasureSpec& BodyMeasureSpecOf(int measure) {
    static const BodyMeasureSpec kSpecs[kBodyMeasureCount] = {
        {"Bust", {4, -1}, 1},       // shapeValueBody 4: "Bust > Size"
        {"Waist", {22, 23}, 2},     // "Waist > Waist width" / "Waist > Waist depth"
        {"Hip", {24, 25}, 2},       // "Hips > Hip width" / "Hips > Hip depth"
    };
    return kSpecs[measure >= 0 && measure < kBodyMeasureCount ? measure : 0];
}

// The average of the sliders the measurement drives, in the shape's own 0..1-ish units, or 0 when the shape is not loaded.
inline float BodyMeasureAverage(const std::vector<float>& bodyShape, int measure) {
    const BodyMeasureSpec& spec = BodyMeasureSpecOf(measure);
    double sum = 0.0;
    for (int i = 0; i < spec.sliderCount; ++i) {
        const int s = spec.sliders[i];
        if (s < 0 || static_cast<size_t>(s) >= bodyShape.size()) return 0.0f;
        sum += bodyShape[static_cast<size_t>(s)];
    }
    return static_cast<float>(sum / static_cast<double>(spec.sliderCount));
}

// The [lo, hi] range a single slider is clamped to, like the viewer's plain rows: -1..2, or the card's own value when it is outside that.
inline float BodyMeasureSliderLo(float card) { return std::min(-1.0f, card); }
inline float BodyMeasureSliderHi(float card) { return std::max(2.0f, card); }

// The [lo, hi] range the measurement row is clamped to, the union of the sliders' ranges (in the shape's units).
inline void BodyMeasureRange(const std::vector<float>& cardShape, int measure, float& lo, float& hi) {
    const BodyMeasureSpec& spec = BodyMeasureSpecOf(measure);
    lo = 2.0f;
    hi = -1.0f;
    for (int i = 0; i < spec.sliderCount; ++i) {
        const int s = spec.sliders[i];
        const float def = s >= 0 && static_cast<size_t>(s) < cardShape.size() ? cardShape[static_cast<size_t>(s)] : 0.5f;
        lo = std::min(lo, BodyMeasureSliderLo(def));
        hi = std::max(hi, BodyMeasureSliderHi(def));
    }
    if (lo > hi) { lo = -1.0f; hi = 2.0f; }   // an empty card: the plain slider's range
}

// Move every slider the measurement drives by (target - the current average). Each slider is clamped to its own range, so
// the measurement does not reach target when a component is pinned — that is what "unsupported value" means for the model.
// The card's slider shape and the current shape must be the same size; non-finite targets are refused. Returns true when
// at least one slider moved.
inline bool BodyMeasureSetTarget(std::vector<float>& bodyShape, const std::vector<float>& cardShape, int measure, float target) {
    if (!std::isfinite(target) || bodyShape.size() != cardShape.size() || bodyShape.empty()) return false;
    const BodyMeasureSpec& spec = BodyMeasureSpecOf(measure);
    const float delta = target - BodyMeasureAverage(bodyShape, measure);
    bool moved = false;
    for (int i = 0; i < spec.sliderCount; ++i) {
        const int s = spec.sliders[i];
        if (s < 0 || static_cast<size_t>(s) >= bodyShape.size()) continue;
        const size_t index = static_cast<size_t>(s);
        const float card = cardShape[index];
        const float next = std::clamp(bodyShape[index] + delta, BodyMeasureSliderLo(card), BodyMeasureSliderHi(card));
        const float safe = std::isfinite(next) ? next : card;
        moved = moved || safe != bodyShape[index];
        bodyShape[index] = safe;
    }
    return moved;
}

} // namespace cardbridge::shape
