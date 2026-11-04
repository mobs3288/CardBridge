#include "shape/Shape.h"

#include <algorithm>
#include <cmath>

namespace cardbridge::shape {

namespace {

using Bone = ShapeBone;   // the generated formulas call it Bone

#include "shape/ShapeBodyGenerated.inc"
#include "shape/ShapeHeadGenerated.inc"

float lerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d < 0) d += 360.0f;
    if (d > 180.0f) d -= 360.0f;
    return a + d * std::min(1.0f, std::max(0.0f, t));
}
Vec3 lerp(Vec3 a, Vec3 b, float t) {
    t = std::min(1.0f, std::max(0.0f, t));
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

std::vector<std::string> toVec(const char* const* a, size_t n) { return std::vector<std::string>(a, a + n); }

} // namespace

AnimKeys AnimKeys::Parse(const Bytes& data) {
    AnimKeys k;
    ByteReader r(data, false);
    int32_t count = r.i32();
    if (count < 0 || count > 100000) throw FormatError("corrupt key-frame table");
    for (int32_t i = 0; i < count; ++i) {
        std::string name = r.dotnetString();
        int32_t n = r.i32();
        if (n < 0 || static_cast<size_t>(n) * 40 > r.remaining()) throw FormatError("corrupt key-frame list");
        auto& v = k.d_[name];
        v.reserve(static_cast<size_t>(n));
        for (int32_t j = 0; j < n; ++j) {
            r.i32();   // key number
            Key key;
            key.pos = {r.f32(), r.f32(), r.f32()};
            key.rot = {r.f32(), r.f32(), r.f32()};
            key.scl = {r.f32(), r.f32(), r.f32()};
            v.push_back(key);
        }
    }
    return k;
}

bool AnimKeys::get(const std::string& name, float rate, Vec3 v[3], const bool flag[3]) const {
    auto it = d_.find(name);
    if (it == d_.end() || it->second.empty()) return false;
    const auto& keys = it->second;
    // The game clamps a slider to 0..1. The SliderUnlocker plugin (BepisPlugins) lets cards go beyond it and replaces the
    // result outside the range with a straight line through the first and last key (SliderMath.Calculate*); many cards
    // rely on it (values like -0.5 or 1.3). The rotation extrapolation is skipped for a few names, as the plugin does.
    const float rawRate = rate;
    const bool beyond = rawRate < 0.0f || rawRate > 1.0f;
    rate = std::min(1.0f, std::max(0.0f, rate));
    auto sample = [&](int group, Vec3 Key::*field) {
        if (rate == 0.0f) { v[group] = keys.front().*field; return; }
        if (rate == 1.0f) { v[group] = keys.back().*field; return; }
        float pos = static_cast<float>(keys.size() - 1) * rate;
        size_t i = static_cast<size_t>(std::floor(pos));
        float t = pos - static_cast<float>(i);
        if (i + 1 >= keys.size()) { v[group] = keys.back().*field; return; }
        const Vec3& a = keys[i].*field;
        const Vec3& b = keys[i + 1].*field;
        if (group == 1) v[group] = {lerpAngle(a.x, b.x, t), lerpAngle(a.y, b.y, t), lerpAngle(a.z, b.z, t)};
        else v[group] = lerp(a, b, t);
    };
    if (flag[0]) sample(0, &Key::pos);
    if (flag[1]) sample(1, &Key::rot);
    if (flag[2]) sample(2, &Key::scl);
    if (beyond && keys.size() >= 2) {
        const Key& k0 = keys.front();
        const Key& kn = keys.back();
        auto line = [&](Vec3 a, Vec3 b) { return a + (b + a * -1.0f) * rawRate; };
        if (flag[0]) v[0] = line(k0.pos, kn.pos);
        if (flag[2]) v[2] = line(k0.scl, kn.scl);
        auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
        bool keepRot = has("cf_s_Mune") || has("cf_s_Mouth") || has("cf_s_LegLow") || has("cf_s_MayuTip") || (has("thigh") && has("01")) ||
                       (name.rfind("cf_a_bust", 0) == 0 && name.size() >= 5 && name.compare(name.size() - 5, 5, "_size") == 0);
        if (flag[1] && !keepRot) {
            // SliderMath.CalculateRotation: the full turn from the first to the last key, unwrapped to the direction of the first step.
            const Vec3 r0 = k0.rot, r1 = keys[1].rot, rn = kn.rot;
            float d1[3] = {r1.x - r0.x, r1.y - r0.y, r1.z - r0.z};
            float dn[3] = {rn.x - r0.x, rn.y - r0.y, rn.z - r0.z};
            for (int a = 0; a < 3; ++a) {
                bool up = d1[a] >= 0.0f;
                if (dn[a] > 0.0f && !up) dn[a] -= 360.0f;
                else if (dn[a] < 0.0f && up) dn[a] += 360.0f;
            }
            const float base[3] = {r0.x, r0.y, r0.z}, last[3] = {rn.x, rn.y, rn.z};
            float out[3];
            for (int a = 0; a < 3; ++a)
                out[a] = rawRate < 0.0f ? base[a] - dn[a] * std::fabs(rawRate) : last[a] + dn[a] * std::fabs(rawRate - 1.0f);
            v[1] = {out[0], out[1], out[2]};
        }
    }
    return true;
}

