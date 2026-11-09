#include "assemble/AutoCloth.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace cardbridge::assemble {

namespace {

Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(Vec3 v) { return std::sqrt(dot(v, v)); }
float dist2(Vec3 a, Vec3 b) { const Vec3 d = sub(a, b); return dot(d, d); }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float smooth01(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }

int nearestRigBone(const Scene& scene, Vec3 p) {
    int best = -1;
    float bestD = 1e30f;
    for (size_t i = 0; i < scene.bones.size(); ++i) {
        if (scene.bones[i].source != "rig") continue;
        const float d = dist2(p, scene.bones[i].pos);
        if (d < bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

// Synthetic bone rest orientation inherits its parent's rotation so Reshape
// (which preserves parent-relative offsets) and pose keep the garment's
// silhouette. An identity rotation here shears the chain when the body is
// reshaped or posed.
Mat4 worldFor(Vec3 pos, const Scene& scene, int parent) {
    Mat4 w;
    if (parent >= 0 && static_cast<size_t>(parent) < scene.bones.size()) {
        const Mat4& p = scene.bones[static_cast<size_t>(parent)].world;
        for (int idx : {0, 1, 2, 4, 5, 6, 8, 9, 10}) w.m[idx] = p.m[idx];
    }
    w.m[3] = pos.x; w.m[7] = pos.y; w.m[11] = pos.z;
    w.m[12] = w.m[13] = w.m[14] = 0.0f; w.m[15] = 1.0f;
    return w;
}

} // namespace

int SynthesizeAutoCloth(Scene& scene, const std::function<void(const std::string&)>& log) {
    if (std::getenv("CARDBRIDGE_NO_AUTO_CLOTH")) return 0;
    if (const char* ac = std::getenv("CARDBRIDGE_AUTO_CLOTH"); ac && std::string(ac) == "0") return 0;
    if (scene.bones.empty() || scene.meshes.empty()) return 0;

    std::set<std::string> hasAuthored;
    for (const auto& ch : scene.chains) hasAuthored.insert(ch.part);

    int chainsMade = 0, bonesMade = 0, partsPatched = 0;

    for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
        ScenePart& part = scene.parts[pi];
        if (part.underPiece || part.nudeSwitch) continue;
        if (part.group != "Accessories" && part.group != "Outfit") continue;
        // Accessories: chest/back/waist/arm (shoulder coats, wings, capes) hang free
        // from their node. Garments: only loose tops/gloves/subparts - body-fit
        // brass/plants keep their authored skinning (no chains).
        if (part.group == "Accessories") {
            if (part.category != 124 && part.category != 125 && part.category != 126 && part.category != 128) continue;
        } else { // Outfit garments
            if (!(part.category == 105 || part.category == 109 || (part.category >= 200 && part.category <= 212))) continue;
        }
        if (part.meshes.empty()) continue;
        if (hasAuthored.count(part.id)) continue; // authored DynamicBone wins
        bool hasShapes = false;
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            if (!scene.meshes[static_cast<size_t>(mi)].shapes.empty()) { hasShapes = true; break; }
        }
        if (hasShapes) continue; // re-skinning would break morph offsets; cloth with blend shapes is rare

        struct VRef { int mesh; size_t vert; };
        std::vector<VRef> all;
        for (int mi : part.meshes) {
            if (mi < 0 || static_cast<size_t>(mi) >= scene.meshes.size()) continue;
            const auto& m = scene.meshes[static_cast<size_t>(mi)];
            for (size_t vi = 0; vi < m.vertices.size(); ++vi) all.push_back({mi, vi});
        }
        if (all.size() < 80) continue; // small trinket (hat, glasses, ring): not hanging cloth

        // Anchor for hang measurement.
        int partAnchorBone = -1;
        Vec3 anchorPos{0, 0, 0};
        if (!part.attachNode.empty()) {
            const int bi = scene.boneIndex(part.attachNode);
            if (bi >= 0) { partAnchorBone = bi; anchorPos = scene.bones[static_cast<size_t>(bi)].pos; }
        }
        if (partAnchorBone < 0) {
            // Garment: top of the mesh is where it is tied.
            float maxY = -1e30f;
            for (const auto& r : all) maxY = std::max(maxY, scene.meshes[static_cast<size_t>(r.mesh)].vertices[r.vert].pos.y);
            Vec3 c{0, 0, 0};
            int n = 0;
            for (const auto& r : all) {
                const Vec3 p = scene.meshes[static_cast<size_t>(r.mesh)].vertices[r.vert].pos;
                if (p.y > maxY - 0.05f) { c = c + p; ++n; }
            }
            if (n == 0) continue;
            c = c * (1.0f / n);
            partAnchorBone = nearestRigBone(scene, c);
            if (partAnchorBone < 0) continue;
            anchorPos = scene.bones[static_cast<size_t>(partAnchorBone)].pos;
        }

        float minY = 1e30f, sideMax = 0;
        for (const auto& r : all) {
            const Vec3 p = scene.meshes[static_cast<size_t>(r.mesh)].vertices[r.vert].pos;
            minY = std::min(minY, p.y);
            const float dx = p.x - anchorPos.x, dz = p.z - anchorPos.z;
            sideMax = std::max(sideMax, std::sqrt(dx * dx + dz * dz));
        }
        const float hangDepth = anchorPos.y - minY;
        if (std::getenv("CARDBRIDGE_DEBUG_CLOTH")) std::fprintf(stderr, "clothdbg %s cat=%d verts=%zu anchor=%s (%.2f %.2f %.2f) minY=%.2f hang=%.2f side=%.2f\n",
            part.id.c_str(), part.category, all.size(), partAnchorBone >= 0 && static_cast<size_t>(partAnchorBone) < scene.bones.size() ? scene.bones[static_cast<size_t>(partAnchorBone)].name.c_str() : "?", anchorPos.x, anchorPos.y, anchorPos.z, minY, hangDepth, sideMax);
        if (hangDepth < 0.07f && sideMax < 0.10f) continue; // nothing hangs

        struct HV { int mesh; size_t vert; Vec3 pos; };
        std::vector<HV> hanging;
        for (const auto& r : all) {
            const Vec3 p = scene.meshes[static_cast<size_t>(r.mesh)].vertices[r.vert].pos;
            const float dy = anchorPos.y - p.y;
            const float dx = p.x - anchorPos.x, dz = p.z - anchorPos.z;
            const float horiz = std::sqrt(dx * dx + dz * dz);
            bool hang = false;
            if (part.group == "Outfit") {
                // Garments are body-fit shells: require clearly loose geometry so the
                // torso (horiz~0.10, dy~0.4) is never taken as cloth. Kimono wide
                // sleeves stick out sideways, overhang tails drop far behind/in front.
                hang = (horiz > 0.20f) || (dy > 0.25f && horiz > 0.14f) || (dy > 0.30f && horiz > 0.12f);
            } else {
                hang = (dy > 0.04f || horiz > 0.09f); // accessories hang from their node
            }
            if (hang) hanging.push_back({r.mesh, r.vert, p});
        }
        if (hanging.size() < 50 || hanging.size() < static_cast<size_t>(static_cast<float>(all.size()) * 0.04f)) continue;

        // Split hanging verts into clusters (left / right / mid). Deterministic
        // and universal: kimono sleeves split left/right, shoulder-coat back
        // panels fall into mid, wings split left/right, coat tails -> mid.
        std::vector<HV> left, right, mid;
        for (const auto& hv : hanging) {
            const float dx = hv.pos.x - anchorPos.x;
            if (dx < -0.02f) left.push_back(hv);
            else if (dx > 0.02f) right.push_back(hv);
            else mid.push_back(hv);
        }
        struct Cluster { const char* suffix; std::vector<HV> verts; };
        std::vector<Cluster> clusters;
        const float hN = static_cast<float>(hanging.size());
        const bool splitLR = static_cast<float>(left.size()) >= 0.15f * hN && static_cast<float>(right.size()) >= 0.15f * hN;
        const bool needMid = static_cast<float>(mid.size()) >= 0.25f * hN;
        if (splitLR) {
            if (needMid) {
                clusters.push_back({"left", left});
                clusters.push_back({"right", right});
                clusters.push_back({"mid", mid});
            } else {
                for (const auto& hv : mid) {
                    if (hv.pos.x - anchorPos.x >= 0) right.push_back(hv);
                    else left.push_back(hv);
                }
                clusters.push_back({"left", left});
                clusters.push_back({"right", right});
            }
        } else {
            clusters.push_back({"fall", hanging});
        }

        bool partPatched = false;
        for (const auto& cl : clusters) {
            // A top garment's center hanging verts ARE its torso (body-fit, not
            // a tail). Synthesizing there collapses the chest; kimono/overhang
            // sleeves and tails live in the left/right clusters.
            if (part.group == "Outfit" && part.category == 105) {
                const std::string suf = cl.suffix;
                if (suf == "mid" || suf == "fall") continue; // left/right only; back tails live on bottom/accessory
            }
            if (cl.verts.size() < 30 || static_cast<float>(cl.verts.size()) < 0.08f * hN) continue;
            // Top (nearest anchor) and tip (farthest) centroids.
            std::vector<size_t> order(cl.verts.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return dist2(cl.verts[a].pos, anchorPos) < dist2(cl.verts[b].pos, anchorPos);
            });
            const size_t topN = std::max<size_t>(1, cl.verts.size() / 7);
            Vec3 top{0, 0, 0}, tip{0, 0, 0};
            for (size_t k = 0; k < topN; ++k) top = top + cl.verts[order[k]].pos;
            for (size_t k = 0; k < topN; ++k) tip = tip + cl.verts[order[order.size() - 1 - k]].pos;
            top = top * (1.0f / topN);
            tip = tip * (1.0f / topN);
            Vec3 dir = sub(tip, top);
            const float clen = len(dir);
            if (clen < 0.09f) continue; // not a hang, a smudge
            Vec3 unit = dir * (1.0f / clen);

            int clusterAnchor = partAnchorBone;
            if (part.attachNode.empty()) { // garment: root the chain where the cloth
                // leaves the body (nearest rig joint to the cluster top), not where
                // its tip happens to be skinned. The tip itself keeps its authored
                // base influence in the weight blend (see rebind below), so elbow/
                // hip articulation is inherited via the blend + the root parent.
                const int nb = nearestRigBone(scene, top);
                if (nb >= 0) clusterAnchor = nb;
            }
            if (clusterAnchor < 0) continue;

            // ~10cm per link, 3..6 bones: long overhang coats (0.6-0.9m) go to
            // 6 short capsules so the PMX bodies and the Y-preview particles
            // track the shin/hand bends instead of cutting corners.
            const int N = static_cast<int>(std::clamp(std::round(clen / 0.10f), 3.0f, 6.0f));
            // Band centroids along the chain (keeps bones inside the cloth).
            std::vector<Vec3> bandPos(static_cast<size_t>(N));
            std::vector<int> bandCount(static_cast<size_t>(N), 0);
            for (const auto& hv : cl.verts) {
                const float t = clamp01(dot(sub(hv.pos, top), unit) / clen);
                const int b = std::min(N - 1, static_cast<int>(t * N));
                bandPos[static_cast<size_t>(b)] = bandPos[static_cast<size_t>(b)] + hv.pos;
                bandCount[static_cast<size_t>(b)]++;
            }
            for (int b = 0; b < N; ++b) {
                if (bandCount[static_cast<size_t>(b)] > 0) bandPos[static_cast<size_t>(b)] = bandPos[static_cast<size_t>(b)] * (1.0f / bandCount[static_cast<size_t>(b)]);
                else {
                    const float t = (static_cast<float>(b) + 0.5f) / static_cast<float>(N);
                    bandPos[static_cast<size_t>(b)] = top + dir * t;
                }
            }

            std::vector<int> chainBones;
            int parent = clusterAnchor;
            for (int j = 0; j < N; ++j) {
                std::string name = part.id + "/auto_" + cl.suffix + "_" + std::to_string(j);
                for (int n = 2; scene.boneIndex(name) >= 0; ++n) name = part.id + "/auto_" + cl.suffix + "_" + std::to_string(j) + "#" + std::to_string(n);
                SceneBone nb;
                nb.name = name;
                nb.parent = parent;
                nb.pos = bandPos[static_cast<size_t>(j)];
                nb.world = worldFor(nb.pos, scene, parent);
                nb.source = part.id;
                scene.bones.push_back(nb);
                const int idx = static_cast<int>(scene.bones.size()) - 1;
                chainBones.push_back(idx);
                parent = idx;
            }
            if (chainBones.size() < 2) continue;

            // Loose-cloth preset, stiffer for sideways flaps (wings / wide sleeves).
            const float horizLen = std::sqrt(dir.x * dir.x + dir.z * dir.z);
            const bool flap = horizLen > std::fabs(dir.y) * 1.25f;
            SceneChain ch;
            ch.name = part.id + "/auto cloth " + cl.suffix;
            ch.part = part.id;
            ch.bones = chainBones;
            if (flap) { ch.damping = 0.30f; ch.elasticity = 0.25f; ch.stiffness = 0.35f; ch.inert = 0.04f; ch.radius = 0.025f; }
            else { ch.damping = 0.30f; ch.elasticity = 0.40f; ch.stiffness = 0.12f; ch.inert = 0.08f; ch.radius = 0.015f; }
            // Per-step units (PhysicsSim adds this directly each 1/60s update);
            // keep it small so rest-aligned chains are not snapped, wings only sag gently.
            ch.gravity = {0, -0.15, 0};
            ch.force = {0, 0, 0};
            scene.chains.push_back(ch);
            ++chainsMade;
            bonesMade += static_cast<int>(chainBones.size());
            partPatched = true;

            // Hybrid blend: keep the authored multi-bone blend scaled by
            // (1-wAuto) so the garment's base deformation (elbow/hip flex) is
            // preserved, and add secondary cloth motion via the auto bone
            // (wAuto). Capped at 0.75 so the root never detaches; at rest the
            // rest pose is unchanged (skin == identity for both).
            for (const auto& hv : cl.verts) {
                const float t = clamp01(dot(sub(hv.pos, top), unit) / clen);
                if (t < 0.15f) continue;
                const float wAuto = smooth01((t - 0.15f) / 0.50f) * 0.75f;
                const int targetLocal = std::min(N - 1, std::max(0, static_cast<int>(t * static_cast<float>(N))));
                const int autoIdx = chainBones[static_cast<size_t>(targetLocal)];
                SceneVertex& v = scene.meshes[static_cast<size_t>(hv.mesh)].vertices[hv.vert];
                int oldBones[4]; float oldW[4]; Vec3 oldL[4], oldNL[4];
                for (int k = 0; k < 4; ++k) { oldBones[k] = v.bone[k]; oldW[k] = v.weight[k]; oldL[k] = v.local[k]; oldNL[k] = v.nlocal[k]; }
                bool bad = false;
                for (int k = 0; k < 4; ++k) if (oldW[k] > 0 && (oldBones[k] < 0 || static_cast<size_t>(oldBones[k]) >= scene.bones.size())) bad = true;
                if (bad) continue;
                const Mat4 invAuto = inverse(scene.bones[static_cast<size_t>(autoIdx)].world);
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += oldW[k];
                if (!(sum > 1e-6f)) continue;
                const float scale = (1.0f - wAuto) / sum;
                v.bone[0] = autoIdx; v.weight[0] = wAuto;
                v.local[0] = invAuto.point(v.pos); v.nlocal[0] = invAuto.dir(v.normal);
                for (int k = 0; k < 3; ++k) {
                    v.bone[k + 1] = oldBones[k]; v.weight[k + 1] = oldW[k] * scale;
                    v.local[k + 1] = oldL[k]; v.nlocal[k + 1] = oldNL[k];
                }
            }
        }
        if (partPatched) {
            ++partsPatched;
            if (log) log("auto cloth: " + part.id + " -> chains with bones (authored DynamicBone missing, hanging " + std::to_string(static_cast<int>(hanging.size())) + "/" + std::to_string(static_cast<int>(all.size())) + " verts)");
        }
    }
    if (log && chainsMade > 0) log("auto cloth: " + std::to_string(chainsMade) + " chain(s), " + std::to_string(bonesMade) + " bone(s) across " + std::to_string(partsPatched) + " part(s)");
    if (std::getenv("CARDBRIDGE_DEBUG_CLOTH") && chainsMade == 0) std::fprintf(stderr, "auto cloth: none (all hanging parts have authored chains or nothing hangs)\n");
    return chainsMade;
}

} // namespace cardbridge::assemble
