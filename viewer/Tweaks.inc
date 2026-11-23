// viewer fragment: slider access (included in order by main.cpp)

bool ReshapeNow(bool final);   // defined in Pose.inc
void TweakEdited(int id, bool released);   // defined in Pose.inc
void ApplyNoseRidgeLive();                  // defined in Pose.inc
void JiggleSettingsChanged();              // defined in Pose.inc

// ---- tweak access ----------------------------------------------------------------------------------------------------
// A row wraps either one float (almost all of them) or a virtual value with its own read / write: the three Body
// measurements rows in the Body tab are one control each that drives the underlying shape sliders together. `get` /
// `set` take over from `v` when set, so every call site goes through Value() / Set() and does not care which of the two
// the row is. A step of 0 means the slider snaps to nothing: a typed number is kept exactly.
struct SliderRef {
    float* v = nullptr;
    float def = 0, lo = 0, hi = 1, step = 0.01f;
    bool percent = true;   // shown as 0..100 %, otherwise as plain millimetres
    const char* fmt = nullptr;   // a printf format for the value (one double), instead of the two above
    std::function<float()> get;          // a virtual read (Body measurements); overrides *v when set
    std::function<void(float)> set;      // a virtual write; overrides *v = ... when set
    bool Valid() const { return v != nullptr || static_cast<bool>(get); }
    float Value() const { return get ? get() : (v ? *v : 0.0f); }
    void Set(float nv) const { if (set) set(nv); else if (v) *v = nv; }
};

// ---- the Body tab's "Body measurements" group: Bust / Waist / Hip as real measurements -------------------------------
// The maker has no BWH slider, so the rows read the skin: the perimeter of the torso at the bust / waist / hip heights,
// in metres (the scene's unit), shown as cm. Each row couples the sliders that drive one measurement (bust -> body 4,
// waist -> body 22/23, hip -> body 24/25, the bones those tables deform): a drag moves them together, keeping the card's
// own width against its depth. Typing a value solves for the coupling: the typed cm is reached by re-shaping in a closed
// loop until the measured perimeter matches (the slider clamps are the reachability limits; the closest reachable value
// wins when the target is out of reach). Dragging is live like any body slider; the underlying row below moves with it.
bool IsBodyMeasureId(int id) { return id == kBwhBust || id == kBwhWaist || id == kBwhHip; }

int BodyMeasureIndexOf(int id) {
    if (id == kBwhBust) return shape::kBodyMeasureBust;
    if (id == kBwhWaist) return shape::kBodyMeasureWaist;
    return shape::kBodyMeasureHip;
}

const std::array<int, 2>& BodyMeasureSliders(int measure) {
    static const std::array<int, 2> bust = {4, -1};
    static const std::array<int, 2> waist = {22, 23};
    static const std::array<int, 2> hip = {24, 25};
    return measure == shape::kBodyMeasureBust ? bust : measure == shape::kBodyMeasureWaist ? waist : hip;
}

float BwhValueCm(int id) {   // the measured perimeter in cm (0 when the measurement is unavailable)
    if (!g.bwhMeasure.valid) return 0.0f;
    switch (BodyMeasureIndexOf(id)) {
        case shape::kBodyMeasureBust: return g.bwhMeasure.bust * 100.0f;
        case shape::kBodyMeasureWaist: return g.bwhMeasure.waist * 100.0f;
        default: return g.bwhMeasure.hip * 100.0f;
    }
}
float BwhCardDefCm(int id) {   // the card's own measurement, what the row's reset restores to
    return g.bwhCardDef[BodyMeasureIndexOf(id)] * 100.0f;
}

void BwhApplyCoupled(int measure, float delta, const std::array<float, 2>& base) {
    const auto& sliders = BodyMeasureSliders(measure);
    for (int i = 0; i < 2 && sliders[static_cast<size_t>(i)] >= 0; ++i) {
        const size_t s = static_cast<size_t>(sliders[static_cast<size_t>(i)]);
        const float card = g.tw.bodyCard[s];
        g.tw.body[s] = std::clamp(base[static_cast<size_t>(i)] + delta, shape::BodyMeasureSliderLo(card), shape::BodyMeasureSliderHi(card));
    }
}
float BwhMeasureFor(int measure) {
    switch (measure) {
        case shape::kBodyMeasureBust: return g.bwhMeasure.bust;
        case shape::kBodyMeasureWaist: return g.bwhMeasure.waist;
        default: return g.bwhMeasure.hip;
    }
}

