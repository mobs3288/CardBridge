#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// The character maker and the game run their camera through AmplifyColor: a 32x32x32 colour look-up table (the game's "Lut_Amount")
// applied to the finished frame. Without it renders look pale next to the game. The table is a game asset, so it is not part of this
// repository: tools/extract_colour_grade.py copies it to <library>/colorgrade/lut_amount.png and LoadColourGrade reads it from there.
// Returns false (and leaves grading off) when the file is missing.
bool LoadColourGrade(const std::string& libraryDir);

bool ColourGradeLoaded();
void SetColourGradeEnabled(bool on);
bool ColourGradeEnabled();

// Grades a finished frame (0xAARRGGBB pixels) in place; pixels equal to `skip` (the flat background) are left alone.
void ApplyColourGrade(std::vector<uint32_t>& bgra, uint32_t skip);

} // namespace cardbridge::exporter
