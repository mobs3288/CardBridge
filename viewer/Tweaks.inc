// viewer fragment: slider access (included in order by main.cpp)

// ---- tweak access ----------------------------------------------------------------------------------------------------
struct SliderRef {
    float* v = nullptr;
    float def = 0, lo = 0, hi = 1, step = 0.01f;
    bool percent = true;   // shown as 0..100 %, otherwise as plain millimetres
    const char* fmt = nullptr;   // a printf format for the value (one double), instead of the two above
};

bool SliderFor(int id, SliderRef& r) {
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
    if (id == kNippleR) { r.v = &g.tw.nippleColour.x; r.def = assemble::kDefaultNippleColour.x; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kNippleG) { r.v = &g.tw.nippleColour.y; r.def = assemble::kDefaultNippleColour.y; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kNippleB) { r.v = &g.tw.nippleColour.z; r.def = assemble::kDefaultNippleColour.z; r.lo = 0; r.hi = 1; r.step = 0.01f; return true; }
    if (id == kAPoseDeg) { r.v = &g.aPoseDeg; r.def = 40.0f; r.lo = 15; r.hi = 70; r.step = 1; r.percent = false; r.fmt = "%.0f"; return true; }
    if (id == kHeightCm) { r.v = &g.heightCm; r.def = g.baseHeightCm; r.lo = 100; r.hi = 250; r.step = 1; r.percent = false; r.fmt = "%.0f cm"; return true; }
    if (id == kSag) { r.v = &g.sag; r.def = g.sagDefault; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
    if (id == kJiggleIntensity) { r.v = &g.jiggleIntensity; r.def = 0.5f; r.lo = 0; r.hi = 2; r.step = 0.05f; r.percent = false; r.fmt = "%.2f"; return true; }
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

// Pose controls change the picture at once, no re-assembly.
bool IsLiveId(int id) { return id == kAPose || id == kAPoseDeg || id == kHeightCm || id == kSag || (id >= kBoneEdit && id < kBoneEdit + kBoneEditCount) || (id >= kMatBase && id < kMatBase + kMatCount); }
void TweakEdited(int id, bool released);   // defined in Pose.inc

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

int PresetChipCount(int rowId) { return rowId == kNippleSize ? 4 : rowId == kBustCup ? kBustCupCount : rowId == kBustBand ? kBustBandCount : 0; }
int PresetChipW(int rowId) { return rowId == kNippleSize ? 52 : rowId == kBustCup ? 37 : 32; }
int PresetChipGap(int rowId) { return rowId == kNippleSize ? 6 : 2; }
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
        if (!SliderFor(kBodyBust, r) || !r.v) return -1;
        if (*r.v < -0.005f || *r.v > 1.005f) return -1;   // dragged beyond the presets: no chip is "it"
        int b = 0, c = 0;
        BustActiveFromValue(*r.v, b, c);
        return rowId == kBustBand ? b : c;
    }
    if (rowId == kNippleSize) {
        SliderRef r;
        if (!SliderFor(kNippleMm, r) || !r.v) return -1;
        int best = 0;
        float bestD = 1e30f;
        for (int i = 0; i < 4; ++i) { const float d = std::fabs(*r.v - kNippleSizeMm[i]); if (d < bestD) { bestD = d; best = i; } }
        return best;
    }
    return -1;
}
const char* PresetChipText(int rowId, int i) {
    if (rowId == kBustBand && i >= 0 && i < kBustBandCount) return kBustBandLabels[i];
    if (rowId == kBustCup && i >= 0 && i < kBustCupCount) return kBustCupLabels[i];
    if (rowId == kNippleSize && i >= 0 && i < 4) return kNippleSizeLabels[i];
    return "";
}
void ApplyPresetChip(int rowId, int chip) {
    if (rowId == kBustBand || rowId == kBustCup) {
        SliderRef r;
        if (!SliderFor(kBodyBust, r) || !r.v) return;
        int b = 0, c = 0;
        BustActiveFromValue(*r.v, b, c);
        if (rowId == kBustBand) b = std::clamp(chip, 0, kBustBandCount - 1); else c = std::clamp(chip, 0, kBustCupCount - 1);
        *r.v = static_cast<float>(b * kBustCupCount + c) / static_cast<float>(kPresetCombos - 1);
        TweakEdited(kBodyBust, true);
        char msg[64];
        std::snprintf(msg, sizeof msg, "bust size: %s%s", kBustBandLabels[b], kBustCupLabels[c]);
        Log(msg);
        InvalidatePanel();
        return;
    }
    if (rowId == kNippleSize) {
        SliderRef r;
        if (!SliderFor(kNippleMm, r) || !r.v) return;
        chip = std::clamp(chip, 0, 3);
        *r.v = std::clamp(kNippleSizeMm[chip], r.lo, r.hi);
        TweakEdited(kNippleMm, true);
        char msg[48];
        std::snprintf(msg, sizeof msg, "nipple size: %s (%+.0f mm)", kNippleSizeLabels[chip], static_cast<double>(kNippleSizeMm[chip]));
        Log(msg);
        InvalidatePanel();
    }
}

