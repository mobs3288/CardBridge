// Scratch: dump the AssetBundle container table: path -> (fileId, classId of asset).
//   usage: ufcontainer <bundle>
#include "unity/UnityScene.h"
#include "unity/UnityBundle.h"
#include "unity/UnitySerialized.h"
#include "core/FileIo.h"
#include <cstdio>
#include <string>
#include <map>
using namespace cardbridge;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: ufcontainer <bundle>\n"); return 1; }
    auto bundle = unity::UnityBundle::Open(fileio::ReadFile(argv[1]), argv[1]);
    unity::UnityFile uf(bundle);
    for (const auto* o : uf.serialized().ofClass(142)) {   // AssetBundle
        Value v = uf.serialized().read(*o);
        for (const auto& e : v.at("m_Container").asArray()) {
            const std::string path = e.at("first").asStr();
            const Value& second = e.at("second");
            const int64_t fileId = second.at("fileID").asInt();
            const int64_t pathId = second.at("asset").at("m_PathID").asInt();
            // resolve the class of the asset from the shared file table if it's a local file
            int cls = -1;
            if (fileId == 0) {
                const unity::ObjectInfo* info = uf.serialized().find(pathId);
                if (info) cls = info->classId;
            }
            std::fprintf(stderr, "%-72s fileId=%lld pathId=%lld class=%d\n", path.c_str(), (long long)fileId, (long long)pathId, cls);
        }
    }
    return 0;
}
