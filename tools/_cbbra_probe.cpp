// Scratch diagnostic for the bra / breast-cover artifact investigation.
// Dumps part/mesh/submesh/material/texture data and counts geometrically
// duplicate triangles. Not committed; built against build/libcardbridge_core.a.
#include "../src/card/CardFile.h"
#include "../src/resolve/Resolver.h"
#include "../src/resolve/ModIndex.h"
#include "../src/resolve/ListTable.h"
#include "../src/library/Library.h"
#include "../src/assemble/Assembler.h"
#include "../src/core/FileIo.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

static std::string shaderDefaultFor(const assemble::Scene& s, const assemble::SceneMaterial& m, const char* prop) {
    if (m.shaderIndex < 0 || (size_t)m.shaderIndex >= s.shaders.size()) return "<no-shader>";
    for (const auto& [n, v] : s.shaders[(size_t)m.shaderIndex].texDefaults)
        if (n == prop) return v;
    return "<none>";
}

static void dumpPart(const assemble::Scene& s, const assemble::ScenePart& p) {
    std::printf("=== PART %-22s group=%-11s status=%-14s attach=%s bundle=%s\n",
        p.id.c_str(), p.group.c_str(), p.status.c_str(), p.attachNode.c_str(), p.bundle.c_str());
    size_t totalTris = 0, totalVerts = 0;
    for (int mi : p.meshes) {
        if (mi < 0 || (size_t)mi >= s.meshes.size()) continue;
        const auto& m = s.meshes[(size_t)mi];
        totalVerts += m.vertices.size();
        size_t tris = 0;
        for (const auto& sm : m.submeshes) tris += sm.indices.size() / 3;
        totalTris += tris;
        Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
        for (const auto& v : m.vertices) {
            lo.x = std::min(lo.x, v.pos.x); lo.y = std::min(lo.y, v.pos.y); lo.z = std::min(lo.z, v.pos.z);
            hi.x = std::max(hi.x, v.pos.x); hi.y = std::max(hi.y, v.pos.y); hi.z = std::max(hi.z, v.pos.z);
        }
        std::printf("  MESH[%d] %-40s verts=%zu tris=%zu submeshes=%zu bounds=(%.4f,%.4f,%.4f)-(%.4f,%.4f,%.4f)\n",
            mi, m.name.c_str(), m.vertices.size(), tris, m.submeshes.size(),
            lo.x,lo.y,lo.z, hi.x,hi.y,hi.z);
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            const auto& sm = m.submeshes[si];
            int mat = sm.material;
            std::printf("    SUB[%zu] tris=%zu mat=%d ", si, sm.indices.size()/3, mat);
            if (mat < 0 || (size_t)mat >= s.materials.size()) { std::printf("<INVALID>\n"); continue; }
            const auto& mt = s.materials[(size_t)mat];
            std::string texName = "<none>";
            if (mt.texture >= 0 && (size_t)mt.texture < s.textures.size()) texName = s.textures[(size_t)mt.texture].name;
            std::printf("name=%-34s shader=%-34s tex=%d(%s) size=%s diff=(%.2f,%.2f,%.2f,%.2f) alpha=%d dbl=%d hidden=%d default(_MainTex)=%s shaderIdx=%d\n",
                mt.name.c_str(), mt.shader.c_str(), mt.texture, texName.c_str(),
                [&]{ char b[64]; if (mt.texture>=0 && (size_t)mt.texture<s.textures.size()) std::snprintf(b,sizeof b,"%dx%d",s.textures[(size_t)mt.texture].width,s.textures[(size_t)mt.texture].height); else std::snprintf(b,sizeof b,"-"); return std::string(b); }().c_str(),
                mt.diffuse.x,mt.diffuse.y,mt.diffuse.z,mt.diffuse.w, mt.alpha?1:0, mt.doubleSided?1:0, mt.hidden?1:0,
                shaderDefaultFor(s, mt, "_MainTex").c_str(), mt.shaderIndex);
            for (const auto& tb : mt.texProps)
                std::printf("       TEXPROP %-20s -> %d %s\n", tb.name.c_str(), tb.texture,
                    (tb.texture>=0 && (size_t)tb.texture<s.textures.size()) ? s.textures[(size_t)tb.texture].name.c_str() : "<none>");
        }
    }
    std::printf("  TOTAL verts=%zu tris=%zu\n", totalVerts, totalTris);
}

