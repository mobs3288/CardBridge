#pragma once

#include "core/Bytes.h"
#include <map>
#include <string>

namespace cardbridge::assemble {

// Stiletto "CustomHeel" (BepInEx/config/Stiletto/Heels/<Name>.txt, legacy installs
// BepInEx/Stiletto/<Name>.txt): the heel configuration the game applies when the
// shoe is worn. Angles are degrees (X-axis pitch, Euler(angle,0,0)), height is
// metres (body lift). Missing lines in a file default to 0, exactly as
// DynamicFileProvider does (angleToes/shoe* absent -> 0).
struct HeelConfig {
    float ankleDeg = 0;    // cf_j_foot local pitch
    float legDeg = 0;      // cf_j_leg03 local pitch
    float toeDeg = 0;      // cf_j_toes local pitch (shipped data: 0, toes follow the foot)
    float heightMeters = 0;  // _body lift
    // Shoe mesh warp (UpdateShoeWarp bindpose delta). All shipped .txt files
    // leave these at 0/1 (identity); parsed so future data can use them.
    float shoeScaleX = 1, shoeScaleY = 1, shoeScaleZ = 1;
    float shoeAngleDeg = 0;
    float shoeTranslateY = 0, shoeTranslateZ = 0;
    float shoeShearY = 0, shoeShearZ = 0;
    bool hasConfig = false;   // true when loaded from a Stiletto source (even 0/0/0 = flat)
    std::string source;       // "zipmod <guid>" / "known <Name>" for warnings
};

// Stiletto GetAngleInRange: angles beyond +/-60 fold to the other side of the circle.
float HeelAngleInRange(float deg);

// Parses one Stiletto heel file's text ("key=value" lines). Returns false when
// no recognized key is present (so callers fall back to geometry).
bool ParseStilettoHeel(const std::string& text, HeelConfig& out);

// The known base-game / legacy heel data transcribed from BepInEx/Stiletto/*.txt
// (keyed by ListInfoBase.Name, the file stem). 31 of 44 base co_shoes rows match
// exactly (trailing spaces preserved); the rest are legacy mod names. Providers
// with an entry get the game's own values instead of geometry estimates.
bool KnownHeelConfig(const std::string& shoeName, HeelConfig& out);

// All known entries (for tests: every Name round-trips).
const std::map<std::string, HeelConfig>& KnownHeelTable();

} // namespace cardbridge::assemble
