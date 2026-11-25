#include "assemble/BraPhysicsProfile.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::assemble {

namespace {

float cap(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

} // namespace

JiggleSettings SupportedBraJiggle(const JiggleSettings& base) {
    const JiggleSettings b = SanitizeJiggleSettings(base);
    const JiggleSettings support = JigglePresetSettings(JigglePreset::Subtle);

    JiggleSettings s = b;
    // The garment supplies confinement, not a change in mass. The spring is at least as firm as the measured
    // supportive preset but never softer than what the user chose.
    s.stiffness = cap(std::max(b.stiffness, support.stiffness), 5.0f, 5000.0f);
    // Keep the user's damping ratio unless it is more oscillatory than the supportive preset's; then raise it to
    // that reference (near-critical without becoming rigid). Recompute the damper at the new spring rate so the
    // ratio actually stays the same: c = 2 zeta sqrt(k m).
    const float zeta = cap(std::max(JiggleDampingRatio(b), JiggleDampingRatio(support)), 0.0f, 0.70f);
    s.damping = cap(2.0f * zeta * std::sqrt(s.stiffness * std::max(1e-4f, s.mass)), 0.0f, 200.0f);
    // A bra constrains up/down travel more than side-to-side travel; keep whatever smaller range the user chose.
    s.amplitudeVertical = std::min(b.amplitudeVertical, support.amplitudeVertical * 0.85f);
    s.amplitudeDepth = std::min(b.amplitudeDepth, support.amplitudeDepth * 0.80f);
    s.amplitudeHorizontal = std::min(b.amplitudeHorizontal, support.amplitudeHorizontal);
    // The bone should follow the fabric's motion less than free skin's, and the master strength may only go down.
    s.rotationStrength = std::min(b.rotationStrength, support.rotationStrength);
    s.strength = std::min(b.strength, support.strength);
    s.gravity = std::min(b.gravity, support.gravity);
    return SanitizeJiggleSettings(s);
}

} // namespace cardbridge::assemble
