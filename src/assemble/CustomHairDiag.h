// Diagnostics for items that carry their own (often imported) skeleton - typical for custom "hair"
// converted from another game: a rig with its own bones, long strand chains, DynamicBone chains,
// skinned onto the shared skeleton of the model. This module MEASURES what the converter saw and
// verifies what it assumed. It never modifies geometry, bones or physics.
//
// The findings are reported into `SceneIssue`s (severity "info" for what the item is and what the
// converter had to assume; "warning" for a verified conversion inconsistency) and into one
// human-readable log line per part. Everything that had to be verified is verified here; nothing
// is guessed, and nothing is "corrected" because it looks unusual.
#pragma once

#include <string>
#include <vector>

namespace cardbridge::assemble {

// Measurements of one added item, gathered while the item is assembled.
struct ImportedRigReport {
    std::string partId;        // scene part id, e.g. "hair.back" or "outfit0.acc00"
    std::string group;         // Hair / Accessories / Outfit / ...
    std::string attachNode;    // accessories: the body socket they hang on; otherwise empty
    // What the item holds.
    int skinnedMeshes = 0;     // renderers with usable skinning, bone list and bind poses
    int customBones = 0;       // distinct skinned bone names that are NOT on the shared skeleton
    int chains = 0;            // DynamicBone chains the item carries (classic + Ver02)
    int droppedChains = 0;     // chains that had to be discarded (fewer than two valid bones)
    // Transform verification.
    float rootScale = 1.0f;    // the prefab root's smallest scale axis (1.0 unless the prefab carries one)
    float minScale = 1.0f;     // the smallest local-scale axis anywhere in the item (0 = broken prefab)
    float maxScale = 1.0f;     // the largest local-scale axis anywhere in the item
    float bindScaleErr = 0.0f; // relative error between bind-pose scale and hierarchy scale (0 = consistent)
    float strandDrop = 0.0f;   // how far the lowest vertex of the item hangs below its attachment point (model units)
};

// What the measurements mean.
struct CustomRigVerdict {
    bool importedRig = false;          // custom skeleton detected: item reports it, nothing else changes
    bool hairLike = false;             // imported hair-like: head-attached custom skeleton with strand chains
    std::string headline;              // one line: what this item is, for the log / manifest
    std::vector<std::string> infos;    // honest statement of what the converter did/kept (severity "info")
    std::vector<std::string> warnings; // verified inconsistency (severity "warning")
};

// Pure analysis: no side effects, no allocations beyond the strings of the report.
CustomRigVerdict AnalyzeImportedRig(const ImportedRigReport& r);

// True for the head-attached sockets an item can hang on (head / face / hair sockets, e.g. a_n_headside).
bool IsHeadAttachNode(const std::string& node);

} // namespace cardbridge::assemble
