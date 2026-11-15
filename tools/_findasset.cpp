// Scratch: resolve an asset name to (class, pathId) in a bundle.
//   usage: findasset <bundle> <name>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: findasset <bundle> <name>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) { std::fprintf(stderr, "cannot open bundle\n"); return 1; }
    unity::UnityFile uf(bundle);
    std::string want = argv[2];
    unity::PPtr a = uf.findAsset(want);
    unity::PPtr t = uf.findTexture(want);
    std::fprintf(stderr, "findAsset('%s') -> fileId=%lld pathId=%lld null=%d\n", want.c_str(), (long long)a.fileId, (long long)a.pathId, a.isNull() ? 1 : 0);
    std::fprintf(stderr, "findTexture('%s') -> fileId=%lld pathId=%lld null=%d\n", want.c_str(), (long long)t.fileId, (long long)t.pathId, t.isNull() ? 1 : 0);
    return 0;
}
