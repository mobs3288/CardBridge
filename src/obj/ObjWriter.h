#pragma once

#include "export/ModelExport.h"

namespace cardbridge::obj {

// Static geometry only: OBJ/MTL has no standard skinning, morph, alpha-test or hierarchy
// representation. Those features are reported as warnings rather than faked with extensions.
void WriteObj(const assemble::Scene& scene, const card::Card& card,
              const exporter::ModelExportOptions& opt, exporter::ModelExportResult& result);

} // namespace cardbridge::obj
