// viewer fragment: the Morphs tab - every morph of the exported PMX, live (included in order by main.cpp)
//
// The Morphs tab lists the exact vertex morphs the exported PMX carries, built from the scene by exporter::BuildLiveMorphs
// (the same code the export uses to write the file, so names, panels and offsets match the PMX that would come out of this
// scene). A slider adds weight * offset to the rest vertices before skinning, the same way the game applies blend shapes and
// MikuMikuDance applies PMX vertex morphs - so the movement seen here is the movement the PMX does. The outfit and nude
// toggles are material morphs in the PMX (they hide and show meshes, not move vertices); the Parts tab does that job.
// Bones move in the Bones tab, the DynamicBone physics in the Look tab (Y) - together that is every way the PMX moves.

// per-mesh accumulation of the current weights; handed to the renderers (the GPU view folds it into the rest vertices
// before skinning, the software view adds it to its copied frame)
void RebuildMorphDelta() {
    std::shared_ptr<Loaded> L = Current();
    g.morphDelta.assign(L ? L->scene.meshes.size() : 0, {});
    if (!L) return;
    for (size_t i = 0; i < g.morphs.size() && i < g.morphWeights.size(); ++i) {
        const float w = g.morphWeights[i];
        if (w == 0.0f) continue;
        const exporter::LiveMorph& morph = g.morphs[i];
        for (size_t mi = 0; mi < morph.byMesh.size() && mi < g.morphDelta.size(); ++mi) {
            const std::vector<std::pair<uint32_t, Vec3>>& offs = morph.byMesh[mi];
            if (offs.empty()) continue;
            std::vector<Vec3>& dst = g.morphDelta[mi];
            if (dst.empty()) dst.assign(L->scene.meshes[mi].vertices.size(), Vec3{0, 0, 0});
            for (const auto& [vi, d] : offs) {
                Vec3& a = dst[vi];
                a.x += d.x * w; a.y += d.y * w; a.z += d.z * w;
            }
        }
    }
    if (g.gpuOk && g.gpuScene == L) g.gpu.SetMorphDelta(&g.morphDelta);   // store; the caller refreshes the skin
}

// Builds the catalog from the current scene. `keepWeights` (a re-shape: the same scene, same order) carries the current
// weights over by index; a fresh load starts every weight at 0.
void SetMorphCatalog(bool keepWeights) {
    std::shared_ptr<Loaded> L = Current();
    std::vector<float> old = std::move(g.morphWeights);
    std::vector<exporter::LiveMorph> catalog;
    try { if (L) catalog = exporter::BuildLiveMorphs(L->scene); }
    catch (const std::exception& e) { Log(std::string("morphs: could not build the exported PMX's morph list: ") + e.what(), LogKind::Warn); }
    g.morphs = std::move(catalog);
    g.morphWeights.assign(g.morphs.size(), 0.0f);
    if (keepWeights)
        for (size_t i = 0; i < g.morphs.size() && i < old.size(); ++i) g.morphWeights[i] = old[i];
    g.morphDelta.assign(L ? L->scene.meshes.size() : 0, {});
    RebuildMorphDelta();   // folds the kept weights (usually none on load) into the GPU copy
}

void LoadMorphCatalog() { SetMorphCatalog(false); }
void RebuildMorphCatalog() { SetMorphCatalog(true); }

// A morph slider (or the tab's reset) changed: accumulate and show at once. No re-assembly, no re-skin math needed -
// the GPU view re-uploads the rest with the deltas folded in, the software view reads morphDelta on the next picture.
void MorphSlideEdited() {
    RebuildMorphDelta();
    if (g.showExp) {   // the X / "as exported" copy is a separate baked scene: it cannot show live morphs
        g.showExp = false;
        Log("as exported: off (morphs play in the live view)");
    }
    std::shared_ptr<Loaded> L = Current();
    if (g.gpuOk && g.gpuScene == L) g.gpu.Refresh(false);   // rebuild the rest with the deltas folded in and re-skin (the pose rides along)
    else { g.dirty = true; InvalidateView(); }
    InvalidatePanel();
}

// All weights at zero, and their number for the header status ("3 of 212 on")
size_t MorphsOn() {
    size_t on = 0;
    for (float w : g.morphWeights) if (std::fabs(w) > 1e-4f) ++on;
    return on;
}

void ResetAllMorphs() {
    std::fill(g.morphWeights.begin(), g.morphWeights.end(), 0.0f);
    MorphSlideEdited();
    Log("reset every morph (0)");
}