// Solve for the coupled delta that lands the measurement on `targetCm`. Re-shapes many times with g.bwhSolving set (the
// screen stays still) and leaves the body at the best result; the typed commit re-shapes once more with the full path.
void BodyMeasureSolve(int id, float targetCm) {
    std::shared_ptr<Loaded> L = Current();
    if (!L) return;
    const int measure = BodyMeasureIndexOf(id);
    if (!g.bwhMeasure.valid) {   // no body to measure: fall back to the plain coupled-delta mapping
        shape::BodyMeasureSetTarget(g.tw.body, g.tw.bodyCard, measure, std::clamp(targetCm, -100.0f, 200.0f) / 100.0f);
        return;
    }
    const auto& sliders = BodyMeasureSliders(measure);
    std::array<float, 2> backup{};
    for (int i = 0; i < 2 && sliders[static_cast<size_t>(i)] >= 0; ++i)
        backup[static_cast<size_t>(i)] = g.tw.body[static_cast<size_t>(sliders[static_cast<size_t>(i)])];
    float dLo = -1e9f, dHi = 1e9f;
    for (int i = 0; i < 2 && sliders[static_cast<size_t>(i)] >= 0; ++i) {
        const float card = g.tw.bodyCard[static_cast<size_t>(sliders[static_cast<size_t>(i)])];
        dLo = std::max(dLo, shape::BodyMeasureSliderLo(card) - backup[static_cast<size_t>(i)]);
        dHi = std::min(dHi, shape::BodyMeasureSliderHi(card) - backup[static_cast<size_t>(i)]);
    }
    if (dLo > dHi) return;
    const float target = std::clamp(targetCm, 10.0f, 200.0f) * 0.01f;
    g.bwhSolving = true;
    auto measureAt = [&](float d) {
        BwhApplyCoupled(measure, d, backup);
        ReshapeNow(false);   // suppressed: updates the measurement, touches nothing on screen
        return BwhMeasureFor(measure);
    };
    const float atLo = measureAt(dLo), atHi = measureAt(dHi);
    float bestD = dLo, bestErr = std::fabs(atLo - target);
    if (std::fabs(atHi - target) < bestErr) { bestD = dHi; bestErr = std::fabs(atHi - target); }
    const bool straddles = (atLo - target) * (atHi - target) <= 0.0f;
    if (straddles) {
        float lo = dLo, hi = dHi, plo = atLo;
        for (int it = 0; it < 14; ++it) {
            const float mid = 0.5f * (lo + hi);
            const float p = measureAt(mid);
            const float err = std::fabs(p - target);
            if (err < bestErr) { bestErr = err; bestD = mid; }
            if ((plo < target) == (p < target)) { lo = mid; plo = p; } else hi = mid;
        }
    }
    BwhApplyCoupled(measure, bestD, backup);
    ReshapeNow(false);
    g.bwhSolving = false;
    if (bestErr > 0.005f) {
        const char* name = id == kBwhBust ? "Bust" : id == kBwhWaist ? "Waist" : "Hip";
        char msg[96];
        std::snprintf(msg, sizeof msg, "%s cannot reach %.1f cm on this card; nearest is %.1f cm", name, static_cast<double>(targetCm),
                      static_cast<double>(BwhValueCm(id)));
        Log(msg, LogKind::Warn);
    }
}

