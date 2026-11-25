// Diagnostic: compare source Unity mesh geometry with the assembled scene mesh.
//   usage: geomcheck <card.png> <library> <mods> [mesh-name-substring]
#include "assemble/Assembler.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include "unity/UnityScene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
using namespace cardbridge;

static void dumpCsv(const char* path, const std::vector<Vec3>& v) {
    std::ofstream f(path);
    for (const auto& p : v) f << p.x << ',' << p.y << ',' << p.z << '\n';
}

struct Bounds { float x0=1e30f,x1=-1e30f,y0=1e30f,y1=-1e30f,z0=1e30f,z1=-1e30f; int n=0; };
static void add(Bounds& b, const Vec3& p) {
    b.x0=std::min(b.x0,p.x); b.x1=std::max(b.x1,p.x);
    b.y0=std::min(b.y0,p.y); b.y1=std::max(b.y1,p.y);
    b.z0=std::min(b.z0,p.z); b.z1=std::max(b.z1,p.z); ++b.n;
}
static void show(const char* tag, const Bounds& b) {
    std::fprintf(stderr, "%-28s n=%6d x=[%7.3f,%7.3f] y=[%7.3f,%7.3f] z=[%7.3f,%7.3f]\n",
                 tag, b.n, b.x0,b.x1,b.y0,b.y1,b.z0,b.z1);
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: geomcheck <card.png> <library> <mods> [mesh-substr]\n"); return 1; }
    const std::string want = argc > 4 ? argv[4] : "o_top_sailor01_03";
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr,"card: %s\n",e.what()); return 2; }
    std::vector<std::string> issues;
    std::string lib = argv[2], modsDir = argv[3];
    resolve::ListTable lists;
    library::CheckLibrary(lib, false);
    lists.loadBaseGame(library::AbdataRoot(lib), &issues);
    resolve::ModIndex::Stats st;
    auto mods = resolve::ModIndex::Build(modsDir, lib + "/modindex.json", &issues, &st, [](size_t,size_t){});
    auto resolver = std::make_unique<resolve::Resolver>(mods, lists, library::AbdataRoot(lib));
    assemble::AssembleOptions ao;
    ao.log = nullptr;
    assemble::Scene scene = assemble::Assemble(c, *resolver, ao);

    std::fprintf(stderr, "=== ASSEMBLED SCENE (mesh name contains '%s') ===\n", want.c_str());
    for (size_t i=0;i<scene.meshes.size();++i) {
        const auto& m=scene.meshes[i];
        if (m.name.find(want)==std::string::npos) continue;
        Bounds b; for (const auto& v:m.vertices) add(b,v.pos);
        char tag[128]; std::snprintf(tag,sizeof tag,"scene[%zu] %s",i,m.name.c_str());
        show(tag,b);
        { std::vector<Vec3> ps; for (const auto& v:m.vertices) ps.push_back(v.pos);
          dumpCsv(("out/_scene_" + m.name + ".csv").c_str(),ps);
          dumpCsv("out/_scene_verts.csv",ps); }
        const char* part=""; for (const auto& p:scene.parts) if (std::find(p.meshes.begin(),p.meshes.end(),(int)i)!=p.meshes.end()) { part=p.id.c_str(); break; }
        std::fprintf(stderr, "  part=%s verts=%zu submeshes=%zu reshapable=%d\n", part, m.vertices.size(), m.submeshes.size(), (int)m.reshapable);
        {
            std::set<int> bs;
            for (const auto& v:m.vertices) for(int k=0;k<4;++k) if(v.weight[k]>0 && v.bone[k]>=0) bs.insert(v.bone[k]);
            for (int bi:bs) if (bi<(int)scene.bones.size()) {
                const Mat4& w=scene.bones[bi].world;
                float cx=std::sqrt(w.m[0]*w.m[0]+w.m[1]*w.m[1]+w.m[2]*w.m[2]);
                float cy=std::sqrt(w.m[4]*w.m[4]+w.m[5]*w.m[5]+w.m[6]*w.m[6]);
                float cz=std::sqrt(w.m[8]*w.m[8]+w.m[9]*w.m[9]+w.m[10]*w.m[10]);
                std::fprintf(stderr,"    bone[%d] %-24s scale=(%.3f %.3f %.3f) pos=(%.3f %.3f %.3f)\n",bi,scene.bones[bi].name.c_str(),cx,cy,cz,w.translation().x,w.translation().y,w.translation().z);
            }
        }
        // vertex histogram along y and z
        std::map<int,int> hy, hz;
        for (const auto& v:m.vertices) { ++hy[(int)std::floor(v.pos.y*2)]; ++hz[(int)std::floor(v.pos.z*4)]; }
        std::fprintf(stderr,"  y hist:"); for (const auto& [k,n]:hy) std::fprintf(stderr," %g:%d",k/2.0f,n); std::fprintf(stderr,"\n");
        std::fprintf(stderr,"  z hist:"); for (const auto& [k,n]:hz) if(n>4) std::fprintf(stderr," %g:%d",k/4.0f,n); std::fprintf(stderr,"\n");
        for (size_t v=0;v<std::min<size_t>(m.vertices.size(),12);++v) {
            const auto& x=m.vertices[v];
            std::fprintf(stderr,"   v%zu pos=(%.3f %.3f %.3f) bone0=%d w0=%.3f bone1=%d w1=%.3f uv=(%.3f %.3f)\n",
                v,x.pos.x,x.pos.y,x.pos.z,x.bone[0],x.weight[0],x.bone[1],x.weight[1],x.uv.x,x.uv.y);
        }
    }

    std::fprintf(stderr,"=== CHAINS mentioning spinesk/tie ===\n");
    for (size_t c=0;c<scene.chains.size();++c) {
        const auto& ch=scene.chains[c];
        bool hit=false; for(int b:ch.bones) if(b>=0&&b<(int)scene.bones.size()&&scene.bones[b].name.find("spinesk")!=std::string::npos) hit=true;
        if(!hit) continue;
        std::fprintf(stderr,"  chain %zu name='%s' own=%d part=%s bones:",c,ch.name.c_str(),(int)ch.characterOwn,ch.part.c_str());
        for(int b:ch.bones) if(b>=0&&b<(int)scene.bones.size()) std::fprintf(stderr," %s",scene.bones[b].name.c_str());
        std::fprintf(stderr,"\n");
    }
    std::fprintf(stderr,"=== SOURCE BUNDLES (mesh name contains '%s') ===\n", want.c_str());
    // every resolved item's bundle; open each once
    std::vector<std::string> tried;
    if (const char* extra = std::getenv("GEOMCHECK_BUNDLE")) {
        std::string why; auto uf = resolver->openBaseBundle(extra,&why);
        if (!uf) std::fprintf(stderr,"  extra bundle %s failed: %s\n",extra,why.c_str());
        else {
            std::fprintf(stderr,"  extra bundle %s meshes:",extra);
            for (const auto* o : uf->serialized().ofClass(unity::cls::Mesh))
                std::fprintf(stderr," '%s'",uf->serialized().peekName(*o).c_str());
            std::fprintf(stderr,"\n");
            for (const auto* o : uf->serialized().ofClass(unity::cls::Mesh)) {
                std::string nm = uf->serialized().peekName(*o);
                if (nm.find(want)==std::string::npos) continue;
                unity::PPtr p{}; p.pathId=o->pathId;
                auto mesh=uf->readMesh(p,&why);
                if (!mesh) { std::fprintf(stderr,"  readMesh failed %s: %s\n",nm.c_str(),why.c_str()); continue; }
                Bounds b; for (const auto& v:mesh->positions) add(b,v);
                std::fprintf(stderr,"  EXTRA bundle=%s mesh=%s verts=%zu\n",extra,nm.c_str(),mesh->positions.size());
                show("    source",b);
                dumpCsv(("out/_source_" + nm + ".csv").c_str(),mesh->positions);
                dumpCsv("out/_source_verts.csv",mesh->positions);
                // bind pose vs inverse scene bone world (for the first few bind entries)
                for (size_t b=0;b<std::min<size_t>(mesh->bindPoses.size(),6);++b) {
                    const Mat4& bp=mesh->bindPoses[b];
                    std::fprintf(stderr,"    bind[%zu] col scales=(%.3f %.3f %.3f) t=(%.3f %.3f %.3f)\n",b,
                        std::sqrt(bp.m[0]*bp.m[0]+bp.m[1]*bp.m[1]+bp.m[2]*bp.m[2]),
                        std::sqrt(bp.m[4]*bp.m[4]+bp.m[5]*bp.m[5]+bp.m[6]*bp.m[6]),
                        std::sqrt(bp.m[8]*bp.m[8]+bp.m[9]*bp.m[9]+bp.m[10]*bp.m[10]),
                        bp.m[3],bp.m[7],bp.m[11]);
                }
                std::map<int,int> hy;
                for (const auto& v:mesh->positions) ++hy[(int)std::floor(v.y*2)];
                std::fprintf(stderr,"    y hist:"); for (const auto& [k,n]:hy) std::fprintf(stderr," %g:%d",k/2.0f,n); std::fprintf(stderr,"\n");
                for (size_t v=0;v<std::min<size_t>(mesh->positions.size(),12);++v) {
                    const Vec3& x=mesh->positions[v];
                    std::fprintf(stderr,"    v%zu pos=(%.3f %.3f %.3f)\n",v,x.x,x.y,x.z);
                }
            }
        }
    }
    for (const auto& ref : resolve::ItemsFromCard(c, ao.outfit<0?0:ao.outfit)) {
        auto hi = resolver->resolve(c, ref);
        if (!hi.ok()) continue;
        std::string why; auto uf = resolver->openBundle(hi,&why);
        if (!uf) continue;
        if (std::find(tried.begin(),tried.end(),uf->name())!=tried.end()) continue;
        tried.push_back(uf->name());
        bool any=false;
        for (const auto* o : uf->serialized().ofClass(unity::cls::Mesh))
            if (uf->serialized().peekName(*o).find(want)!=std::string::npos) any=true;
        if (!any) continue;
        for (const auto* o : uf->serialized().ofClass(unity::cls::Mesh)) {
            const std::string nm = uf->serialized().peekName(*o);
            if (nm.find(want)==std::string::npos) continue;
            unity::PPtr p{}; p.fileId=0; p.pathId=o->pathId;
            auto mesh=uf->readMesh(p,&why);
            if (!mesh) { std::fprintf(stderr,"  readMesh failed %s: %s\n",nm.c_str(),why.c_str()); continue; }
            Bounds b; for (const auto& v:mesh->positions) add(b,v);
            std::fprintf(stderr,"  bundle=%s mesh=%s verts=%zu\n",uf->name().c_str(),nm.c_str(),mesh->positions.size());
            show("    source",b);
            std::map<int,int> hy;
            for (const auto& v:mesh->positions) ++hy[(int)std::floor(v.y*2)];
            std::fprintf(stderr,"    y hist:"); for (const auto& [k,n]:hy) std::fprintf(stderr," %g:%d",k/2.0f,n); std::fprintf(stderr,"\n");
            for (size_t v=0;v<std::min<size_t>(mesh->positions.size(),12);++v) {
                const Vec3& x=mesh->positions[v];
                std::fprintf(stderr,"    v%zu pos=(%.3f %.3f %.3f)\n",v,x.x,x.y,x.z);
            }
        }
    }
    return 0;
}
