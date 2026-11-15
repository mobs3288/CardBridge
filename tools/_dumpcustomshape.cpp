// Scratch: dump the text assets of a Unity bundle (the body / face shape tables).
//   usage: dumpcustomshape <bundle> [name-filter]
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: dumpcustomshape <bundle> [name-filter]\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    if (!bundle) { std::fprintf(stderr, "cannot open bundle\n"); return 1; }
    unity::UnityFile uf(bundle);
    for (const auto& [name, bytes] : uf.readTextAssets()) {
        if (argc > 2 && name.find(argv[2]) == std::string::npos) continue;
        std::printf("---- %s (%zu bytes) ----\n", name.c_str(), bytes.size());
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
        std::printf("\n");
    }
    return 0;
}
