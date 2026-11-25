#pragma once

#include "assemble/JiggleSettings.h"

namespace cardbridge::assemble {

// The supported-breast profile. A supportive bra does not change the character's tissue: it holds it closer to the
// chest wall, resists vertical bounce more than lateral sway, and settles faster. CardBridge already has a measured
// setting for exactly that mechanical behaviour - the "Subtle" preset ("firm and well damped ... like a tight sports
// top") - so the profile is derived from that tuned reference and from the user's own values. There are no scattered
// multipliers in the exporter, and no game/character-specific numbers here.
//
// The function always returns a NEW, sanitized setting; `base` is never modified. When no bra is detected the caller
// passes the original settings through AddPhysics untouched, so the existing no-bra physics is bit-identical.
JiggleSettings SupportedBraJiggle(const JiggleSettings& base);

} // namespace cardbridge::assemble
