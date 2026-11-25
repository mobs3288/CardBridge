// Scratch: inspect the mnpb shader's texture defaults and forward render state.
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
using namespace cardbridge;
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: mnpbshader <bundle> <shader-name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) return 2;
    unity::UnityFile uf(bundle);
    for (const auto* o : uf.serialized().ofClass(48)) {
        Value v = uf.serialized().read(*o);
        std::string n = v.at("m_ParsedForm").at("m_Name").asStr();
        if (n.empty()) n = v.at("m_Name").asStr();
        if (n != argv[2]) continue;
        std::fprintf(stderr, "shader '%s' pathId=%lld\n", n.c_str(), (long long)o->pathId);
        for (const auto& [p, d] : uf.readShaderTextureDefaults(o->pathId))
            std::fprintf(stderr, "  tex %s = %s\n", p.c_str(), d.c_str());
        for (const auto& [p, d] : uf.readShaderPropDefaults(o->pathId))
            std::fprintf(stderr, "  prop %s = (%.3f %.3f %.3f %.3f)\n", p.c_str(), d.x, d.y, d.z, d.w);
        unity::ShaderRenderState rs;
        std::fprintf(stderr, "  renderstate ok=%d cull=%d cullProp='%s' zwrite=%d queue=%d\n",
                     (int)uf.readShaderRenderState(o->pathId, rs), rs.cull, rs.cullProperty.c_str(), (int)rs.zWrite, rs.queue);
    }
    return 0;
}
