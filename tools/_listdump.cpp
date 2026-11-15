// Scratch: dump base-game list rows (raw keys) for the given categories.
//   usage: listdump <library> <category...>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "core/FileIo.h"
#include "core/MsgPack.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: listdump <library> [category...]\n"); return 1; }
    std::string dir = std::string(argv[1]) + "/abdata/list/characustom";
    std::vector<std::string> issues;
    std::vector<int> cats;
    for (int i = 2; i < argc; ++i) cats.push_back(std::atoi(argv[i]));
    for (const std::string& f : fileio::ListFiles(dir, {".unity3d"}, &issues)) {
        try {
            auto bundle = unity::UnityBundle::Open(fileio::ReadFile(f), fileio::FileName(f));
            unity::UnityFile uf(bundle);
            for (const auto& [name, bytes] : uf.readTextAssets()) {
                Value doc = MsgPackParse(bytes);
                int category = static_cast<int>(doc.at("categoryNo").asInt());
                bool want = cats.empty();
                for (int c : cats) want = want || c == category;
                if (!want) continue;
                std::vector<std::string> keys;
                for (const auto& k : doc.at("lstKey").asArray()) keys.push_back(k.asStr());
                std::printf("==== category %d (%s) rows=%zu keys=%zu\n", category, f.c_str(), doc.at("dictList").asMap().size(), keys.size());
                size_t shown = 0;
                for (const auto& [idv, rowv] : doc.at("dictList").asMap()) {
                    const auto& row = rowv.asArray();
                    std::printf("id %lld: ", (long long)idv.asInt());
                    for (size_t i = 0; i < keys.size() && i < row.size(); ++i)
                        if (keys[i] == "KokanHide" || keys[i] == "OverBodyMask" || keys[i] == "OverBodyMaskAB" || keys[i] == "OverBraMask" || keys[i] == "OverBraMaskAB" || keys[i] == "NotBra" || keys[i] == "Name" || keys[i] == "StateType")
                            std::printf("%s=%s ", keys[i].c_str(), row[i].asStr().c_str());
                    std::printf("\n");
                    if (++shown >= 600) { std::printf("...\n"); break; }
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", f.c_str(), e.what());
        }
    }
    return 0;
}
