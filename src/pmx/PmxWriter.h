#pragma once

#include "core/Bytes.h"
#include "core/Math.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

// A PMX model as flat data. The writer knows nothing about Koikatsu or Unity;
// the exporter fills this from an assembled scene, and the importer fills it
// from a .pmx. Fields added for import are inert by default, so a model built
// by the exporter serialises exactly as it did before they existed.
//
// Deform types: 0 BDEF1, 1 BDEF2, 2 BDEF4, 3 SDEF, 4 QDEF.
struct Vertex {
    Vec3 position, normal;
    Vec2 uv;
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    float edgeScale = 1.0f;
    // 255: infer the deform type from the weights (legacy exporter behaviour).
    // 0..4: write exactly this type with the stored weights (imported models).
    uint8_t deformType = 255;
    Vec4 additionalUv[4]{};          // PMX additional UV channels (byte 2 of the globals)
    Vec3 sdefC{}, sdefR0{}, sdefR1{}; // SDEF control / R0 / R1 points
};

struct Material {
    std::string name, nameEn;
    Vec4 diffuse{1, 1, 1, 1};
    Vec3 specular{0, 0, 0};
    float specularPower = 5.0f;
    Vec3 ambient{0.5f, 0.5f, 0.5f};
    uint8_t drawFlags = 0;          // 0x01 double-sided, 0x10 edge
    Vec4 edgeColor{0, 0, 0, 1};
    float edgeSize = 1.0f;
    int texture = -1;               // index into Model::textures
    int sphere = -1;                // index into Model::textures of a sphere map (shades by the normal as seen from the camera); -1 = none
    uint8_t sphereMode = 1;         // 1 multiply, 2 add
    int toon = -1;                  // index into Model::textures of this material's own toon ramp; -1 = a shared ramp
    uint32_t indexCount = 0;        // indices (3 per triangle) this material owns, in order
    std::string memo;
    // PMX stores "own ramp" (flag 0 + texture index) and "shared ramp" (flag 1
    // + one index) separately. 255: derive from `toon` (legacy exporter).
    uint8_t toonFlag = 255;
    uint8_t sharedToon = 0;         // shared ramp index when toonFlag == 1
};

// One bone an IK solver may turn, nearest the target first; angle limits (radians, about the bone's own axes at rest) are optional.
struct IkLink {
    int bone = -1;
    bool limited = false;
    Vec3 lower, upper;
};

struct Bone {
    std::string name, nameEn;
    Vec3 position;
    int parent = -1;
    int layer = 0;
    uint16_t flags = 0x001E;        // rotatable, translatable, visible, enabled (bit 0, tail is a bone, and bit 5, IK, are set from the fields below)
    Vec3 tailOffset;                // the tail when tailBone is -1
    int tailBone = -1;              // the bone the tail points to
    int appendParent = -1;          // >= 0: this bone also turns by appendRatio times that bone's rotation (MMD's 付与; the parent needs a lower `layer`)
    float appendRatio = 1.0f;
    Vec3 fixedAxis{0, 0, 0};        // non-zero: the bone turns only about this axis (a twist bone)
    // IK: when `ikTarget` is a bone this is an IK bone. The solver turns the links until `ikTarget` reaches this bone's position, at most
    // `ikLoop` passes of at most `ikLimit` radians each.
    int ikTarget = -1;
    int ikLoop = 40;
    float ikLimit = 1.0f;
    std::vector<IkLink> ikLinks;
    // Bits the legacy exporter never touched (local axis 0x0800, physics after
    // deform 0x1000, external parent 0x2000, translation append 0x0200, ...).
    Vec3 localAxisX{1, 0, 0}, localAxisZ{0, 0, 1};
    int32_t externalParentKey = 0;   // raw external-parent key (4 bytes in PMX)
    bool flagsFromSource = false;    // true: write `flags` verbatim instead of deriving it
};

// A morph. Kinds the exporter uses: a group morph (type 0: several morphs at a rate), a vertex morph (type 1), a UV morph (type 3: per
// vertex a (du, dv, ?, ?) offset - MikuMikuDance applies the first two to the main UV), a material morph (type 8). `panel` is the MMD
// control panel (1 brows, 2 eyes, 3 mouth, 4 other).
struct MaterialOffset {
    int material = -1;              // -1 = all materials
    bool add = true;                // false = multiply
    Vec4 diffuse{0, 0, 0, 0};
    Vec3 specular{0, 0, 0};
    float specularPower = 0.0f;
    Vec3 ambient{0, 0, 0};
    Vec4 edgeColor{0, 0, 0, 0};
    float edgeSize = 0.0f;
    Vec4 texture{0, 0, 0, 0}, sphere{0, 0, 0, 0}, toon{0, 0, 0, 0};
};

struct BoneMorphOffset { int bone = -1; Vec3 move; Vec4 rotate; };  // rotate is a quaternion (x,y,z,w)
struct FlipOffset { int morph = -1; float edge = 0; };
struct ImpulseOffset { int body = -1; uint8_t localFlag = 0; Vec3 velocity, torque; };

