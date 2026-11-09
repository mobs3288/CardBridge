#pragma once

#include "assemble/Scene.h"

#include <functional>
#include <string>

namespace cardbridge::assemble {

// Universal hanging-cloth synthesis. For every Accessories / Outfit part that
// has hanging geometry but no authored DynamicBone chain (overhang coats,
// kimono wide sleeves, shoulder jackets, capes, wings), this creates chain
// bones down the hanging direction and rebinds the deep vertices to them, then
// adds a loose SceneChain. The existing PhysicsSim preview (viewer Y) and PMX
// export (rigid bodies + spring joints) then pick the result up unchanged.
// Authored chains always win: a part that already has one is left alone.
// Returns the number of chains added. Disabled by AssembleOptions::autoCloth
// == false or CARDBRIDGE_NO_AUTO_CLOTH=1 (checked by the caller).
int SynthesizeAutoCloth(Scene& scene, const std::function<void(const std::string&)>& log = {});

} // namespace cardbridge::assemble
