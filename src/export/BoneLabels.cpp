#include "export/BoneLabels.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace cardbridge::exporter {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The name without a part prefix ("outfit0.acc03/") and without the rig prefix (cf_j_, cf_d_, cf_s_, cf_n_, a_n_ ...).
std::string core(const std::string& name, std::string* prefix = nullptr) {
    std::string s = name;
    if (size_t slash = s.find('/'); slash != std::string::npos) s = s.substr(slash + 1);
    const std::string l = lower(s);
    for (const char* p : {"cf_j_", "cf_d_", "cf_s_", "cf_n_", "cf_hit_", "cf_a_", "a_n_", "ca_", "cm_"}) {
        const std::string ps = p;
        if (l.compare(0, ps.size(), ps) == 0) {
            if (prefix) *prefix = ps;
            return s.substr(ps.size());
        }
    }
    if (prefix) prefix->clear();
    return s;
}

// "FaceLow_tz" -> "face", "low", "tz"; "Eye01" -> "eye", "01". A token is a run of letters or of digits; an upper case letter after a lower
// case one starts a new token.
std::vector<std::string> tokens(const std::string& c) {
    std::vector<std::string> out;
    std::string cur;
    int kind = 0;   // 1 letters, 2 digits
    for (char ch : c) {
        const int k = std::isalpha(static_cast<unsigned char>(ch)) ? 1 : std::isdigit(static_cast<unsigned char>(ch)) ? 2 : 0;
        const bool camel = k == 1 && !cur.empty() && std::isupper(static_cast<unsigned char>(ch)) && std::islower(static_cast<unsigned char>(cur.back()));
        if (k == 0 || (kind && k != kind) || camel) { if (!cur.empty()) out.push_back(lower(cur)); cur.clear(); }
        if (k) cur.push_back(ch);
        kind = k;
    }
    if (!cur.empty()) out.push_back(lower(cur));
    return out;
}

const std::map<std::string, std::string>& words() {
    static const std::map<std::string, std::string> w = {
        {"root", "Root"}, {"height", "Height"}, {"hips", "Hips"}, {"waist", "Waist"}, {"spine", "Spine"}, {"neck", "Neck"}, {"head", "Head"},
        {"shoulder", "Clavicle"}, {"arm", "Arm"}, {"forearm", "Forearm"}, {"hand", "Hand"}, {"wrist", "Wrist"}, {"elbow", "Elbow"},
        {"thumb", "Thumb"}, {"index", "Index"}, {"middle", "Middle"}, {"ring", "Ring"}, {"little", "Little"},
        {"thigh", "Thigh"}, {"leg", "Leg"}, {"knee", "Knee"}, {"foot", "Foot"}, {"toes", "Toes"}, {"ankle", "Ankle"},
        {"siri", "Butt"}, {"kosi", "Hip"}, {"bust", "Breast"}, {"mune", "Chest"}, {"nip", "Nipple"}, {"bnip", "Nipple"}, {"sk", "Skirt"},
        {"eye", "Eye"}, {"hitomi", "Pupil"}, {"mayu", "Brow"}, {"mayumoto", "Brow base"}, {"mouth", "Mouth"}, {"tang", "Tongue"}, {"tooth", "Tooth"},
        {"ear", "Ear"}, {"nose", "Nose"}, {"cheek", "Cheek"}, {"chin", "Chin"}, {"face", "Face"}, {"hair", "Hair"}, {"megane", "Glasses"},
        {"up", "Upper"}, {"low", "Lower"}, {"mid", "Middle"}, {"tip", "Tip"}, {"base", "Base"}, {"bridge", "Bridge"}, {"cavity", "Cavity"},
        {"move", "Move"}, {"ring", "Ring"}, {"joint", "Joint"}, {"acc", "Accessory"}, {"dam", "Damp"}};
    return w;
}

bool hasWord(const std::vector<std::string>& toks, const char* w) { return std::find(toks.begin(), toks.end(), w) != toks.end(); }
bool hasAny(const std::vector<std::string>& toks, std::initializer_list<const char*> ws) { for (const char* w : ws) if (hasWord(toks, w)) return true; return false; }

