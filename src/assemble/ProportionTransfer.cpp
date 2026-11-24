#include "assemble/ProportionTransfer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <type_traits>
#include <nlohmann/json.hpp>

namespace cardbridge::assemble::transfer {
namespace {

constexpr int kBodySliderCount = 44;
constexpr int kFaceSliderCount = 52;

std::vector<float> FloatsOf(const Value& v) {
    std::vector<float> out;
    if (!v.isArray()) return out;
    out.reserve(v.size());
    for (const auto& e : v.asArray()) out.push_back(e.asFloat());
    return out;
}

struct Control {
    std::vector<int> sliders;
    std::vector<std::string> bones;
};

Control controlFor(int id) {
    using namespace cardbridge::measure;
    Control c;
    switch (id) {
    case kTransferHeight:       c.sliders = {0}; break;
    case kTransferHeadSize:     c.sliders = {1}; break;
    case kTransferHeadBodyRatio:c.sliders = {1}; break;
    case kTransferBust:         c.sliders = {4}; break;
    case kTransferWaist:        c.sliders = {22, 23}; break;
    case kTransferHip:          c.sliders = {24, 25}; break;
    case kTransferShoulderWidth:c.sliders = {37, 14, 16, 38}; break;
    case kTransferNeck:         c.sliders = {2, 3}; break;
    case kTransferThigh:        c.sliders = {28, 29}; break;
    case kTransferCalf:         c.sliders = {32, 33}; break;
    case kTransferArmLength:    c.bones = ArmChain(true); { auto r = ArmChain(false); c.bones.insert(c.bones.end(), r.begin(), r.end()); } break;
    case kTransferUpperArm:     c.bones = UpperArmChain(true); { auto r = UpperArmChain(false); c.bones.insert(c.bones.end(), r.begin(), r.end()); } break;
    case kTransferLowerArm:     c.bones = LowerArmChain(true); { auto r = LowerArmChain(false); c.bones.insert(c.bones.end(), r.begin(), r.end()); } break;
    case kTransferLegLength:    c.bones = LegChain(true); { auto r = LegChain(false); c.bones.insert(c.bones.end(), r.begin(), r.end()); } break;
    default: break;
    }
    return c;
}

CorrectionOp sliderOp(int index, float value, const char* measureName) {
    CorrectionOp op;
    op.kind = kBodySlider;
    op.index = index;
    op.value = value;
    op.origin = kOriginTransfer;
    op.safety = kSafetyApproximate;
    op.scope = "body";
    op.label = std::string(measureName) + " → slider " + std::to_string(index) + " = " + std::to_string(value);
    op.why = "transferred from the reference card (absolute value, never accumulated)";
    return op;
}

CorrectionOp lengthOp(const std::string& bone, float ratio, const char* measureName) {
    CorrectionOp op;
    op.kind = kBoneScale;
    op.bone = bone;
    op.origin = kOriginTransfer;
    op.scaleMul = {1, 1, 1};
    op.hasLength = true;
    // A bone transform moves the whole joint subtree and drags clothes with it. Keep it conservative:
    // anything beyond +/-25% needs a real re-fit of the garment, which this tool does not do.
    op.length = std::clamp(ratio, 0.8f, 1.25f);
    op.safety = kSafetyApproximate;
    op.scope = "body";
    op.label = std::string(measureName) + " → " + bone + " length ×" + std::to_string(op.length);
    op.why = "segment length ratio of the reference / target bones";
    return op;
}

float measureOf(const measure::TransferSet& set, int id) {
    return id >= 0 && id < measure::kTransferCount && set[static_cast<size_t>(id)].valid ? set[static_cast<size_t>(id)].value : 0.0f;
}

} // namespace

std::vector<float> BodySlidersOf(const card::Card& card) {
    std::vector<float> v = FloatsOf(card.body.at("shapeValueBody"));
    if (v.size() < static_cast<size_t>(kBodySliderCount)) v.resize(kBodySliderCount, 0.5f);
    return v;
}

std::vector<float> FaceSlidersOf(const card::Card& card) {
    std::vector<float> v = FloatsOf(card.face.at("shapeValueFace"));
    if (v.size() < static_cast<size_t>(kFaceSliderCount)) v.resize(kFaceSliderCount, 0.5f);
    return v;
}

Spec Build(const assemble::Scene& referenceScene, const card::Card& referenceCard, const measure::TransferSet& reference,
           const assemble::Scene& targetScene, const card::Card& targetCard, const measure::TransferSet& target,
           const std::vector<float>& workingBody, const Spec& previous) {
    Spec s;
    s.referenceName = referenceCard.displayName();
    s.referenceSha256 = referenceCard.sha256;
    s.targetSha256 = targetCard.sha256;

    for (int i = 0; i < measure::kTransferCount; ++i) {
        s.selected[i] = previous.selected[i];
        s.refine[i] = previous.refine[i];
        s.targetValue[i] = reference[static_cast<size_t>(i)].valid ? reference[static_cast<size_t>(i)].value : 0.0f;
        s.achievedValue[i] = target[static_cast<size_t>(i)].value;
        s.achievedValid[i] = target[static_cast<size_t>(i)].valid;
    }

    const std::vector<float> refBody = BodySlidersOf(referenceCard);
    std::map<std::pair<int, int>, CorrectionOp> sliderOps;   // (kind, index) -> op
    std::map<std::string, CorrectionOp> boneOps;

    for (int i = 0; i < measure::kTransferCount; ++i) {
        if (!s.selected[i] || !reference[static_cast<size_t>(i)].valid) continue;
        const char* name = measure::TransferName(i);
        const Control c = controlFor(i);
        if (i == measure::kTransferHeadBodyRatio && s.selected[measure::kTransferHeadSize]) {
            s.notes.push_back("Head-to-body ratio is a derived result of height + head size; its achieved value is reported.");
            continue;
        }
        for (int slider : c.sliders) {
            if (slider < 0 || static_cast<size_t>(slider) >= refBody.size()) continue;
            sliderOps[{kBodySlider, slider}] = sliderOp(slider, refBody[static_cast<size_t>(slider)], name);
        }
        for (const std::string& bone : c.bones) {
            if (boneOps.count(bone)) continue;   // first owning measurement wins (arm length before upper/lower)
            const float rl = measure::BoneLocalLength(referenceScene, bone);
            const float tl = measure::BoneLocalLength(targetScene, bone);
            if (rl <= 1e-6f || tl <= 1e-6f) continue;
            boneOps[bone] = lengthOp(bone, rl / tl, name);
        }
    }
    for (auto& kv : sliderOps) s.ops.push_back(kv.second);
    for (auto& kv : boneOps) s.ops.push_back(kv.second);
    if (s.ops.empty()) s.notes.push_back("No transferable measurement was selected, or the reference could not be measured.");
    (void)workingBody;
    return s;
}

void Refine(Spec& spec, std::vector<float>& workingBody, const std::vector<float>& cardBody, const MeasureFn& measureAt) {
    if (!measureAt) return;
    for (int i = 0; i < measure::kTransferCount; ++i) {
        if (!spec.selected[i] || !spec.refine[i]) continue;
        const Control c = controlFor(i);
        const float target = spec.targetValue[i];
        if (!(target > 0.0f)) continue;
        const float tolerance = measure::TransferIsRatio(i) ? 0.01f : 0.5f;   // 1% of a ratio / half a cm

        std::vector<size_t> sliders;
        for (int s : c.sliders)
            if (s >= 0 && static_cast<size_t>(s) < workingBody.size()) sliders.push_back(static_cast<size_t>(s));
        std::vector<CorrectionOp*> lengthOps;
        for (const std::string& bone : c.bones)
            for (auto& op : spec.ops)
                if (op.kind == kBoneScale && op.bone == bone && op.origin == kOriginTransfer && op.hasLength) {
                    lengthOps.push_back(&op);
                    break;
                }
        if (sliders.empty() && lengthOps.empty()) continue;

        std::vector<float> base;
        float lo = -1e9f, hi = 1e9f;   // the coupled sliders' common delta interval is centred on 0, not on +1
        if (!sliders.empty()) {
            for (size_t idx : sliders) {
                base.push_back(workingBody[idx]);
                const float card = idx < cardBody.size() ? cardBody[idx] : 0.5f;
                // Stay inside the maker's 0..1 range for normal cards. SliderUnlocker values outside it
                // are kept as their own bounds, but a refinement must never push a slider into them.
                const float floor = card >= 0.0f && card <= 1.0f ? 0.0f : std::min(-1.0f, card);
                const float ceil  = card >= 0.0f && card <= 1.0f ? 1.0f : std::max(2.0f, card);
                lo = std::max(lo, floor - workingBody[idx]);
                hi = std::min(hi, ceil - workingBody[idx]);
            }
        } else {
            for (CorrectionOp* op : lengthOps) base.push_back(op->length);
            lo = 0.8f; hi = 1.25f;
        }
        if (!(lo < hi)) continue;

        auto apply = [&](float d) {
            if (!sliders.empty()) {
                for (size_t k = 0; k < sliders.size(); ++k) {
                    const float card = sliders[k] < cardBody.size() ? cardBody[sliders[k]] : 0.5f;
                    const float floor = card >= 0.0f && card <= 1.0f ? 0.0f : std::min(-1.0f, card);
                    const float ceil  = card >= 0.0f && card <= 1.0f ? 1.0f : std::max(2.0f, card);
                    workingBody[sliders[k]] = std::clamp(base[k] + d, floor, ceil);
                }
            } else {
                for (size_t k = 0; k < lengthOps.size(); ++k)
                    lengthOps[k]->length = std::clamp(base[k] * d, 0.8f, 1.25f);
            }
        };
        auto valueAt = [&](float d) {
            apply(d);
            const measure::TransferSet set = measureAt(workingBody);
            return measureOf(set, i);
        };
        auto errorOf = [&](float v) { return v > 0.0f ? std::fabs(v - target) : std::numeric_limits<float>::max(); };

        float bestD = 0.0f, bestErr = errorOf(valueAt(0.0f));
        const float vLo = valueAt(lo), vHi = valueAt(hi);
        if (errorOf(vLo) < bestErr) { bestErr = errorOf(vLo); bestD = lo; }
        if (errorOf(vHi) < bestErr) { bestErr = errorOf(vHi); bestD = hi; }
        // An invalid measurement is reported as 0; with a positive target the sign test still rejects it,
        // so a legitimate 0 at a range endpoint must not be mistaken for "unavailable".
        if (std::isfinite(vLo) && std::isfinite(vHi) && (vLo - target) * (vHi - target) <= 0.0f) {
            float a = lo, b = hi, va = vLo;
            for (int it = 0; it < 12; ++it) {
                const float mid = 0.5f * (a + b);
                const float vm = valueAt(mid);
                if (errorOf(vm) < bestErr) { bestErr = errorOf(vm); bestD = mid; }
                if (std::isfinite(vm) && (va - target) * (vm - target) <= 0.0f) b = mid;
                else { a = mid; va = vm; }
            }
        }
        apply(bestD);
        const measure::TransferSet finalSet = measureAt(workingBody);
        spec.achievedValue[i] = measureOf(finalSet, i);
        spec.achievedValid[i] = spec.achievedValue[i] > 0.0f;
        if (bestErr > tolerance) {
            char buf[192];
            std::snprintf(buf, sizeof buf, "%s: cannot reach %.2f%s on this card; nearest %.2f%s (residual %.2f%s)",
                          measure::TransferName(i), target, measure::TransferUnit(i), spec.achievedValue[i],
                          measure::TransferUnit(i), bestErr, measure::TransferUnit(i));
            spec.notes.push_back(buf);
        }
        // Keep the layer ops in sync with the refined slider values so a CLI export from the spec alone matches.
        for (size_t k = 0; k < sliders.size(); ++k)
            for (auto& op : spec.ops)
                if (op.kind == kBodySlider && static_cast<size_t>(op.index) == sliders[k]) op.value = workingBody[sliders[k]];
    }
}

ProportionLayer Merge(const ProportionLayer& normalize, const std::vector<CorrectionOp>& transferOps, bool enabled) {
    ProportionLayer out;
    out.enabled = enabled;
    for (const CorrectionOp& o : normalize.ops) {
        bool conflict = false;
        for (const CorrectionOp& t : transferOps) {
            if (o.kind != t.kind) continue;
            if (o.kind == kBoneScale ? o.bone == t.bone : o.index == t.index) { conflict = true; break; }
        }
        if (!conflict) out.ops.push_back(o);
    }
    out.ops.insert(out.ops.end(), transferOps.begin(), transferOps.end());
    return out;
}

std::string ToJson(const Spec& spec) {
    nlohmann::json j;
    j["reference"] = spec.referenceName;
    j["referenceSha256"] = spec.referenceSha256;
    j["targetSha256"] = spec.targetSha256;
    j["selected"] = nlohmann::json::array();
    j["refine"] = nlohmann::json::array();
    j["target"] = nlohmann::json::array();
    j["achieved"] = nlohmann::json::array();
    for (int i = 0; i < measure::kTransferCount; ++i) {
        j["selected"].push_back(spec.selected[i]);
        j["refine"].push_back(spec.refine[i]);
        j["target"].push_back(spec.targetValue[i]);
        j["achieved"].push_back(spec.achievedValue[i]);
    }
    j["notes"] = spec.notes;
    ProportionLayer layer;
    layer.ops = spec.ops;
    j["layer"] = nlohmann::json::parse(ProportionLayerToJson(layer));
    return j.dump(2);
}

bool FromJson(const std::string& text, Spec& out, std::string* error) {
    Spec s;
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || !j.contains("layer")) {
            if (error) *error = "expected a transfer spec with a \"layer\" object";
            return false;
        }
        s.referenceName = j.value("reference", std::string());
        s.referenceSha256 = j.value("referenceSha256", std::string());
        s.targetSha256 = j.value("targetSha256", std::string());
        s.notes = j.value("notes", std::vector<std::string>{});
        const auto readArray = [&](const char* key, auto& dest) {
            if (!j.contains(key) || !j[key].is_array()) return;
            size_t n = std::min<size_t>(j[key].size(), dest.size());
            for (size_t i = 0; i < n; ++i) dest[i] = j[key][i].get<typename std::decay<decltype(dest[i])>::type>();
        };
        readArray("selected", s.selected);
        readArray("refine", s.refine);
        readArray("target", s.targetValue);
        readArray("achieved", s.achievedValue);
        for (int i = 0; i < measure::kTransferCount; ++i) s.achievedValid[i] = s.achievedValue[i] > 0.0f;
        ProportionLayer layer;
        std::string err;
        if (!ProportionLayerFromJson(j["layer"].dump(), layer, &err)) {
            if (error) *error = err;
            return false;
        }
        s.ops = layer.ops;
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid transfer spec: ") + e.what();
        return false;
    }
    out = std::move(s);
    if (error) error->clear();
    return true;
}

} // namespace cardbridge::assemble::transfer
