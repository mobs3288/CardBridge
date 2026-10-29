#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace cardbridge::exporter {

struct ExportOptions {
    std::string outDir;            // created; must be empty or absent unless `overwrite`
    std::string baseName;          // file stem; defaults to the character name
    float scale = 12.5f;           // model units per metre (MMD convention)
    bool overwrite = false;
    int outfit = 0;                // recorded in the manifest
    std::string assetLibrary;      // recorded as provenance only
    std::string modsFolder;        // recorded as provenance only
    // Optional progress sink (the viewer's live log): called with short stage descriptions.
    std::function<void(const std::string&)> log;
};

struct ExportResult {
    std::string folder, pmxPath, manifestPath, previewPath;
    size_t vertices = 0, triangles = 0, materials = 0, bones = 0, textures = 0, parts = 0;
};

// Writes <outDir>/<name>.pmx, <outDir>/textures/*.png and
// <outDir>/<name>.cardbridge.json - one self-contained folder with relative
// paths only, the contract the engine importer reads (plan section 16.1).
// Parts with includeInExport == false are left out of the PMX but stay listed
// in the manifest. Throws FormatError on IO or consistency errors.
ExportResult ExportScene(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt);

// Structural checks on an assembled scene, each finding one line: indices outside their arrays, non-finite positions, vertex
// weights that do not sum to 1, bones, materials or textures that do not exist, and parts whose geometry lies far outside the
// body (a transform applied twice or not at all). Empty = nothing found. Tolerances: weights 1e-3, a part may not reach more
// than 25 % above the head's top or 15 % of the body height below the floor.
std::vector<std::string> ValidateScene(const assemble::Scene& scene);

// The manifest as JSON (exposed for tests).
nlohmann::json BuildManifest(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt,
                             const std::string& pmxFile, const std::vector<std::string>& textureFiles,
                             const std::vector<std::pair<int, int>>& partMaterialRange);

} // namespace cardbridge::exporter
