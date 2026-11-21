#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cardbridge::assemble { struct SceneTexture; struct Scene; }

namespace cardbridge::exporter {

// The character maker and the game run their camera through AmplifyColor: a 32x32x32 colour look-up table (the game's "Lut_Amount")
// applied to the finished frame. Without it renders look pale next to the game. The table is a game asset, so it is not part of this
// repository: tools/extract_colour_grade.py copies it to <library>/colorgrade/lut_amount.png and LoadColourGrade reads it from there.
// Returns false (and leaves grading off) when the file is missing.
bool LoadColourGrade(const std::string& libraryDir);

// Loads the same 1024x32 RGB strip from memory (used by the loader and by tests that exercise grading without a game library).
bool LoadColourGradePixels(const uint8_t* rgb, int width, int height);

bool ColourGradeLoaded();
void SetColourGradeEnabled(bool on);
bool ColourGradeEnabled();

// Grades a finished frame (0xAARRGGBB pixels) in place; pixels equal to `skip` (the flat background) are left alone.
void ApplyColourGrade(std::vector<uint32_t>& bgra, uint32_t skip);

// The texture counterpart of ApplyColourGrade: grades the RGB of an sRGB colour texture exactly once and records that fact in
// SceneTexture::colourGraded. Masks, normal maps and other linear data (srgb == false) are never touched; alpha is preserved.
// Returns true when this call actually changed the texture. Both the export bake and the PMX writer use it, so the in-memory
// texture the as-exported view samples and the PNG the PMX points at cannot disagree about whether grading happened.
bool GradeTexture(assemble::SceneTexture& tex);

// Applies GradeTexture to every still-ungraded colour texture of a scene (idempotent). Called at the end of the as-exported
// bake so that materials the shader bake cannot run — KKS Koikano shaders, create-job textures, missing shaders — reach the
// plain/MMD renderer with the same graded bytes the PMX writer would write for them.
size_t ApplyOutstandingColourGrade(assemble::Scene& scene);

} // namespace cardbridge::exporter