bool BwhFor(int id, SliderRef& r) {
    if (g.tw.body.size() != g.tw.bodyCard.size() || g.tw.body.empty()) return false;
    const int measure = BodyMeasureIndexOf(id);
    if (g.bwhMeasure.valid) {
        const float current = BwhValueCm(id);
        const float def = BwhCardDefCm(id);
        r.lo = std::max(20.0f, def - 40.0f); r.hi = std::min(180.0f, def + 40.0f);
        if (r.hi - r.lo < 40.0f) { r.lo = 20.0f; r.hi = 180.0f; }
        r.def = def; r.step = 0; r.percent = false; r.fmt = "%.1f cm";
        r.get = [id]() { return BwhValueCm(id); };
        r.set = [id](float v) { BodyMeasureSolve(id, v); };
        (void)current;
        return true;
    }
    float lo = 0.0f, hi = 0.0f;
    shape::BodyMeasureRange(g.tw.bodyCard, measure, lo, hi);
    r.lo = lo * 100.0f; r.hi = hi * 100.0f; r.def = shape::BodyMeasureAverage(g.tw.bodyCard, measure) * 100.0f;
    r.step = 0;                     // no snap: a typed number is kept exactly
    r.percent = false;              // the value *is* the percentage; the box types what the row shows
    r.fmt = "%.1f%%";
    r.get = [id]() { return shape::BodyMeasureAverage(g.tw.body, BodyMeasureIndexOf(id)) * 100.0f; };
    r.set = [id](float v) {
        if (!std::isfinite(v)) return;
        shape::BodyMeasureSetTarget(g.tw.body, g.tw.bodyCard, BodyMeasureIndexOf(id), v / 100.0f);
    };
    return true;
}

void BwhDragBegin(int id) {
    const auto& sliders = BodyMeasureSliders(BodyMeasureIndexOf(id));
    g.bwhDragId = id;
    for (int i = 0; i < 2 && sliders[static_cast<size_t>(i)] >= 0; ++i)
        g.bwhDragBase[static_cast<size_t>(i)] = g.tw.body[static_cast<size_t>(sliders[static_cast<size_t>(i)])];
}
void BwhDragAt(int id, float frac) {
    if (g.bwhDragId != id) BwhDragBegin(id);
    const int measure = BodyMeasureIndexOf(id);
    const auto& sliders = BodyMeasureSliders(measure);
    float dLo = -1e9f, dHi = 1e9f;
    for (int i = 0; i < 2 && sliders[static_cast<size_t>(i)] >= 0; ++i) {
        const float card = g.tw.bodyCard[static_cast<size_t>(sliders[static_cast<size_t>(i)])];
        dLo = std::max(dLo, shape::BodyMeasureSliderLo(card) - g.bwhDragBase[static_cast<size_t>(i)]);
        dHi = std::min(dHi, shape::BodyMeasureSliderHi(card) - g.bwhDragBase[static_cast<size_t>(i)]);
    }
    if (dLo > dHi) return;
    const float d = dLo + std::clamp(frac, 0.0f, 1.0f) * (dHi - dLo);
    BwhApplyCoupled(measure, d, g.bwhDragBase);
}

