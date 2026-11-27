#pragma once

#include <string>
#include <vector>

namespace cardbridge::pmx {

// Side and qualifier information that must never be lost when two bones are
// considered equivalent. A left twist bone is not the right arm, and an IK
// helper is not the deformation bone it assists.
enum class BoneSide { None = 0, Left = 1, Right = 2 };

struct BoneIdentity {
    bool valid = false;
    std::string canonical;        // semantic id, e.g. "upper_body", "arm", "knee"
    BoneSide side = BoneSide::None;
    bool twist = false;
    bool ik = false;
    bool helper = false;
};

// Unicode-stable normalization: trim, lowercase ASCII, fold full-width digits
// and Latin letters, remove separators and whitespace. Side markers are kept.
std::string NormalizeBoneName(const std::string& name);

// Splits a name into a semantic identity. Unknown names are still normalized
// and reported as `valid=false` with their normalized spelling in `canonical`.
BoneIdentity IdentifyBone(const std::string& name);

// Two identities are equivalent only when canonical id, side and every
// qualifier agree. Unknown names compare by normalized spelling.
bool EquivalentBone(const BoneIdentity& a, const BoneIdentity& b);

// Extensible alias registry (no core matching changes needed for new names).
struct BoneAlias {
    std::string canonical;
    std::vector<std::string> names;
};
void RegisterBoneAliases(const std::vector<BoneAlias>& aliases);
const std::vector<BoneAlias>& BuiltinBoneAliases();

} // namespace cardbridge::pmx
