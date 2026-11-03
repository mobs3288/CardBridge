#pragma once

#include "core/Bytes.h"
#include "core/Math.h"

#include <string>
#include <vector>

namespace cardbridge::pmx {

// A PMX 2.0 model as flat data. The writer knows nothing about Koikatsu or
// Unity; the exporter fills this from an assembled scene.
struct Vertex {
    Vec3 position, normal;
    Vec2 uv;
    int bone[4] = {0, 0, 0, 0};
    float weight[4] = {1, 0, 0, 0};
    float edgeScale = 1.0f;
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
    uint32_t indexCount = 0;        // indices (3 per triangle) this material owns, in order
    std::string memo;
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
    // IK: when `ikTarget` is a bone this is an IK bone. The solver turns the links until `ikTarget` reaches this bone's position, at most
    // `ikLoop` passes of at most `ikLimit` radians each.
    int ikTarget = -1;
    int ikLoop = 40;
    float ikLimit = 1.0f;
    std::vector<IkLink> ikLinks;
};

// A morph. Only the two kinds the exporter needs: a material morph (type 8: per material an offset that multiplies or adds to its
// colours) and a group morph (type 0: several morphs at a rate). `panel` is the MMD control panel (1 brows, 2 eyes, 3 mouth, 4 other).
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

struct Morph {
    std::string name, nameEn;
    int panel = 4;
    std::vector<MaterialOffset> materials;               // a material morph when non-empty
    std::vector<std::pair<int, float>> group;            // (morph index, rate): a group morph when non-empty
    std::vector<std::pair<int, Vec3>> vertices;          // (vertex index, offset): a vertex morph when non-empty
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

// A 6-DOF spring joint between two rigid bodies.
struct Joint {
    std::string name, nameEn;
    int bodyA = -1, bodyB = -1;
    Vec3 position, rotation;
    Vec3 moveLower, moveUpper, rotLower, rotUpper, springMove, springRot;
};

// A display frame; `special` frames (Root, Exp) cannot be deleted in an editor.
struct DisplayFrame {
    std::string name, nameEn;
    bool special = false;
    std::vector<int> bones, morphs;
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
};

// Turns the whole model half a turn about the vertical axis (x and z negated): a character built facing +Z (Unity) then faces -Z, where
// MikuMikuDance and PMX put a model's front. Positions, normals, bone and morph offsets, rigid bodies and joints turn with it; IK angle limits
// are about the world axes, so the x and z limits mirror (a bend of +a about x is a bend of -a once the model is turned).
void TurnAboutY(Model& m);

// Serialises to PMX 2.0 (UTF-16LE text, vertex / material / group morphs, bones with IK, rigid bodies and joints).
// Throws FormatError if the model is inconsistent (index out of range, material
// index counts not matching the index buffer).
Bytes WritePmx(const Model& m);

// UTF-8 to UTF-16LE for PMX text fields (exposed for tests).
Bytes Utf8ToUtf16Le(const std::string& s);

} // namespace cardbridge::pmx