// Count triangles that are geometrically identical (same three world positions,
// ignoring winding). Reports duplicates across ALL submeshes of a mesh.
static void dupTriangles(const assemble::Scene& s, const assemble::SceneMesh& m) {
    struct Key { Vec3 p[3]; bool operator<(const Key& o) const {
        for (int i=0;i<3;++i){ if (p[i].x<o.p[i].x-1e-6f) return true; if (p[i].x>o.p[i].x+1e-6f) return false;
            if (p[i].y<o.p[i].y-1e-6f) return true; if (p[i].y>o.p[i].y+1e-6f) return false;
            if (p[i].z<o.p[i].z-1e-6f) return true; if (p[i].z>o.p[i].z+1e-6f) return false; }
        return false; } };
    std::map<Key,int> count;
    size_t tris = 0, degen = 0;
    for (const auto& sm : m.submeshes) for (size_t i=0;i+2<sm.indices.size();i+=3) {
        uint32_t ia=sm.indices[i], ib=sm.indices[i+1], ic=sm.indices[i+2];
        if (ia>=m.vertices.size()||ib>=m.vertices.size()||ic>=m.vertices.size()) continue;
        ++tris;
        Vec3 a=m.vertices[ia].pos,b=m.vertices[ib].pos,c=m.vertices[ic].pos;
        if ((a.x==b.x&&a.y==b.y&&a.z==c.z&&b.z==c.z) || (a.x==b.x&&a.y==b.y) || (b.x==c.x&&b.y==c.y) || (a.x==c.x&&a.y==c.y)) ++degen;
        Vec3 ps[3]={a,b,c};
        std::sort(ps,ps+3,[](const Vec3&x,const Vec3&y){ return x.x<y.x||(x.x==y.x&&(x.y<y.y||(x.y==y.y&&x.z<y.z))); });
        Key k; k.p[0]=ps[0]; k.p[1]=ps[1]; k.p[2]=ps[2];
        ++count[k];
    }
    size_t dups=0, extra=0;
    for (const auto& [k,c] : count) if (c>1) { ++dups; extra += (size_t)(c-1); }
    std::printf("    DUPCHECK %-34s tris=%zu distinct=%zu dup-tri-groups=%zu extra-copies=%zu near-degenerate=%zu\n",
        m.name.c_str(), tris, count.size(), dups, extra, degen);
}

int main(int argc, char** argv) {
    std::string cardPath = argc>1 ? argv[1] : "C:/Users/pepsi/Downloads/139306275_p3.png";
    std::string libRoot  = argc>2 ? argv[2] : "D:/Project K/cardbridge/library_full";
    std::string modsDir  = argc>3 ? argv[3] : "E:/Game/[ScrewThisNoise] Koikatsu BetterRepack RX21/mods";
    std::vector<std::string> issues;
    resolve::ModIndex mi = resolve::ModIndex::Build(modsDir, "", &issues);
    resolve::ListTable lists;
    lists.loadBaseGame(library::AbdataRoot(libRoot), &issues);
    resolve::Resolver resolver(mi, lists, library::AbdataRoot(libRoot));
    card::Card card = card::LoadCard(cardPath);
    assemble::AssembleOptions ao; ao.outfit = 0;
    assemble::Scene scene = assemble::Assemble(card, resolver, ao);
    std::printf("SCENE %zu parts %zu meshes %zu materials %zu textures\n",
        scene.parts.size(), scene.meshes.size(), scene.materials.size(), scene.textures.size());

    // Summary of all parts
    std::printf("\n===== PART SUMMARY =====\n");
    for (const auto& p : scene.parts) {
        size_t tris=0,verts=0; Vec3 lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
        for (int mi2 : p.meshes) { if (mi2<0||(size_t)mi2>=scene.meshes.size()) continue;
            const auto& m=scene.meshes[(size_t)mi2]; verts+=m.vertices.size();
            for (const auto& sm:m.submeshes) tris+=sm.indices.size()/3;
            for (const auto& v:m.vertices){lo.x=std::min(lo.x,v.pos.x);lo.y=std::min(lo.y,v.pos.y);lo.z=std::min(lo.z,v.pos.z);hi.x=std::max(hi.x,v.pos.x);hi.y=std::max(hi.y,v.pos.y);hi.z=std::max(hi.z,v.pos.z);} }
        std::printf("PART %-22s %-11s tris=%-7zu verts=%-7zu bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)\n",
            p.id.c_str(),p.group.c_str(),tris,verts,lo.x,lo.y,lo.z,hi.x,hi.y,hi.z);
    }

    // Detailed dump of the bra, top and any part with geometry near the bust.
    std::printf("\n===== DETAIL (bra/top/bust-region parts) =====\n");
    for (const auto& p : scene.parts) {
        bool interesting = p.id.find("bra")!=std::string::npos || p.id.find("top")!=std::string::npos;
        if (!interesting) continue;
        dumpPart(scene, p);
    }

    // Duplicate-triangle check on every mesh of every part (report only interesting ones + all with duplicates).
    std::printf("\n===== DUPLICATE TRIANGLE CHECK (all meshes) =====\n");
    for (const auto& m : scene.meshes) {
        bool interesting = m.name.find("bra")!=std::string::npos || m.name.find("top")!=std::string::npos;
        // We always run it; print only if interesting or duplicates found by re-running cheaply.
        dupTriangles(scene, m);
        (void)interesting;
    }

    std::printf("\n===== ISSUES =====\n");
    for (const auto& i : scene.issues) std::printf("[%s] %s: %s\n", i.severity.c_str(), i.part.c_str(), i.message.c_str());
    return 0;
}