// The group a name alone points to ("" = no idea).
std::string groupOfName(const std::string& name) {
    const auto t = tokens(core(name));
    if (hasAny(t, {"eye", "hitomi", "eyeline", "eyelid"})) return "Face: Eyes";
    if (hasAny(t, {"mayu", "mayumoto", "mayuge"})) return "Face: Brows";
    if (hasAny(t, {"mouth", "mouthup", "tang", "tooth", "lip", "chin"})) return "Face: Mouth and chin";
    if (hasAny(t, {"cheek"})) return "Face: Cheeks";
    if (hasAny(t, {"ear"})) return "Face: Ears";
    if (hasAny(t, {"nose"})) return "Face: Nose";
    if (hasAny(t, {"face", "megane"})) return "Face";
    if (hasAny(t, {"thumb", "index", "middle", "ring", "little", "hand"})) return "Hands";
    if (hasAny(t, {"shoulder", "arm", "forearm", "elbow", "wrist"})) return "Arms";
    if (hasAny(t, {"thigh", "leg", "knee", "foot", "toes", "ankle"})) return "Legs";
    if (hasAny(t, {"bust", "mune", "nip", "bnip", "bnipacc"})) return "Chest";
    if (hasAny(t, {"sk"})) return "Skirt";
    if (hasAny(t, {"head", "neck"})) return "Head and neck";
    if (hasAny(t, {"siri", "kosi", "waist", "spine", "hips", "root", "height"})) return "Torso";
    return "";
}

} // namespace

bool IsHelperBone(const std::string& name) {
    std::string prefix;
    core(name, &prefix);
    return prefix == "cf_d_" || prefix == "cf_s_" || prefix == "cf_n_" || prefix == "cf_hit_" || prefix == "cf_a_" || prefix == "a_n_";
}

std::string BoneLabelEn(const std::string& name) {
    std::string prefix;
    const std::string c = core(name, &prefix);
    std::string label;
    std::string side;
    auto toks = tokens(c);
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::string& t = toks[i];
        if (t == "l" && i + 1 == toks.size()) { side = "Left"; continue; }
        if (t == "r" && i + 1 == toks.size()) { side = "Right"; continue; }
        if ((t.size() == 2 || t == "txdam") && (t[0] == 't' || t[0] == 'r') && (t[1] == 'x' || t[1] == 'y' || t[1] == 'z')) continue;   // axis suffix of a moving / turning node
        if (t == "s" || t == "j") continue;
        auto it = words().find(t);
        std::string w = it != words().end() ? it->second : (std::isdigit(static_cast<unsigned char>(t[0])) ? t : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])))) + t.substr(1));
        label += (label.empty() ? "" : " ") + w;
    }
    if (label.empty()) label = name;
    if (!side.empty()) label = side + " " + label;
    if (prefix == "cf_d_") label += " (driven)";
    else if (prefix == "cf_s_") label += " (shape)";
    else if (prefix == "cf_n_" || prefix == "a_n_") label += " (attach)";
    if (size_t slash = name.find('/'); slash != std::string::npos) label = name.substr(0, slash) + ": " + label;
    return label;
}

std::string BoneGroup(const assemble::Scene& scene, size_t bone) {
    const auto& b = scene.bones[bone];
    if (b.source != "rig") {
        if (b.source.rfind("hair", 0) == 0) return "Hair";
        if (b.source.find(".acc") != std::string::npos) return "Accessories";
        return b.source == "head" ? "Face" : "Clothes";
    }
    // The bone's own name first, then its ancestors up to the root.
    for (int i = static_cast<int>(bone); i >= 0; i = scene.bones[static_cast<size_t>(i)].parent) {
        std::string g = groupOfName(scene.bones[static_cast<size_t>(i)].name);
        if (!g.empty()) return g;
        if (scene.bones[static_cast<size_t>(i)].parent == i) break;
    }
    return "Other";
}

std::vector<std::pair<std::string, std::vector<int>>> BoneFrames(const assemble::Scene& scene) {
    static const char* order[] = {"Root", "Torso", "Head and neck", "Face", "Face: Eyes", "Face: Brows", "Face: Mouth and chin", "Face: Cheeks", "Face: Ears", "Face: Nose",
                                  "Chest", "Arms", "Hands", "Legs", "Skirt", "Hair", "Accessories", "Clothes", "Other", "Helpers"};
    std::map<std::string, std::vector<int>> by;
    for (size_t i = 0; i < scene.bones.size(); ++i)
        by[IsHelperBone(scene.bones[i].name) ? "Helpers" : BoneGroup(scene, i)].push_back(static_cast<int>(i));
    std::vector<std::pair<std::string, std::vector<int>>> out;
    if (!scene.bones.empty()) {   // the Root frame: the bone everything hangs from
        out.push_back({"Root", {0}});
        auto& torso = by[IsHelperBone(scene.bones[0].name) ? "Helpers" : BoneGroup(scene, 0)];
        torso.erase(std::remove(torso.begin(), torso.end(), 0), torso.end());
    }
    for (const char* g : order) {
        if (std::string(g) == "Root") continue;
        auto it = by.find(g);
        if (it != by.end() && !it->second.empty()) out.push_back({g, std::move(it->second)});
        if (it != by.end()) by.erase(it);
    }
    for (auto& [g, list] : by) if (!list.empty()) out.push_back({g, std::move(list)});   // any group the order does not know
    return out;
}

} // namespace cardbridge::exporter
