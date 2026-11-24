#include "assemble/Proportions.h"

#include "shape/Shape.h"
#include "card/Abmx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <nlohmann/json.hpp>

namespace cardbridge::assemble {

namespace {

std::vector<double> floatsOf(const Value& v) {
    std::vector<double> out;
    if (!v.isArray()) return out;
    out.reserve(v.size());
    for (const auto& e : v.asArray()) out.push_back(e.asFloat());
    return out;
}

// The half-way-back correction: move v half the distance toward 0.5. Never all the way.
float halfWayBack(float v) { return 0.5f + (v - 0.5f) * 0.5f; }

std::string fmt(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
    return buf;
}

// Bones the head chain occupies, for parent/child ABMX compounding notes and scope tagging.
const char* kHeadChainBones[] = {
    "cf_s_head", "cf_j_head", "cf_J_N_FaceRoot", "cf_J_FaceRoot", "cf_J_FaceBase",
    "cf_J_FaceUp_ty", "cf_J_FaceUp_tz", "cf_J_FaceLow_tz", "cf_J_FaceLow_sx",
    "cf_J_CheekUp2_L", "cf_J_CheekUp2_R", "cf_J_CheekLow_s_L", "cf_J_CheekLow_s_R",
    "cf_J_Chin_Base", "cf_J_ChinLow", "cf_J_ChinTip_Base", "cf_J_CheekUpBase",
    "cf_J_CheekUp_s_L", "cf_J_CheekUp_s_R", "cf_J_MouthBase_ty", "cf_J_MouthLow", "cf_J_Mouthup",
    "cf_J_MouthBase_rx", "cf_J_Nose_tip", "cf_J_Nose_rx", "cf_J_NoseBase_rx", "cf_J_NoseBridge_ty", "cf_J_NoseBridge_rx",
    "cf_j_neck", "cf_s_neck",
};

bool isHeadBone(const std::string& name) {
    for (const char* n : kHeadChainBones) if (name == n) return true;
    return false;
}

struct FindingBuilder {
    std::vector<ProportionFinding> findings;
    std::vector<CorrectionOp> suggestions;

