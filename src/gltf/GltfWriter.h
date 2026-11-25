#pragma once

#include "export/ModelExport.h"

namespace cardbridge::gltf {

// Both writers build the same document from the canonical Scene. The only difference is the
// buffer/image encoding: GLB packs everything into one binary chunk, glTF writes sibling files.
void WriteGlb(const assemble::Scene& scene, const card::Card& card,
              const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result);
void WriteGltf(const assemble::Scene& scene, const card::Card& card,
               const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result);

} // namespace cardbridge::gltf
