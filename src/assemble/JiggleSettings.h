#pragma once

#include <string>

namespace cardbridge::assemble {

// The physical settings of the body's own secondary motion (breast and hip jiggle). One set drives both the live preview and the PMX
// export, so what the viewer shows and what MikuMikuDance plays come from the same numbers.
//
// The units are real: mass in kilograms, stiffness in newtons per metre, damping in newton-seconds per metre, gravity as a multiple
// of 9.81 m/s^2. The amplitudes scale how far each world axis may swing from the posed rest shape (0 locks the axis); the rotation
// strength blends the bone's inherited parent rotation with the rotation that aims it at its child (0 = rigid with the parent,
// 1 = fully follows the simulated chain). `strength` is the master scale of the whole effect, kept as the old jiggle intensity.
struct JiggleSettings {
    float mass = 1.0f;              // kg, the root link (the links below it taper)
    float stiffness = 150.0f;       // N/m, the spring that pulls a link back to its posed place
    float damping = 10.0f;          // N s/m, the velocity the link loses (Natural: natural frequency ~1.95 Hz, damping ratio ~0.41)
    float gravity = 1.0f;           // x 9.81 m/s^2; at rest the body fully supports the part (no static sag); gravity only swings it
    float amplitudeVertical = 1.0f;   // how far the links may travel up / down
    float amplitudeHorizontal = 1.0f; // how far they may sway sideways (the visible left / right jiggle)
    float amplitudeDepth = 1.0f;      // how far they may flop forward / back
    float rotationStrength = 1.0f;    // how strongly a bone turns to follow its simulated child
    float strength = 1.0f;            // master scale of the whole effect (the old --jiggle-intensity); dynamic only, never the static sag
    float simulationHz = 120.0f;      // fixed simulation rate of the preview (the PMX runs in MMD's own solver)
};

enum class JigglePreset { Subtle, VerySubtle, VeryVerySubtle, Natural, Dynamic, Custom };

// The share of the part's weight the body keeps itself. It is 1.0: the body fully supports the part at rest, so the physics never
// adds any static sag of its own - the Sag slider is the ONLY source of resting droop. (The formula is kept for the manifest, which
// reports the gravity-induced sag as 0 under this model.)
inline constexpr float kJiggleSupport = 1.0f;

const char* JigglePresetName(JigglePreset preset);
// Subtle, Natural and Dynamic are tuned on the same physical model, not as arbitrary multipliers.
JiggleSettings JigglePresetSettings(JigglePreset preset);
// The preset whose numbers `s` has, or Custom when it differs.
JigglePreset MatchJigglePreset(const JiggleSettings& s);

// Replaces NaN / infinities with the Natural default and clamps every value into a range the fixed-step solver (and the PMX exporter)
// can handle. Safe to call on user input from the CLI and viewer.
JiggleSettings SanitizeJiggleSettings(JiggleSettings s);

// ---- derived values (all finite for a sanitized setting) -------------------------------------------------------------------
// Undamped angular frequency of the root link, radians a second.
float JiggleAngularFrequency(const JiggleSettings& s);
// Damping ratio c / (2 sqrt(k m)); 1 = critically damped, below 1 rings.
float JiggleDampingRatio(const JiggleSettings& s);
// The PMX rigid body's damping: Bullet keeps v *= (1 - d)^dt per second, so the matching decay is 1 - exp(-c / (2 m)).
float JigglePmxDamping(const JiggleSettings& s, float linkMass);
// The static sag the body does not support, metres (the preview starts at the posed shape and settles this far down).
float JiggleStaticSagMeters(const JiggleSettings& s);
// The largest displacement from the posed rest shape on a world axis: 0 = X (horizontal), 1 = Y (vertical), 2 = Z (depth).
float JiggleMaxDisplacement(const JiggleSettings& s, int axis);
// The largest turn of one PMX joint, radians. The three axes are the PMX joint's own: X flop (vertical / depth), Z sway (horizontal),
// Y twist (small, so the part does not spin on its own bone).
float JiggleJointRange(const JiggleSettings& s, bool bust, int axis);

} // namespace cardbridge::assemble