    void add(ProportionFinding f, CorrectionOp* op) {
        f.opIndex = op ? static_cast<int>(suggestions.size()) : -1;
        findings.push_back(std::move(f));
        if (op) suggestions.push_back(std::move(*op));
    }
};

bool abmxBoneScaled(const card::AbmxMod* m) {
    if (!m) return false;
    return std::fabs(m->scale.x - 1.0f) >= 0.15f || std::fabs(m->scale.y - 1.0f) >= 0.15f || std::fabs(m->scale.z - 1.0f) >= 0.15f;
}

// One pass over the card: the slider rules (extreme / out-of-range / height / head size) and the ABMX rule.
FindingBuilder analyze(const card::Card& card) {
    FindingBuilder fb;

    const auto checkSliders = [&](const std::vector<double>& values, const std::vector<shape::SliderInfo>& sliders,
                                  int kind, const char* scope) {
        for (size_t i = 0; i < values.size() && i < sliders.size(); ++i) {
            const float v = static_cast<float>(values[i]);
            const shape::SliderInfo& si = sliders[i];
            std::string prop = std::string(si.group) + " · " + si.label;
            const bool outOfRange = v < 0.0f || v > 1.0f;
            const float dev = std::fabs(v - 0.5f);
            const bool extreme = dev >= 0.45f;
            const bool height = i == 0 && kind == kBodySlider && (v <= 0.30f || v >= 1.30f);
            const bool headSize = i == 1 && kind == kBodySlider && (v <= 0.40f || v >= 1.05f);
            if (!(outOfRange || extreme || height || headSize)) continue;

            const float corrected = halfWayBack(v);
            const std::string opScope = (i == 1 && kind == kBodySlider) ? std::string("head") : std::string(scope);
            CorrectionOp op;
            op.kind = kind;
            op.index = static_cast<int>(i);
            op.value = corrected;
            op.label = prop + " " + fmt(v) + " → " + fmt(corrected);
            op.why = height ? std::string(v < 0.5f ? "unusually low body height" : "unusually high body height")
                    : headSize ? std::string(v < 0.5f ? "unusually small head size" : "unusually large head size")
                    : outOfRange ? std::string("value outside the maker's 0..1 range (SliderUnlocker)")
                                 : std::string("extreme slider value");
            op.safety = (outOfRange || height || headSize) ? kSafetyApproximate : kSafetySafe;
            op.scope = opScope;

            ProportionFinding f;
            f.property = prop;
            f.current = fmt(v);
            f.reason = op.why;
            f.suggestion = std::string("set it to ") + fmt(corrected) + " (half-way toward neutral)";
            f.safety = op.safety;
            f.scope = opScope;
            fb.add(std::move(f), &op);
        }
    };

    const std::vector<double> body = floatsOf(card.body.at("shapeValueBody"));
    const std::vector<double> face = floatsOf(card.face.at("shapeValueFace"));
    checkSliders(body, shape::BodySliders(), kBodySlider, "body");
    checkSliders(face, shape::FaceSliders(), kFaceSlider, "face");

    // ABMX (KKABMPlugin.ABMData): per-bone scale multipliers the plugin applies on top of the shape.
    const card::AbmxTable abmx = card::LoadAbmx(card.extended);
    if (!abmx.empty()) {
        const int coordinate = std::clamp(static_cast<int>(card.status.at("coordinateType").asInt(0)), 0, 6);
        for (const auto& kv : abmx.bones) {
            const card::AbmxMod* m = abmx.find(kv.first, coordinate);
            if (!m) m = &kv.second.front();   // v2 tables (one entry) apply to every coordinate
            if (m->empty()) continue;
            const bool scaled = abmxBoneScaled(m);
            const bool length = std::fabs(m->length - 1.0f) > 1e-4f;
            const bool positional = m->hasPosition() || m->hasRotation();
            if (!scaled && !length && !positional) continue;
            const bool head = isHeadBone(kv.first);
            if (scaled) {
                CorrectionOp op;
                op.kind = kBoneScale;
                op.bone = kv.first;
                op.scaleMul = Vec3{1.0f + (1.0f - m->scale.x) * 0.5f, 1.0f + (1.0f - m->scale.y) * 0.5f, 1.0f + (1.0f - m->scale.z) * 0.5f};
                op.label = kv.first + " ABMX scale (" + fmt(m->scale.x) + ", " + fmt(m->scale.y) + ", " + fmt(m->scale.z) + ") → ×(" +
                           fmt(op.scaleMul.x) + ", " + fmt(op.scaleMul.y) + ", " + fmt(op.scaleMul.z) + ")";
                op.why = "ABMX bone scale deviates from 1.0 (Advanced Bone Modifier)";
                op.safety = kSafetyApproximate;
                op.scope = head ? "head" : "body";
                ProportionFinding f;
                f.property = kv.first + " (ABMX scale)";
                f.current = "(" + fmt(m->scale.x) + ", " + fmt(m->scale.y) + ", " + fmt(m->scale.z) + ")";
                f.reason = op.why;
                f.suggestion = "multiply the bone's scale by (" + fmt(op.scaleMul.x) + ", " + fmt(op.scaleMul.y) + ", " + fmt(op.scaleMul.z) + ")";
                f.safety = kSafetyApproximate;
                f.scope = head ? "head" : "body";
                fb.add(std::move(f), &op);
            } else {
                // length / position / rotation: informational only (an op cannot undo these additively).
                ProportionFinding f;
                f.property = kv.first + (length ? " (ABMX length)" : positional ? " (ABMX position/rotation)" : "");
                f.current = length ? fmt(m->length)
                                    : (m->hasRotation() ? "rot(" + fmt(m->rotation.x) + ", " + fmt(m->rotation.y) + ", " + fmt(m->rotation.z) + ")"
                                                        : "pos(" + fmt(m->position.x) + ", " + fmt(m->position.y) + ", " + fmt(m->position.z) + ")");
                f.reason = "ABMX modifier the shape system cannot absorb; review it manually";
                f.suggestion.clear();
                f.safety = kSafetyManual;
                f.scope = head ? "head" : "body";
                fb.add(std::move(f), nullptr);
            }
        }
        // Parent/child compounding note: if two bones in one parent chain are both scaled, the effects compound. Informational.
        std::set<std::string> scaled;
        for (const auto& kv : abmx.bones) {
            const card::AbmxMod* m = abmx.find(kv.first, coordinate);
            if (!m) m = kv.second.empty() ? nullptr : &kv.second.front();
            if (m && abmxBoneScaled(m)) scaled.insert(kv.first);
        }
        static const char* chains[][3] = {
            {"cf_n_height", "cf_j_spine01", "cf_j_spine03"},
            {"cf_s_head", "cf_J_FaceRoot", "cf_J_FaceBase"},
            {"cf_j_neck", "cf_s_neck", "cf_s_head"},
        };
        for (const auto& chain : chains) {
            int inChain = 0;
            std::string names;
            for (const char* n : chain) {
                if (scaled.count(n)) { ++inChain; if (!names.empty()) names += " + "; names += n; }
            }
            if (inChain >= 2) {
                ProportionFinding f;
                f.property = names;
                f.current = "both scaled";
                f.reason = "parent and child bones are both ABMX-scaled; the effects multiply down the chain";
                f.suggestion.clear();
                f.safety = kSafetyManual;
                f.scope = isHeadBone(chain[0]) || std::string(chain[0]) == "cf_s_head" || std::string(chain[0]) == "cf_j_neck" ? "head" : "body";
                fb.add(std::move(f), nullptr);
            }
        }
    }
    return fb;
}

} // namespace

std::vector<ProportionFinding> AnalyzeCard(const card::Card& card) { return analyze(card).findings; }

std::vector<CorrectionOp> SuggestCorrections(const card::Card& card, const std::string& onlyScope) {
    FindingBuilder fb = analyze(card);
    if (onlyScope.empty()) return fb.suggestions;
    std::vector<CorrectionOp> out;
    for (const auto& op : fb.suggestions)
        if (op.scope == onlyScope) out.push_back(op);
    return out;
}

std::vector<CorrectionOp> PresetNormalizeHead(const card::Card& card) { return SuggestCorrections(card, "head"); }
std::vector<CorrectionOp> PresetNormalizeBody(const card::Card& card) { return SuggestCorrections(card, "body"); }
std::vector<CorrectionOp> PresetNormalizeFace(const card::Card& card) { return SuggestCorrections(card, "face"); }
std::vector<CorrectionOp> PresetNormalizeOverall(const card::Card& card) { return SuggestCorrections(card); }

float SliderValueCorrected(float baseValue, const ProportionLayer& layer, int kind, int index) {
    if (const CorrectionOp* op = layer.forSlider(kind, index)) return op->value;
    return baseValue;
}

Vec3 BoneScaleCorrected(Vec3 baseScale, const ProportionLayer& layer, const std::string& bone, bool* changed) {
    Vec3 r = baseScale;
    bool any = false;
    if (layer.enabled) {
        // A transfer op owns the bone: a normalize op on the same bone is not applied on top of it.
        const CorrectionOp* transfer = nullptr;
        for (const CorrectionOp& o : layer.ops)
            if (o.kind == kBoneScale && o.bone == bone && o.origin == kOriginTransfer) transfer = &o;
        if (transfer) {
            r.x *= transfer->scaleMul.x; r.y *= transfer->scaleMul.y; r.z *= transfer->scaleMul.z;
            any = true;
        } else {
            for (const CorrectionOp& o : layer.ops)
                if (o.kind == kBoneScale && o.bone == bone) {
                    r.x *= o.scaleMul.x;
                    r.y *= o.scaleMul.y;
                    r.z *= o.scaleMul.z;
                    any = true;
                }
        }
    }
    if (changed) *changed = any;
    return r;
}

void BoneTransformCorrected(float& length, Vec3& position, const ProportionLayer& layer, const std::string& bone) {
    length = 1.0f;
    position = {0, 0, 0};
    if (!layer.enabled) return;
    for (const CorrectionOp& o : layer.ops) {
        if (o.kind != kBoneScale || o.bone != bone || o.origin != kOriginTransfer) continue;
        if (o.hasLength) length *= o.length;
        if (o.hasPosition) position = position + o.position;
    }
}

bool ProportionLayerFromJson(const std::string& text, ProportionLayer& out, std::string* error) {
    ProportionLayer parsed;
    try {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object() || !j.contains("ops") || !j["ops"].is_array()) {
            if (error) *error = "expected {\"enabled\": bool, \"ops\": [...]}";
            return false;
        }
        parsed.enabled = j.value("enabled", true);
        for (const auto& e : j["ops"]) {
            if (!e.is_object()) { if (error) *error = "each op must be an object"; return false; }
            CorrectionOp op;
            op.kind = e.value("kind", -1);
            if (op.kind < kBodySlider || op.kind > kBoneScale) { if (error) *error = "op.kind must be 0 (body slider), 1 (face slider) or 2 (bone scale)"; return false; }
            if (op.kind == kBoneScale) {
                op.bone = e.value("bone", std::string());
                if (op.bone.empty()) { if (error) *error = "a bone-scale op needs a bone name"; return false; }
                if (e.contains("scale") && e["scale"].is_array() && e["scale"].size() == 3)
                    op.scaleMul = {e["scale"][0].get<float>(), e["scale"][1].get<float>(), e["scale"][2].get<float>()};
                if (std::fabs(op.scaleMul.x) < 1e-6f || std::fabs(op.scaleMul.y) < 1e-6f || std::fabs(op.scaleMul.z) < 1e-6f) {
                    if (error) *error = "a bone-scale op cannot have a zero axis";
                    return false;
                }
            } else {
                op.index = e.value("index", -1);
                const int maxIndex = op.kind == kBodySlider ? 43 : 51;
                if (op.index < 0 || op.index > maxIndex) { if (error) *error = "slider index out of range"; return false; }
                op.value = e.value("value", 0.5f);
                if (!std::isfinite(op.value) || op.value < -4.0f || op.value > 5.0f) { if (error) *error = "slider value out of sane range"; return false; }
            }
            if (op.kind == kBoneScale) {
                op.origin = std::clamp(e.value("origin", kOriginNormalize), kOriginNormalize, kOriginTransfer);
                op.hasLength = e.value("hasLength", false);
                if (op.hasLength) {
                    op.length = e.value("length", 1.0f);
                    if (!std::isfinite(op.length) || op.length < 0.01f || op.length > 100.0f) {
                        if (error) *error = "bone length out of sane range";
                        return false;
                    }
                }
                op.hasPosition = e.value("hasPosition", false);
                if (op.hasPosition && e.contains("position") && e["position"].is_array() && e["position"].size() == 3) {
                    op.position = {e["position"][0].get<float>(), e["position"][1].get<float>(), e["position"][2].get<float>()};
                }
                if (op.hasPosition && (std::fabs(op.position.x) > 100.0f || std::fabs(op.position.y) > 100.0f || std::fabs(op.position.z) > 100.0f)) {
                    if (error) *error = "bone position out of sane range";
                    return false;
                }
            }
            op.label = e.value("label", std::string());
            op.why = e.value("why", std::string());
            op.safety = std::clamp(e.value("safety", kSafetyApproximate), kSafetySafe, kSafetyManual);
            op.scope = e.value("scope", std::string());
            parsed.ops.push_back(std::move(op));
        }
    } catch (const std::exception& e) {
        if (error) *error = std::string("invalid JSON: ") + e.what();
        return false;
    }
    out = std::move(parsed);
    if (error) error->clear();
    return true;
}

std::string ProportionLayerToJson(const ProportionLayer& layer) {
    nlohmann::json j;
    j["enabled"] = layer.enabled;
    j["ops"] = nlohmann::json::array();
    for (const CorrectionOp& o : layer.ops) {
        nlohmann::json e;
        e["kind"] = o.kind;
        e["index"] = o.index;
        e["bone"] = o.bone;
        e["scale"] = {o.scaleMul.x, o.scaleMul.y, o.scaleMul.z};
        e["value"] = o.value;
        e["origin"] = o.origin;
        e["hasLength"] = o.hasLength;
        e["length"] = o.length;
        e["hasPosition"] = o.hasPosition;
        e["position"] = {o.position.x, o.position.y, o.position.z};
        e["label"] = o.label;
        e["why"] = o.why;
        e["safety"] = o.safety;
        e["scope"] = o.scope;
        j["ops"].push_back(std::move(e));
    }
    return j.dump(2);
}

} // namespace cardbridge::assemble
