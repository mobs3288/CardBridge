#pragma once

#include <cstdint>
#include <vector>

namespace cardbridge::exporter {

// Decodes the embedded outfit-icon background template (a stage-style pink tiled backdrop
// with a filigree frame, embedded as JPEG bytes - see outfit_template.inc). The picture the
// outfit file carries is the mannequin composited over this: the template ships with CardBridge,
// so it works with no game files and looks identical everywhere. False when the embedded bytes
// fail to decode (a build problem, not a user problem).
bool DecodeOutfitTemplate(std::vector<uint8_t>& rgba, int& width, int& height);

} // namespace cardbridge::exporter