bool SliderFor(int id, SliderRef& r) {
    if (IsBodyMeasureId(id)) return BwhFor(id, r);
    if (id >= 0 && id < kFaceBase) {
        if (static_cast<size_t>(id) >= g.tw.body.size()) return false;
        r.v = &g.tw.body[static_cast<size_t>(id)];
        r.def = g.tw.bodyCard[static_cast<size_t>(id)];
        r.lo = std::min(-1.0f, r.def); r.hi = std::max(2.0f, r.def);
        return true;
    }
    if (id >= kFaceBase && id < kNippleMm) {
        size_t i = static_cast<size_t>(id - kFaceBase);
        if (i >= g.tw.face.size()) return false;
        r.v = &g.tw.face[i];
        r.def = g.tw.faceCard[i];
        r.lo = std::min(-1.0f, r.def); r.hi = std::max(2.0f, r.def);
        return true;
    }
    if (id == kNippleMm) { r.v = &g.tw.nippleMm; r.def = 0; r.lo = -15; r.hi = 15; r.step = 1; r.percent = false; return true; }
    if (id == kAreola) { r.v = &g.tw.areola; r.def = g.tw.areolaCard; r.lo = 0; r.hi = 1; return true; }
    if (id == kSkinShade) { r.v = &g.tw.skinShade; r.def = 1.5f; r.lo = 0; r.hi = 2; r.step = 0.05f; return true; }
    if (id == kSkinShadeUnder) { r.v = &g.tw.skinShadeUnder; r.def = 1.0f; r.lo = 0; r.hi = 2; r.step = 0.05f; return true; }
    if (id == kSkinShadeTop) { r.v = &g.tw.skinShadeTop; r.def = 1.2f; r.lo = 0; r.hi = 2; r.step = 0.05f; return true; }
    if (id == kSkinShadeWidth) { r.v = &g.tw.skinShadeWidth; r.def = 0.6f; r.lo = 0.1f; r.hi = 2; r.step = 0.05f; return true; }
    if (id == kSkinAo) { r.v = &g.tw.skinAo; r.def = 0.85f; r.lo = 0; r.hi = 2; r.step = 0.05f; return true; }
    if (id == kNoseRidge) { r.v = &g.tw.noseRidge; r.def = 1.0f; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kNoseRidgeOpacity) {
        r.get = [] { return g.tw.noseRidgeOpacity; };
        r.set = [](float v) { g.tw.noseRidgeOpacity = v; ApplyNoseRidgeLive(); };   // lightweight live layer
        r.def = 1.0f; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; return true;
    }
    if (id == kTransStrength) { r.v = &g.tw.transStrength; r.def = 0.65f; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }   // shown as 0..100
    if (id == kTransSize) { r.v = &g.tw.transSizeCm; r.def = 4.0f; r.lo = 0.5f; r.hi = 15.0f; r.step = 0.25f; r.percent = false; r.fmt = "%.2f cm"; return true; }
    if (id == kNippleR) { r.v = &g.tw.nippleColour.x; r.def = assemble::kDefaultNippleColour.x; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kNippleG) { r.v = &g.tw.nippleColour.y; r.def = assemble::kDefaultNippleColour.y; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kNippleB) { r.v = &g.tw.nippleColour.z; r.def = assemble::kDefaultNippleColour.z; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kAPoseDeg) { r.v = &g.aPoseDeg; r.def = 40.0f; r.lo = 15; r.hi = 70; r.step = 1; r.percent = false; r.fmt = "%.0f"; return true; }
    if (id == kHeightCm) { r.v = &g.heightCm; r.def = g.baseHeightCm; r.lo = 100; r.hi = 250; r.step = 1; r.percent = false; r.fmt = "%.0f cm"; return true; }
    if (id == kSag) { r.v = &g.sag; r.def = g.sagDefault; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleMass) { r.v = &g.jiggle.mass; r.def = 1.0f; r.lo = 0.05f; r.hi = 5.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f kg"; return true; }
    if (id == kJiggleStiffness) { r.v = &g.jiggle.stiffness; r.def = 150.0f; r.lo = 20.0f; r.hi = 2000.0f; r.step = 5.0f; r.percent = false; r.fmt = "%.0f N/m"; return true; }
    if (id == kJiggleDamping) { r.v = &g.jiggle.damping; r.def = 10.0f; r.lo = 0.0f; r.hi = 40.0f; r.step = 0.1f; r.percent = false; r.fmt = "%.1f N s/m"; return true; }
    if (id == kJiggleGravity) { r.v = &g.jiggle.gravity; r.def = 1.0f; r.lo = 0.0f; r.hi = 3.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f x"; return true; }
    if (id == kJiggleAmpVertical) { r.v = &g.jiggle.amplitudeVertical; r.def = 1.0f; r.lo = 0.0f; r.hi = 2.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleAmpHorizontal) { r.v = &g.jiggle.amplitudeHorizontal; r.def = 1.0f; r.lo = 0.0f; r.hi = 2.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleAmpDepth) { r.v = &g.jiggle.amplitudeDepth; r.def = 1.0f; r.lo = 0.0f; r.hi = 2.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleRotation) { r.v = &g.jiggle.rotationStrength; r.def = 1.0f; r.lo = 0.0f; r.hi = 2.0f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleStrength) { r.v = &g.jiggle.strength; r.def = 1.0f; r.lo = 0.0f; r.hi = 2.5f; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleSimHz) { r.v = &g.jiggle.simulationHz; r.def = 120.0f; r.lo = 30.0f; r.hi = 240.0f; r.step = 10.0f; r.percent = false; r.fmt = "%.0f Hz"; return true; }
    if (id >= kMorphBase && id < kMorphBase + static_cast<int>(g.morphWeights.size())) {   // a morph of the exported PMX
        const size_t i = static_cast<size_t>(id - kMorphBase);
        r.v = &g.morphWeights[i];
        r.def = 0; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; r.fmt = nullptr;
        return true;
    }
    if (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) {   // a field of the selected bone
        if (g.selBoneName.empty()) return false;
        assemble::BonePose& e = g.boneEdits[g.selBoneName];
        float* f[kBoneEditCount] = {&e.pos.x, &e.pos.y, &e.pos.z, &e.rot.x, &e.rot.y, &e.rot.z, &e.scl.x, &e.scl.y, &e.scl.z};
        const int k = id - kBoneEdit;
        r.v = f[k];
        r.percent = false;
        if (k < 3) { r.def = 0; r.lo = -0.3f; r.hi = 0.3f; r.step = 0.001f; r.fmt = "%+.3f"; }
        else if (k < 6) { r.def = 0; r.lo = -180; r.hi = 180; r.step = 1; r.fmt = "%+.0f"; }
        else { r.def = 1; r.lo = 0.1f; r.hi = 3; r.step = 0.01f; r.fmt = "%.2f"; }
        return true;
    }
    if (id >= kMatBase && id < kMatBase + kMatCount) {   // a field of the selected material
        std::shared_ptr<Loaded> L = Current();
        if (!L || g.selMaterial < 0 || static_cast<size_t>(g.selMaterial) >= g.mat.originals.size()) return false;
        const int k = id - kMatBase;
        material::EffectiveMaterial orig = material::EffectiveOf(g.mat.originals[static_cast<size_t>(g.selMaterial)], nullptr);
        r.v = &g.matBuf[k];
        switch (k) {
        case 0: case 1: case 2:
            r.def = (k == 0 ? orig.baseColor.x : k == 1 ? orig.baseColor.y : orig.baseColor.z) * 255.0f;
            r.lo = 0; r.hi = 255; r.step = 1; r.percent = false; r.fmt = "%.0f"; return true;   // %d with the float value printed garbage
        case 3: r.def = orig.opacity; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; r.fmt = nullptr; return true;
        case 4: r.def = orig.roughness; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; r.fmt = nullptr; return true;
        case 5: r.def = orig.metallic; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; r.fmt = nullptr; return true;
        case 6: r.def = orig.specularPower; r.lo = 0; r.hi = 1; r.step = 0.01f; r.percent = true; r.fmt = nullptr; return true;
        case 7: r.def = orig.emissionIntensity; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true;
        case 8: r.def = orig.normalIntensity; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true;
        default: return false;
        }
    }
    return false;
}

// Pose controls change the picture at once, no re-assembly. Morphs too: a weight is a live edit, like a pose bone.
bool IsLiveId(int id) { return id == kAPose || id == kAPoseDeg || id == kHeightCm || id == kSag || (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) || (id >= kMatBase && id < kMatBase + kMatCount) || (id >= kMorphBase && id < kMorphBase + static_cast<int>(g.morphWeights.size())); }
void TransSwitchChanged(int id);   // defined in Paint.inc (the translucency switch: apply or revert the bake at once)
void BakeQualityChanged();         // defined in Export.inc (reload the card so shader maps use the preset cap)
void TransApplyNow();              // defined in Paint.inc (re-applies the baked alpha from the stored masks)
void TransClearPaint();            // defined in Paint.inc (the "clear the brush" button)

// ---- preset chips: the band + cup pair for the bust size, S..XL for the nipple size ---------------------------------
// A plain slider is hard to shop for, so the Bust Size row carries a band (65..105) and a cup (AA..G), and the Look tab's
// nipple row carries simple sizes. The chips write the same sliders a drag would (bust: body 4, mapped linearly across
// 0..1 as one number, band * 8 cups; nipple: the out/in offset in mm), so everything downstream (re-shape, export, reset,
// undo) sees an ordinary slider change.
constexpr int kBustBandCount = 9;                         // 65..105 in steps of 10
constexpr int kBustCupCount = 8;                          // AA A B C D E F G
constexpr int kPresetCombos = kBustBandCount * kBustCupCount;   // 72: band and cup as one number
const char* const kBustBandLabels[kBustBandCount] = {"65", "70", "75", "80", "85", "90", "95", "100", "105"};
const char* const kBustCupLabels[kBustCupCount] = {"AA", "A", "B", "C", "D", "E", "F", "G"};
constexpr float kNippleSizeMm[4] = {-4.0f, 0.0f, 4.0f, 8.0f};   // out/in offsets per preset; the card default is 0
const char* const kNippleSizeLabels[4] = {"S", "M", "L", "XL"};
// The five presets; the first is the tightest (a sports top), the last is the most lively.
const char* const kJigglePresetLabels[5] = {"Subtle", "Very Subtle", "V.V. Subtle", "Natural", "Dynamic"};
const char* const kBakeQualityLabels[3] = {"Standard", "High", "Ultra"};
const char* const kUpscalerLabels[2] = {"Waifu2x", "Real-ESRGAN"};

int PresetChipCount(int rowId) { return rowId == kNippleSize ? 4 : rowId == kBustCup ? kBustCupCount : rowId == kBustBand ? kBustBandCount : rowId == kJigglePreset ? 5 : rowId == kBakeQuality ? 3 : rowId == kBakeUpscalerModel ? 2 : 0; }
int PresetChipW(int rowId) { return rowId == kNippleSize ? 52 : rowId == kBustCup ? 37 : rowId == kJigglePreset ? 70 : rowId == kBakeQuality ? 62 : rowId == kBakeUpscalerModel ? 100 : 32; }
int PresetChipGap(int rowId) { return rowId == kNippleSize ? 6 : rowId == kJigglePreset || rowId == kBakeQuality || rowId == kBakeUpscalerModel ? 6 : 2; }
RECT PresetChipRect(int rowId, int i, int y) {
    const int w = PresetChipW(rowId), gap = PresetChipGap(rowId);
    const int x = 52 + i * (w + gap);
    return RECT{x, y + 1, x + w, y + kRowH - 1};
}
int PresetChipAt(int rowId, int x) {
    const int count = PresetChipCount(rowId);
    if (count <= 0) return -1;
    const int w = PresetChipW(rowId), gap = PresetChipGap(rowId);
    for (int i = 0; i < count; ++i) {
        const int cx = 52 + i * (w + gap);
        if (x >= cx && x < cx + w) return i;
    }
    return -1;
}
void BustActiveFromValue(float v, int& bandIdx, int& cupIdx) {
    const int n = static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(kPresetCombos - 1)));
    bandIdx = n / kBustCupCount;
    cupIdx = n % kBustCupCount;
}
// The chip to light up for a row, or -1 when the underlying slider is outside the presets' range.
int ActivePresetChip(int rowId) {
    if (rowId == kBustBand || rowId == kBustCup) {
        SliderRef r;
        if (!SliderFor(kBodyBust, r) || !r.Valid()) return -1;
        if (r.Value() < -0.005f || r.Value() > 1.005f) return -1;   // dragged beyond the presets: no chip is "it"
        int b = 0, c = 0;
        BustActiveFromValue(r.Value(), b, c);
        return rowId == kBustBand ? b : c;
    }
    if (rowId == kNippleSize) {
        SliderRef r;
        if (!SliderFor(kNippleMm, r) || !r.Valid()) return -1;
        int best = 0;
        float bestD = 1e30f;
        for (int i = 0; i < 4; ++i) { const float d = std::fabs(r.Value() - kNippleSizeMm[i]); if (d < bestD) { bestD = d; best = i; } }
        return best;
    }
    if (rowId == kBakeQuality) return std::clamp(g.bakeQuality, 0, 2);
    if (rowId == kBakeUpscalerModel) return g.upscaleUseEsrgan ? 1 : 0;
    if (rowId == kJigglePreset) {
        switch (assemble::MatchJigglePreset(g.jiggle)) {
        case assemble::JigglePreset::Subtle: return 0;
        case assemble::JigglePreset::VerySubtle: return 1;
        case assemble::JigglePreset::VeryVerySubtle: return 2;
        case assemble::JigglePreset::Natural: return 3;
        case assemble::JigglePreset::Dynamic: return 4;
        default: return -1;   // Custom: no chip is "it"
        }
    }
    return -1;
}
const char* PresetChipText(int rowId, int i) {
    if (rowId == kBustBand && i >= 0 && i < kBustBandCount) return kBustBandLabels[i];
    if (rowId == kBustCup && i >= 0 && i < kBustCupCount) return kBustCupLabels[i];
    if (rowId == kNippleSize && i >= 0 && i < 4) return kNippleSizeLabels[i];
    if (rowId == kJigglePreset && i >= 0 && i < 5) return kJigglePresetLabels[i];
    if (rowId == kBakeQuality && i >= 0 && i < 3) return kBakeQualityLabels[i];
    if (rowId == kBakeUpscalerModel && i >= 0 && i < 2) return kUpscalerLabels[i];
    return "";
}
void ApplyPresetChip(int rowId, int chip) {
    if (rowId == kBakeUpscalerModel) {
        g.upscaleUseEsrgan = chip != 0;
        Log(std::string("upscaler: ") + kUpscalerLabels[g.upscaleUseEsrgan ? 1 : 0]);
        InvalidatePanel();
        return;
    }
    if (rowId == kBakeQuality) {
        chip = std::clamp(chip, 0, 2);
        if (chip == g.bakeQuality) return;
        g.bakeQuality = chip;
        BakeQualityChanged();
        return;
    }
    if (rowId == kBustBand || rowId == kBustCup) {
        SliderRef r;
        if (!SliderFor(kBodyBust, r) || !r.Valid()) return;
        int b = 0, c = 0;
        BustActiveFromValue(r.Value(), b, c);
        if (rowId == kBustBand) b = std::clamp(chip, 0, kBustBandCount - 1); else c = std::clamp(chip, 0, kBustCupCount - 1);
        r.Set(static_cast<float>(b * kBustCupCount + c) / static_cast<float>(kPresetCombos - 1));
        TweakEdited(kBodyBust, true);
        char msg[64];
        std::snprintf(msg, sizeof msg, "bust size: %s%s", kBustBandLabels[b], kBustCupLabels[c]);
        Log(msg);
        InvalidatePanel();
        return;
    }
    if (rowId == kNippleSize) {
        SliderRef r;
        if (!SliderFor(kNippleMm, r) || !r.Valid()) return;
        chip = std::clamp(chip, 0, 3);
        r.Set(std::clamp(kNippleSizeMm[chip], r.lo, r.hi));
        TweakEdited(kNippleMm, true);
        char msg[48];
        std::snprintf(msg, sizeof msg, "nipple size: %s (%+.0f mm)", kNippleSizeLabels[chip], static_cast<double>(kNippleSizeMm[chip]));
        Log(msg);
        InvalidatePanel();
        return;
    }
    if (rowId == kJigglePreset) {
        chip = std::clamp(chip, 0, 4);
        // The preset replaces the whole set; the switches (breast / butt) stay as they are.
        g.jiggle = assemble::JigglePresetSettings(static_cast<assemble::JigglePreset>(chip));
        char msg[64];
        std::snprintf(msg, sizeof msg, "jiggle: %s (mass %.2f kg, stiffness %.0f N/m, damping %.1f, strength %.2f)",
                      kJigglePresetLabels[chip], static_cast<double>(g.jiggle.mass), static_cast<double>(g.jiggle.stiffness),
                      static_cast<double>(g.jiggle.damping), static_cast<double>(g.jiggle.strength));
        Log(msg);
        JiggleSettingsChanged();
    }
}

