#pragma once

#include "core/Bytes.h"
#include "core/Math.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::shape {

// The game's "customize" sliders (44 body, 52 face) are not geometry: each slider value is turned into bone
// scales, offsets and rotations by a few hundred lines of formulas (ShapeBodyInfoFemale / ShapeHeadInfoFemale).
// Those formulas are copied here as generated C++ (tools/gen_shape.py); this file is the machinery around
// them: the key-frame table, the slider-to-source-bone categories and the state the formulas read and write.

struct ShapeBone {
    Vec3 pos, rot;               // local position, local Euler angles (degrees)
    Vec3 scl{1, 1, 1};
    bool rotSet = false;         // true once a formula assigned `rot`
};

struct ShapeSrc {
    Vec3 pos, rot;
    Vec3 scl{1, 1, 1};
};

// Key frames per source name: the sampled shape animation (rate 0..1 across the keys).
class AnimKeys {
public:
    struct Key { Vec3 pos, rot, scl; };
    // The binary written by AnimationKeyInfo.SaveInfo: int count; per entry a .NET string, int keyCount and
    // keyCount x (int no, 9 floats).
    static AnimKeys Parse(const Bytes& data);
    bool empty() const { return d_.empty(); }
    size_t keyCount() const { return d_.empty() ? 0 : d_.begin()->second.size(); }
    // v[0..2] = position, Euler rotation, scale at `rate`, only for the axes groups flagged.
    bool get(const std::string& name, float rate, Vec3 v[3], const bool flag[3]) const;

private:
    std::map<std::string, std::vector<Key>> d_;
};

struct CategoryRow {
    int id = 0;                  // index of the source name
    std::string name;            // source name = key into AnimKeys
    bool use[3][3] = {};         // [pos|rot|scl][x|y|z]
    bool getflag[3] = {};
};
using CategoryTable = std::map<int, std::vector<CategoryRow>>;   // slider -> rows

// Parses the tab-separated customshape list (slider, name, 9 flags per line). `srcNames` maps name -> id.
CategoryTable ParseCategories(const std::string& text, const std::map<std::string, int>& srcIds);

// Everything the generated formulas touch.
struct ShapeState {
    bool initEnd = true;
    int typeBone = 0;
    float correctHeadSize = 1.0f, correctNeckSize = 1.0f, headCorrectValue = 1.0f;
    std::function<ShapeBone*(int)> dstFn;
    std::function<Vec3(ShapeBone*)> parentScaleFn;
    std::map<int, ShapeSrc> srcMap, correctMap;
    ShapeBone* fixCorrect[2] = {nullptr, nullptr};

    ShapeBone* dst(int i) { return dstFn ? dstFn(i) : nullptr; }
    ShapeSrc& src(int i) { return srcMap[i]; }
    ShapeSrc& correct(int i) {
        auto it = correctMap.find(i);
        if (it == correctMap.end()) { ShapeSrc z; z.scl = {0, 0, 0}; it = correctMap.emplace(i, z).first; }   // the game starts these at scale zero
        return it->second;
    }
    Vec3 parentLossyScale(ShapeBone* b) { return parentScaleFn ? parentScaleFn(b) : Vec3{1, 1, 1}; }
};

// Sets one slider: samples the key frames at `value` into the source bones (ShapeInfoBase.ChangeValue).
void ChangeValue(ShapeState& S, const AnimKeys& anm, const CategoryTable& cats, int slider, float value);

// Names of the bones a shape drives, by dst index (the generated enum order).
const std::vector<std::string>& BodyDstNames();
const std::vector<std::string>& BodySrcNames();
const std::vector<std::string>& HeadDstNames();
const std::vector<std::string>& HeadSrcNames();

// Runs the generated formulas.
void RunBodyUpdate(ShapeState& S, unsigned updateMask = 7);
void RunBodyUpdateAlways(ShapeState& S);
void RunHeadUpdate(ShapeState& S);
void RunHeadUpdateAlways(ShapeState& S);

// What each customize slider is, in the maker's own grouping: index = the slider's number in the card (shapeValueBody 44,
// shapeValueFace 52). Labels follow the bones a slider drives; the card format has no names of its own.
struct SliderInfo {
    const char* group;
    const char* label;
};
const std::vector<SliderInfo>& BodySliders();
const std::vector<SliderInfo>& FaceSliders();

// Unity's Quaternion.Euler(x, y, z): rotate z, then x, then y (degrees).
Quat EulerToQuat(Vec3 degrees);

} // namespace cardbridge::shape