CategoryTable ParseCategories(const std::string& text, const std::map<std::string, int>& srcIds) {
    CategoryTable t;
    size_t p = 0;
    while (p < text.size()) {
        size_t q = text.find('\n', p);
        std::string line = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
        p = q == std::string::npos ? text.size() : q + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> col;
        size_t a = 0;
        while (true) {
            size_t b = line.find('\t', a);
            col.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (col.size() < 11) continue;
        CategoryRow row;
        row.name = col[1];
        auto it = srcIds.find(row.name);
        if (it == srcIds.end()) continue;   // the game skips names without an id too
        row.id = it->second;
        for (int g = 0; g < 3; ++g) {
            for (int ax = 0; ax < 3; ++ax) row.use[g][ax] = col[static_cast<size_t>(2 + g * 3 + ax)] != "0";
            row.getflag[g] = row.use[g][0] || row.use[g][1] || row.use[g][2];
        }
        t[std::atoi(col[0].c_str())].push_back(row);
    }
    return t;
}

bool ParseCorrectTable(const Bytes& data, ShapeState& S) {
    try {
        ByteReader r(data);
        const int32_t n = r.i32();
        if (n < 0 || static_cast<size_t>(n) > data.size() / 36) return false;
        for (int32_t i = 0; i < n; ++i) {
            ShapeSrc& c = S.correct(i);
            c.pos = {r.f32(), r.f32(), r.f32()};
            c.rot = {r.f32(), r.f32(), r.f32()};
            c.scl = {r.f32(), r.f32(), r.f32()};
        }
    } catch (const FormatError&) {
        return false;
    }
    return true;
}

void ChangeValue(ShapeState& S, const AnimKeys& anm, const CategoryTable& cats, int slider, float value) {
    auto it = cats.find(slider);
    if (it == cats.end()) return;
    for (const CategoryRow& row : it->second) {
        Vec3 v[3] = {};
        anm.get(row.name, value, v, row.getflag);
        ShapeSrc& s = S.src(row.id);
        if (row.use[0][0]) s.pos.x = v[0].x;
        if (row.use[0][1]) s.pos.y = v[0].y;
        if (row.use[0][2]) s.pos.z = v[0].z;
        if (row.use[1][0]) s.rot.x = v[1].x;
        if (row.use[1][1]) s.rot.y = v[1].y;
        if (row.use[1][2]) s.rot.z = v[1].z;
        if (row.use[2][0]) s.scl.x = v[2].x;
        if (row.use[2][1]) s.scl.y = v[2].y;
        if (row.use[2][2]) s.scl.z = v[2].z;
    }
}

const std::vector<std::string>& BodyDstNames() { static auto v = toVec(kBodyDst, sizeof kBodyDst / sizeof *kBodyDst); return v; }
const std::vector<std::string>& BodySrcNames() { static auto v = toVec(kBodySrc, sizeof kBodySrc / sizeof *kBodySrc); return v; }
const std::vector<std::string>& HeadDstNames() { static auto v = toVec(kHeadDst, sizeof kHeadDst / sizeof *kHeadDst); return v; }
const std::vector<SliderInfo>& BodySliders() {
    static const std::vector<SliderInfo> v = {
        {"Overall", "Height"}, {"Overall", "Head size"}, {"Neck", "Width"}, {"Neck", "Thickness"},
        {"Bust", "Size"}, {"Bust", "Vertical position"}, {"Bust", "Angle (left-right)"}, {"Bust", "Spacing"}, {"Bust", "Angle (up-down)"},
        {"Bust", "Depth"}, {"Bust", "Roundness"}, {"Bust", "Areola puffiness"}, {"Bust", "Nipple thickness"}, {"Bust", "Nipple depth"},
        {"Upper body", "Shoulder-chest width"}, {"Upper body", "Shoulder-chest depth"}, {"Upper body", "Chest width"}, {"Upper body", "Chest depth"},
        {"Upper body", "Ribs width"}, {"Upper body", "Ribs depth"}, {"Upper body", "Torso height"},
        {"Waist", "Belly"}, {"Waist", "Waist width"}, {"Waist", "Waist depth"},
        {"Hips", "Hip width"}, {"Hips", "Hip depth"}, {"Hips", "Butt size"}, {"Hips", "Butt angle"},
        {"Legs", "Thigh width"}, {"Legs", "Thigh depth"}, {"Legs", "Lower thigh width"}, {"Legs", "Lower thigh depth"},
        {"Legs", "Calf width"}, {"Legs", "Calf depth"}, {"Legs", "Shin"}, {"Legs", "Ankle width"}, {"Legs", "Ankle depth"},
        {"Arms", "Shoulder width"}, {"Arms", "Shoulder thickness"}, {"Arms", "Upper arm width"}, {"Arms", "Upper arm thickness"},
        {"Arms", "Forearm width"}, {"Arms", "Forearm thickness"}, {"Arms", "Wrist"},
    };
    return v;
}

const std::vector<SliderInfo>& FaceSliders() {
    static const std::vector<SliderInfo> v = {
        {"Face", "Overall width"}, {"Face", "Upper depth"}, {"Face", "Upper height"}, {"Face", "Forehead height"}, {"Face", "Lower depth"},
        {"Face", "Lower width"}, {"Face", "Jaw height"}, {"Face", "Jaw width"},
        {"Chin", "Height"}, {"Chin", "Depth"}, {"Chin", "Width"}, {"Chin", "Tip height"}, {"Chin", "Tip depth"}, {"Chin", "Tip width"},
        {"Cheeks", "Cheekbone height"}, {"Cheeks", "Cheekbone depth"}, {"Cheeks", "Cheek width"}, {"Cheeks", "Cheek height"}, {"Cheeks", "Cheek base"},
        {"Eyebrows", "Vertical position"}, {"Eyebrows", "Spacing"}, {"Eyebrows", "Angle"}, {"Eyebrows", "Middle"}, {"Eyebrows", "Tip"},
        {"Eyes", "Outline 1"}, {"Eyes", "Outline 2"}, {"Eyes", "Outline 3"}, {"Eyes", "Outline 4"}, {"Eyes", "Outline 5"}, {"Eyes", "Outline 6"},
        {"Eyes", "Spacing"}, {"Eyes", "Angle"}, {"Eyes", "Depth"}, {"Eyes", "Horizontal position"}, {"Eyes", "Tilt"}, {"Eyes", "Tilt 2"},
        {"Eyes", "Height"}, {"Eyes", "Width"},
        {"Nose", "Tip"}, {"Nose", "Base angle"}, {"Nose", "Bridge angle"},
        {"Mouth", "Vertical position"}, {"Mouth", "Width"}, {"Mouth", "Angle"}, {"Mouth", "Upper lip"}, {"Mouth", "Lower lip"}, {"Mouth", "Corners"},
        {"Ears", "Angle"}, {"Ears", "Angle 2"}, {"Ears", "Angle 3"}, {"Ears", "Upper"}, {"Ears", "Lower"},
    };
    return v;
}

const std::vector<std::string>& HeadSrcNames() { static auto v = toVec(kHeadSrc, sizeof kHeadSrc / sizeof *kHeadSrc); return v; }

void RunBodyUpdate(ShapeState& S, unsigned updateMask) { BodyUpdate(S, updateMask); }
void RunBodyUpdateAlways(ShapeState& S) { BodyUpdateAlways(S); }
void RunHeadUpdate(ShapeState& S) { HeadUpdate(S); }
void RunHeadUpdateAlways(ShapeState& S) { HeadUpdateAlways(S); }

Quat EulerToQuat(Vec3 d) {
    const float k = 3.14159265358979f / 360.0f;   // half angle in radians
    float cx = std::cos(d.x * k), sx = std::sin(d.x * k);
    float cy = std::cos(d.y * k), sy = std::sin(d.y * k);
    float cz = std::cos(d.z * k), sz = std::sin(d.z * k);
    // q = qY * qX * qZ  (Unity rotates about Z first, then X, then Y)
    Quat qy{0, sy, 0, cy}, qx{sx, 0, 0, cx}, qz{0, 0, sz, cz};
    auto mul = [](Quat a, Quat b) {
        return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    };
    return mul(mul(qy, qx), qz);
}

} // namespace cardbridge::shape
