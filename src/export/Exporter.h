#pragma once

#include "assemble/Scene.h"
#include "card/CardFile.h"

#include <nlohmann/json.hpp>

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
};

struct ExportResult {
    std::string folder, pmxPath, manifestPath;
    size_t vertices = 0, triangles = 0, materials = 0, bones = 0, textures = 0, parts = 0;
};

// Writes <outDir>/<name>.pmx, <outDir>/textures/*.png and
// <outDir>/<name>.cardbridge.json - one self-contained folder with relative
// paths only, the contract the engine importer reads (plan section 16.1).
// Parts with includeInExport == false are left out of the PMX but stay listed
// in the manifest. Throws FormatError on IO or consistency errors.
ExportResult ExportScene(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt);

// The manifest as JSON (exposed for tests).
nlohmann::json BuildManifest(const assemble::Scene& scene, const card::Card& card, const ExportOptions& opt,
                             const std::string& pmxFile, const std::vector<std::string>& textureFiles,
                             const std::vector<std::pair<int, int>>& partMaterialRange);

} // namespace cardbridge::exporter
