#include "pmx/BoneAliases.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace cardbridge::pmx {
namespace {

std::vector<BoneAlias>& registry() {
    static std::vector<BoneAlias> aliases = [] {
        std::vector<BoneAlias> a = {
            {"center",       {"センター", "Center", "cf_j_root", "cf_n_height"}},
            {"lower_body",   {"下半身", "LowerBody", "cf_j_hips"}},
            {"upper_body",   {"上半身", "UpperBody", "cf_j_spine01"}},
            {"upper_body2",  {"上半身2", "UpperBody2", "cf_j_spine02"}},
            {"neck",         {"首", "Neck", "cf_j_neck", "cf_s_neck"}},
            {"head",         {"頭", "Head", "cf_j_head"}},
            {"shoulder",     {"肩", "Shoulder", "cf_j_shoulder", "cf_s_shoulder"}},
            {"arm",          {"腕", "Arm", "cf_j_arm00", "cf_s_arm00"}},
            {"elbow",        {"ひじ", "肘", "Elbow", "cf_j_forearm01"}},
            {"wrist",        {"手首", "Wrist", "cf_j_hand", "cf_s_hand"}},
            {"hand",         {"手", "Hand", "cf_j_hand"}},
            {"leg",          {"足", "Leg", "cf_j_thigh00", "cf_s_thigh00", "cf_s_leg01"}},
            {"knee",         {"ひざ", "膝", "Knee", "cf_j_leg01", "cf_s_kneeB"}},
            {"ankle",        {"足首", "Ankle", "cf_j_foot", "cf_s_leg02"}},
            {"toe",          {"つま先", "Toe", "cf_j_toes"}},
            {"waist",        {"腰", "Waist", "cf_s_waist01", "cf_s_waist02"}},
            {"hip",          {"Hip", "cf_s_siri"}},
            {"chest",        {"胸", "Chest", "cf_j_bust01", "cf_s_bust01", "cf_s_bust02"}},
            {"upper_body",   {"上半身1", "cf_s_spine01"}},
            {"upper_body2",  {"上半身2", "cf_s_spine02"}},
            {"waist",        {"腰", "Waist", "cf_s_waist01", "cf_s_waist02"}},
            {"elbow",        {"cf_s_forearm01", "cf_j_forearm01"}},
            {"arm_twist",    {"腕捩", "ArmTwist", "cf_s_forearm02", "cf_s_forearm03"}},
            {"leg",          {"cf_s_thigh00", "cf_s_thigh01", "cf_s_thigh02", "cf_j_thigh00"}},
            {"knee",         {"cf_s_kneeB", "cf_s_thigh03"}},
            {"ankle",        {"cf_s_leg01", "cf_s_leg02", "cf_s_leg03", "cf_j_foot"}},
        };
        // Pre-normalize every alias once so matching is cheap and consistent.
        for (BoneAlias& entry : a)
            for (std::string& name : entry.names) name = NormalizeBoneName(name);
        return a;
    }();
    return aliases;
}

std::mutex& registryMutex() {
    static std::mutex mu;
    return mu;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isSeparator(uint32_t c) {
    return c == ' ' || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == ':' || c == 0x30FB;   // Katakana middle dot
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

BoneSide detectSide(const std::string& raw) {
    std::string lower;
    for (unsigned char c : raw) lower += static_cast<char>(std::tolower(c));
    if (raw.rfind("左", 0) == 0 || lower.rfind("left", 0) == 0) return BoneSide::Left;
    if (raw.rfind("右", 0) == 0 || lower.rfind("right", 0) == 0) return BoneSide::Right;
    const size_t n = lower.size();
    auto ends = [&](const char* s) {
        const size_t l = std::strlen(s);
        return n >= l && lower.compare(n - l, l, s) == 0;
    };
    if (ends("_l") || ends(" l") || ends(".l") || ends("(l)") || ends("_left")) return BoneSide::Left;
    if (ends("_r") || ends(" r") || ends(".r") || ends("(r)") || ends("_right")) return BoneSide::Right;
    return BoneSide::None;
}

} // namespace

std::string NormalizeBoneName(const std::string& name) {
    std::string out;
    size_t i = 0;
    while (i < name.size()) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }
        ++i;
        for (int k = 0; k < extra && i < name.size(); ++k, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(name[i]) & 0x3F);
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;   // full-width ASCII
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if (!isSeparator(cp)) appendUtf8(out, cp);
    }
    return out;
}

BoneIdentity IdentifyBone(const std::string& name) {
    BoneIdentity id;
    const std::string normalized = NormalizeBoneName(name);
    // Alias lookup uses the name without its side marker so that "左腕" and
    // "cf_j_arm00_L" both resolve to the "arm" semantic, while `side` keeps
    // them distinct from the right arm.
    std::string core = name;
    for (const char* prefix : {"左", "右", "left", "Left", "LEFT", "right", "Right", "RIGHT"}) {
        const size_t l = std::strlen(prefix);
        if (core.rfind(prefix, 0) == 0) { core = core.substr(l); break; }
    }
    const size_t c = core.size();
    for (const char* suffix : {"_l", "_r", "_L", "_R", "_left", "_right", "_Left", "_Right", " L", " R"}) {
        const size_t l = std::strlen(suffix);
        if (c >= l && core.compare(c - l, l, suffix) == 0) { core = core.substr(0, c - l); break; }
    }
    const std::string coreNorm = NormalizeBoneName(core);
    const std::string lower = [&] {
        std::string s;
        for (unsigned char c : normalized) s += static_cast<char>(std::tolower(c));
        return s;
    }();
    id.side = detectSide(name);
    id.twist = contains(lower, "twist") || contains(normalized, "捩") || contains(normalized, "ねじれ");
    id.ik = contains(lower, "ik");
    id.helper = contains(lower, "helper") || contains(lower, "dummy") ||
                contains(normalized, "補助") || contains(normalized, "ダミー");
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        for (const BoneAlias& entry : registry())
            for (const std::string& alias : entry.names)
                // Short aliases such as "arm" must not match inside "forearm":
                // substring matching is only allowed for long, distinctive ids.
                if (coreNorm == alias || (alias.size() >= 6 && contains(coreNorm, alias))) {
                    id.valid = true;
                    id.canonical = entry.canonical;
                    if (id.canonical == "arm_twist") id.twist = true;
                    return id;
                }
    }
    id.canonical = normalized;   // unknown: still usable for exact/normalized matching
    return id;
}

bool EquivalentBone(const BoneIdentity& a, const BoneIdentity& b) {
    return a.canonical == b.canonical && a.side == b.side &&
           a.twist == b.twist && a.ik == b.ik && a.helper == b.helper;
}

void RegisterBoneAliases(const std::vector<BoneAlias>& aliases) {
    std::lock_guard<std::mutex> lk(registryMutex());
    auto& reg = registry();
    for (BoneAlias entry : aliases) {
        for (std::string& name : entry.names) name = NormalizeBoneName(name);
        reg.push_back(std::move(entry));
    }
}

const std::vector<BoneAlias>& BuiltinBoneAliases() {
    return registry();
}

} // namespace cardbridge::pmx
