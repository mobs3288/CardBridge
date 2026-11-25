// Diagnostic: compare BoneWorlds(scene, rest Pose) against scene.bones[].world.
#include "assemble/Assembler.h"
#include "assemble/Pose.h"
#include "card/CardFile.h"
#include "library/Library.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc,char**argv){
    if(argc<4){std::fprintf(stderr,"usage: poseworld <card> <lib> <mods>\n");return 1;}
    card::Card c; try{c=card::LoadCard(argv[1]);}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 2;}
    std::vector<std::string> issues;
    resolve::ListTable lists; library::CheckLibrary(argv[2],false);
    lists.loadBaseGame(library::AbdataRoot(argv[2]),&issues);
    resolve::ModIndex::Stats st;
    auto mods=resolve::ModIndex::Build(argv[3],std::string(argv[2])+"/modindex.json",&issues,&st,[](size_t,size_t){});
    auto resolver=std::make_unique<resolve::Resolver>(mods,lists,library::AbdataRoot(argv[2]));
    assemble::AssembleOptions ao; ao.log=nullptr;
    auto scene=assemble::Assemble(c,*resolver,ao);
    assemble::Pose rest;
    auto w=assemble::BoneWorlds(scene,rest);
    int bad=0, badT=0; float maxd=0, maxT=0; int worst=-1, worstT=-1;
    for(size_t i=0;i<scene.bones.size()&&i<w.size();++i){
        const auto &a=scene.bones[i].world, &b=w[i];
        float d=0; for(int k=0;k<16;++k) d=std::max(d,std::fabs(a.m[k]-b.m[k]));
        if(d>1e-4f){++bad;if(d>maxd){maxd=d;worst=(int)i;}}
        Vec3 ta=a.translation(), tb=b.translation();
        float dt=std::sqrt((ta.x-tb.x)*(ta.x-tb.x)+(ta.y-tb.y)*(ta.y-tb.y)+(ta.z-tb.z)*(ta.z-tb.z));
        if(dt>1e-4f){++badT;if(dt>maxT){maxT=dt;worstT=(int)i;}}
    }
    std::fprintf(stderr,"bones=%zu matrix mismatches=%d max elem diff=%g worst=%d; TRANSLATION mismatches=%d max=%g worst=%d\n",scene.bones.size(),bad,maxd,worst,badT,maxT,worstT);
    if(worstT>=0) std::fprintf(stderr,"WORST-T %s real=(%.4f %.4f %.4f) pose=(%.4f %.4f %.4f)\n",scene.bones[worstT].name.c_str(),scene.bones[worstT].world.translation().x,scene.bones[worstT].world.translation().y,scene.bones[worstT].world.translation().z,w[worstT].translation().x,w[worstT].translation().y,w[worstT].translation().z);
    int nonIdentity3x3=0; float maxLin=0;
    for(size_t i=0;i<w.size();++i){
        const auto&m=w[i];
        float d=0;
        d=std::max(d,std::fabs(m.m[0]-1));d=std::max(d,std::fabs(m.m[5]-1));d=std::max(d,std::fabs(m.m[10]-1));
        for(int k:{1,2,4,6,8,9}) d=std::max(d,std::fabs(m.m[k]));
        if(d>1e-4f)++nonIdentity3x3; maxLin=std::max(maxLin,d);
    }
    std::fprintf(stderr,"pose-rest matrices with non-identity linear part: %d, max linear deviation %g\n",nonIdentity3x3,maxLin);
    auto skin=assemble::SkinMatrices(scene,rest);
    int skinBad=0; float skinMax=0;
    for(const auto&m:skin){float d=0;for(int k=0;k<16;++k)d=std::max(d,std::fabs(m.m[k]-(k%5==0?1.0f:0.0f)));if(d>1e-4f)++skinBad;skinMax=std::max(skinMax,d);}
    std::fprintf(stderr,"rest SkinMatrices non-identity: %d, max deviation %g\n",skinBad,skinMax);
    for(int i=0;i<(int)scene.bones.size()&&i<8;++i){
        const auto&a=scene.bones[i].world;const auto&b=w[i];
        std::fprintf(stderr,"  %-20s real t=(%.4f %.4f %.4f) pose t=(%.4f %.4f %.4f)\n",scene.bones[i].name.c_str(),a.m[3],a.m[7],a.m[11],b.m[3],b.m[7],b.m[11]);
    }
    if(worst>=0){
        const auto&a=scene.bones[worst].world;const auto&b=w[worst];
        std::fprintf(stderr,"WORST %s real=(%.4f %.4f %.4f | %.4f %.4f %.4f) pose=(%.4f %.4f %.4f | %.4f %.4f %.4f)\n",
            scene.bones[worst].name.c_str(),a.m[0],a.m[4],a.m[8],a.m[3],a.m[7],a.m[11],b.m[0],b.m[4],b.m[8],b.m[3],b.m[7],b.m[11]);
    }
    return 0;
}