// Morph types: 0 group, 1 vertex, 2 bone, 3 UV, 4..7 extra UV, 8 material,
// 9 flip, 10 impulse. 255 infers the type from the legacy vectors below.
struct Morph {
    std::string name, nameEn;
    int panel = 4;
    uint8_t type = 255;
    std::vector<MaterialOffset> materials;               // type 8
    std::vector<std::pair<int, float>> group;            // type 0: (morph index, rate)
    std::vector<std::pair<int, Vec3>> vertices;          // type 1: (vertex index, offset)
    std::vector<std::pair<int, Vec4>> uv;                // types 3..7: (vertex index, offset)
    std::vector<BoneMorphOffset> bones;                  // type 2
    std::vector<FlipOffset> flips;                       // type 9
    std::vector<ImpulseOffset> impulses;                 // type 10
};

// Physics. Shapes: 0 sphere, 1 box, 2 capsule. Modes: 0 follows its bone, 1 physics, 2 physics with the bone aligned.
// `mask` is the groups it collides with (bit n = group n; 0xFFFF = all), the Bullet collision filter.
struct RigidBody {
    std::string name, nameEn;
    int bone = -1;
    uint8_t group = 0;
    uint16_t mask = 0xFFFF;
    uint8_t shape = 0;
    Vec3 size, position, rotation;   // size: radius (sphere), half extents (box), radius and height (capsule); rotation in radians
    float mass = 1.0f, moveDamping = 0.5f, rotDamping = 0.5f, restitution = 0.0f, friction = 0.5f;
    uint8_t mode = 0;
};

// A joint between two rigid bodies. Type 0 is a 6-DOF spring (the only kind
// the legacy exporter wrote), type 1 the plain 6-DOF joint without springs.
struct Joint {
    std::string name, nameEn;
    uint8_t type = 255;             // 255: write a type-0 spring joint (legacy behaviour)
    int bodyA = -1, bodyB = -1;
    Vec3 position, rotation;
    Vec3 moveLower, moveUpper, rotLower, rotUpper, springMove, springRot;
};

// A display frame; `special` frames (Root, Exp) cannot be deleted in an editor.
// `items` preserves the original interleaved order and is authoritative when
// non-empty; clear it after editing `bones`/`morphs` on an imported frame.
struct DisplayFrame {
    std::string name, nameEn;
    bool special = false;
    std::vector<std::pair<uint8_t, int>> items;  // (0 bone, 1 morph, index)
    std::vector<int> bones, morphs;
};

// The on-disk format an imported document came from. The writer reuses it so
// re-exporting an unchanged PMX does not silently upgrade/re-encode it.
struct SourceFormat {
    bool preserve = false;          // false: derive everything (legacy exporter)
    float version = 2.0f;
    uint8_t globalsCount = 8;       // the byte after the version; >= 8
    uint8_t globals[8] = {0, 0, 1, 1, 1, 1, 1, 1}; // encoding, addUv, v, t, m, b, morph, rb index sizes
    Bytes extraGlobals;             // globals beyond the first 8 in a future format
    Bytes trailing;                 // raw bytes after the joints (PMX 2.1 soft bodies / later sections)

    int encoding() const { return globals[0]; }
    int additionalUv() const { return globals[1]; }
    int vertexSize() const { return globals[2]; }
    int textureSize() const { return globals[3]; }
    int materialSize() const { return globals[4]; }
    int boneSize() const { return globals[5]; }
    int morphSize() const { return globals[6]; }
    int rigidBodySize() const { return globals[7]; }
};

struct Model {
    std::string name, nameEn, comment, commentEn;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // ordered by material
    std::vector<std::string> textures;   // relative paths, forward slashes
    std::vector<Material> materials;
    std::vector<Bone> bones;
    std::vector<Morph> morphs;      // listed in the "Exp" display frame unless `frames` is given
    std::vector<RigidBody> rigidBodies;
    std::vector<Joint> joints;
    std::vector<DisplayFrame> frames;   // empty: Root, Exp (every morph) and one frame with every bone
    SourceFormat format;
};

// Turns the whole model half a turn about the vertical axis (x and z negated): a character built facing +Z (Unity) then faces -Z, where
// MikuMikuDance and PMX put a model's front. Positions, normals, bone and morph offsets, rigid bodies and joints turn with it; IK angle limits
// are about the world axes, so the x and z limits mirror (a bend of +a about x is a bend of -a once the model is turned).
void TurnAboutY(Model& m);

// Serialises the model. A generated model becomes PMX 2.0 / UTF-16LE as before;
// a model with `format.preserve` set re-emits its original version, encoding,
// index sizes and trailing bytes. Throws FormatError if the model is
// inconsistent (index out of range, material counts not matching the buffer).
Bytes WritePmx(const Model& m);

// UTF-8 to UTF-16LE for PMX text fields (exposed for tests).
Bytes Utf8ToUtf16Le(const std::string& s);

} // namespace cardbridge::pmx
