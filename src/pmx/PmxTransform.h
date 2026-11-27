#pragma once

#include "core/Math.h"
#include "library/AssetTemplate.h"

namespace cardbridge::pmx {

// Generic, reusable asset transform. Rotation is in degrees, XYZ order; scale
// is applied before rotation, then translation. The source template is never
// modified — ApplyTransform returns a transformed copy.
struct AssetTransform {
    Vec3 translation{0, 0, 0};
    Vec3 rotationDeg{0, 0, 0};
    Vec3 scale{1, 1, 1};
    bool identity() const {
        return translation.x == 0 && translation.y == 0 && translation.z == 0 &&
               rotationDeg.x == 0 && rotationDeg.y == 0 && rotationDeg.z == 0 &&
               scale.x == 1 && scale.y == 1 && scale.z == 1;
    }
};

Vec3 TransformPoint(Vec3 p, const AssetTransform& t);
Vec3 TransformNormal(Vec3 n, const AssetTransform& t);
library::AssetTemplate ApplyTransform(const library::AssetTemplate& source, const AssetTransform& t);

} // namespace cardbridge::pmx
