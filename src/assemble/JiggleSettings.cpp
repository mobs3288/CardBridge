#include "assemble/JiggleSettings.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::assemble {

namespace {

constexpr float kGravity = 9.81f;
// The largest displacement of a link from its posed place on a world axis, metres, at amplitude 1 and strength 1. X is the
// sideways sway, Y the vertical bounce and Z the forward / back flop: the bust and hips move a little further sideways and
// forward than they bounce straight down.
constexpr float kBaseDisplacement[3] = {0.060f, 0.045f, 0.055f};
// The widest turn of a PMX joint in the reference hand-made models' terms: the bust hangs far, the butt less. The PMX axes
// are the joint's own: X is the forward / back flop, Z the sideways sway, Y the twist about the bone (kept small).
constexpr float kBustJointRange = 0.60f;
constexpr float kButtJointRange = 0.34f;
constexpr float kTwistShare = 0.20f;
constexpr float kSwayShare = 0.90f;

bool NearlyEqual(float a, float b) {
    const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-3f * scale;
}

float FiniteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

} // namespace

const char* JigglePresetName(JigglePreset preset) {
    switch (preset) {
    case JigglePreset::Subtle: return "Subtle";
    case JigglePreset::Natural: return "Natural";
    case JigglePreset::Dynamic: return "Dynamic";
    case JigglePreset::Custom: return "Custom";
    }
    return "Custom";
}

JiggleSettings JigglePresetSettings(JigglePreset preset) {
    JiggleSettings s;
    switch (preset) {
    case JigglePreset::Subtle:
        // Firm and well damped: the part moves a little and stops, like a tight sports top.
        s.mass = 1.15f;
        s.stiffness = 360.0f;
        s.damping = 6.0f;
        s.gravity = 0.9f;
        s.amplitudeVertical = 0.65f;
        s.amplitudeHorizontal = 0.65f;
        s.amplitudeDepth = 0.65f;
        s.rotationStrength = 0.65f;
        s.strength = 0.75f;
        break;
    case JigglePreset::Natural:
        // The defaults in the struct: soft skin that visibly sways sideways and forward, still critically damped enough to stop.
        break;
    case JigglePreset::Dynamic:
        // Lighter, softer, livelier: more travel on every axis and a stronger bone follow.
        s.mass = 0.8f;
        s.stiffness = 150.0f;
        s.damping = 2.6f;
        s.gravity = 1.15f;
        s.amplitudeVertical = 1.45f;
        s.amplitudeHorizontal = 1.45f;
        s.amplitudeDepth = 1.45f;
        s.rotationStrength = 1.25f;
        s.strength = 1.25f;
        break;
    case JigglePreset::Custom:
        break;
    }
    return s;
}

JigglePreset MatchJigglePreset(const JiggleSettings& s) {
    for (JigglePreset preset : {JigglePreset::Subtle, JigglePreset::Natural, JigglePreset::Dynamic}) {
        const JiggleSettings p = JigglePresetSettings(preset);
        if (NearlyEqual(s.mass, p.mass) && NearlyEqual(s.stiffness, p.stiffness) && NearlyEqual(s.damping, p.damping) &&
            NearlyEqual(s.gravity, p.gravity) && NearlyEqual(s.amplitudeVertical, p.amplitudeVertical) &&
            NearlyEqual(s.amplitudeHorizontal, p.amplitudeHorizontal) && NearlyEqual(s.amplitudeDepth, p.amplitudeDepth) &&
            NearlyEqual(s.rotationStrength, p.rotationStrength) && NearlyEqual(s.strength, p.strength) &&
            NearlyEqual(s.simulationHz, p.simulationHz))
            return preset;
    }
    return JigglePreset::Custom;
}

JiggleSettings SanitizeJiggleSettings(JiggleSettings s) {
    const JiggleSettings d;
    auto clamp = [](float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); };
    s.mass = clamp(FiniteOr(s.mass, d.mass), 0.05f, 20.0f);
    s.stiffness = clamp(FiniteOr(s.stiffness, d.stiffness), 5.0f, 5000.0f);
    s.damping = clamp(FiniteOr(s.damping, d.damping), 0.0f, 200.0f);
    s.gravity = clamp(FiniteOr(s.gravity, d.gravity), 0.0f, 5.0f);
    s.amplitudeVertical = clamp(FiniteOr(s.amplitudeVertical, d.amplitudeVertical), 0.0f, 3.0f);
    s.amplitudeHorizontal = clamp(FiniteOr(s.amplitudeHorizontal, d.amplitudeHorizontal), 0.0f, 3.0f);
    s.amplitudeDepth = clamp(FiniteOr(s.amplitudeDepth, d.amplitudeDepth), 0.0f, 3.0f);
    s.rotationStrength = clamp(FiniteOr(s.rotationStrength, d.rotationStrength), 0.0f, 2.0f);
    s.strength = clamp(FiniteOr(s.strength, d.strength), 0.0f, 4.0f);
    s.simulationHz = clamp(FiniteOr(s.simulationHz, d.simulationHz), 30.0f, 480.0f);
    return s;
}

float JiggleAngularFrequency(const JiggleSettings& s) {
    return std::sqrt(std::max(0.0f, s.stiffness) / std::max(1e-4f, s.mass));
}

float JiggleDampingRatio(const JiggleSettings& s) {
    const float critical = 2.0f * std::sqrt(std::max(0.0f, s.stiffness) * std::max(1e-4f, s.mass));
    return critical > 1e-6f ? std::max(0.0f, s.damping) / critical : 0.0f;
}

float JigglePmxDamping(const JiggleSettings& s, float linkMass) {
    // Bullet applies v *= (1 - damping)^dt each second. The continuous decay of a mass on a damper is exp(-c t / (2 m)), so the
    // per-second retention is exp(-c / (2 m)) and the PMX value is one minus that. Clamped away from a body that never moves.
    const float m = std::max(0.05f, linkMass);
    return std::min(0.999f, std::max(0.0f, 1.0f - std::exp(-std::max(0.0f, s.damping) / (2.0f * m))));
}

float JiggleStaticSagMeters(const JiggleSettings& s) {
    // The sag the preview settles to: the part hangs under the share of its weight the body does not hold (kJiggleSupport).
    return std::max(0.0f, 1.0f - kJiggleSupport) * std::max(0.0f, s.mass) * std::max(0.0f, s.gravity) * kGravity / std::max(5.0f, s.stiffness);
}

float JiggleMaxDisplacement(const JiggleSettings& s, int axis) {
    if (axis < 0 || axis > 2) return 0.0f;
    const float amplitude = axis == 0 ? s.amplitudeHorizontal : axis == 1 ? s.amplitudeVertical : s.amplitudeDepth;
    return kBaseDisplacement[axis] * std::max(0.0f, amplitude) * std::max(0.0f, s.strength);
}

float JiggleJointRange(const JiggleSettings& s, bool bust, int axis) {
    if (axis < 0 || axis > 2) return 0.0f;
    // X carries the flop (vertical and forward travel together); Z the sideways sway; Y a small twist so the part does not
    // spin on its own bone. This is the corrected reference-models mapping: the old export gave the twist (Y) more room than
    // the sway (Z), which is why the lateral motion was barely visible in MMD.
    const float flop = 0.5f * (s.amplitudeVertical + s.amplitudeDepth);
    const float share[3] = {flop, kTwistShare * s.amplitudeHorizontal, kSwayShare * s.amplitudeHorizontal};
    const float base = bust ? kBustJointRange : kButtJointRange;
    return std::min(1.5f, base * share[axis] * std::max(0.0f, s.strength));
}

} // namespace cardbridge::assemble
