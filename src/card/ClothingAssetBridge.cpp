#include "card/ClothingAssetBridge.h"

#include "core/FileIo.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace cardbridge::card {
namespace {

int64_t translateId(int64_t id, const std::vector<ResolveRow>& rows) {
    for (const auto& rr : rows)
        if (rr.localSlot != 0 && rr.localSlot == id) return rr.slot;
    return id;
}

bool isTextureColumn(const std::string& key) {
    return key.find("Tex") != std::string::npos || key.find("Mask") != std::string::npos;
}

} // namespace

bool EnumerateClothingAssets(const ClothingFile& clothing, const ClothingAssetOptions& options,
                             ClothingAssetResult& out, std::string& error) {
    out = ClothingAssetResult{};
    if (!clothing.supported) { error = clothing.supportNote.empty() ? "unsupported clothing file" : clothing.supportNote; return false; }
    if (options.libraryRoot.empty()) { error = "no asset library configured"; return false; }
    try {
        const library::GameInfo info = library::ReadGameInfo(options.libraryRoot);
        std::vector<std::string> issues;
        resolve::ModIndex mods = resolve::ModIndex::Build(options.modsRoot, options.modCachePath, &issues);
        mods.setGame(info);
        resolve::ListTable lists;
        lists.loadBaseGame(library::AbdataRoot(options.libraryRoot), &issues);
        resolve::Resolver resolver(mods, lists, library::AbdataRoot(options.libraryRoot), info);

        static const char* slotName[9] = {"top", "bottom", "bra", "shorts", "gloves", "pantyhose", "socks", "shoes_inner", "shoes_outer"};
        static const char* propName[9] = {"ClothesTop", "ClothesBot", "ClothesBra", "ClothesShorts", "ClothesGloves",
                                          "ClothesPants", "ClothesSocks", "ClothesShoesInner", "ClothesShoesOuter"};
        static const int categoryNo[9] = {105, 106, 107, 108, 109, 110, 111, 112, 112};

        const Value& cp = clothing.outfit.clothes.at("parts");
        for (size_t i = 0; i < cp.size() && i < 9; ++i) {
            const int64_t id = translateId(cp.at(i).at("id").asInt(0), clothing.rows);
            if (!id) continue;
            resolve::ItemRef ref;
            ref.category = categoryNo[i];
            ref.id = id;
            ref.property = std::string("ChaFileClothes.") + propName[i];
            ref.slot = static_cast<int>(i);
            ref.partId = std::string("clothes.") + slotName[i];
            ref.label = slotName[i];
            const resolve::ResolvedItem ri = resolver.resolve(clothing.rows, ref);
            if (!ri.ok()) { out.warnings.push_back(ref.partId + ": " + ri.detail); continue; }
            // An empty/none slot can still carry a nonzero list id whose row
            // points at the game's dummy prefab. Skip placeholders instead of
            // trying to graft them.
            const std::string assetName = ri.listItem.get("MainData");
            const std::string lowerAsset = [&] { std::string s = assetName; for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }();
            if (assetName.empty() || lowerAsset.find("dummy") != std::string::npos ||
                ri.listItem.get("Name").empty() || ri.listItem.get("Name") == "なし") continue;
            library::AssetRecord rec;
            rec.key = std::string(info.name()) + "|" + std::to_string(ref.category) + "|" + std::to_string(id) + "|" +
                      (ri.modGuid.empty() ? std::string("base") : ri.modGuid);
            rec.game = info.game;
            rec.categoryNo = ref.category;
            rec.category = library::AssetCategory::Clothing;
            rec.id = id;
            rec.name = ri.listItem.get("Name");
            rec.mod = ri.status == resolve::ItemStatus::ResolvedMod;
            rec.modGuid = ri.modGuid; rec.modName = ri.modName; rec.modVersion = ri.modVersion;
            rec.modAuthor = ri.modAuthor; rec.modPath = ri.modPath;
            rec.bundle = ri.bundleRel(); rec.asset = ri.assetName();
            rec.parent = ri.listItem.get("Parent");
            rec.thumbBundle = ri.listItem.get("ThumbAB");
            rec.thumbAsset = ri.listItem.get("ThumbTex");
            for (const auto& [k, v] : ri.listItem.keys)
                if (isTextureColumn(k) && !v.empty() && v != "0") rec.textureDeps.push_back(k + "=" + v);
            std::sort(rec.textureDeps.begin(), rec.textureDeps.end());
            ClothingAssetItem item;
            item.key = ref.partId;
            item.label = ref.label;
            item.record = std::move(rec);
            out.items.push_back(std::move(item));
        }

        const Value& ap = clothing.outfit.accessory.at("parts");
        for (size_t i = 0; i < ap.size(); ++i) {
            const Value& part = ap.at(i);
            const int64_t type = part.at("type").asInt(0);
            const int64_t id = translateId(part.at("id").asInt(0), clothing.rows);
            if (type == 120 || !id) continue;
            char num[8];
            std::snprintf(num, sizeof num, "%02zu", i);
            resolve::ItemRef ref;
            ref.category = static_cast<int>(type);
            ref.id = id;
            ref.property = "accessory" + std::to_string(i) + ".ChaFileAccessory.PartsInfo.id";
            ref.partId = std::string("acc.") + num;
            ref.label = std::string("Accessory ") + num;
            ref.accIndex = static_cast<int>(i);
            ref.attachNode = part.at("parentKey").asStr();
            const resolve::ResolvedItem ri = resolver.resolve(clothing.rows, ref);
            if (!ri.ok()) { out.warnings.push_back(ref.partId + ": " + ri.detail); continue; }
            // An empty/none slot can still carry a nonzero list id whose row
            // points at the game's dummy prefab. Skip placeholders instead of
            // trying to graft them.
            const std::string assetName = ri.listItem.get("MainData");
            const std::string lowerAsset = [&] { std::string s = assetName; for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }();
            if (assetName.empty() || lowerAsset.find("dummy") != std::string::npos ||
                ri.listItem.get("Name").empty() || ri.listItem.get("Name") == "なし") continue;
            library::AssetRecord rec;
            rec.key = std::string(info.name()) + "|" + std::to_string(ref.category) + "|" + std::to_string(id) + "|" +
                      (ri.modGuid.empty() ? std::string("base") : ri.modGuid);
            rec.game = info.game;
            rec.categoryNo = ref.category;
            rec.category = (type == 122 || type == 123) ? library::AssetCategory::Hair : library::AssetCategory::Accessories;
            rec.id = id;
            rec.name = ri.listItem.get("Name");
            rec.mod = ri.status == resolve::ItemStatus::ResolvedMod;
            rec.modGuid = ri.modGuid; rec.modName = ri.modName; rec.modVersion = ri.modVersion;
            rec.modAuthor = ri.modAuthor; rec.modPath = ri.modPath;
            rec.bundle = ri.bundleRel(); rec.asset = ri.assetName();
            rec.parent = ref.attachNode.empty() ? ri.listItem.get("Parent") : ref.attachNode;
            rec.thumbBundle = ri.listItem.get("ThumbAB");
            rec.thumbAsset = ri.listItem.get("ThumbTex");
            for (const auto& [k, v] : ri.listItem.keys)
                if (isTextureColumn(k) && !v.empty() && v != "0") rec.textureDeps.push_back(k + "=" + v);
            std::sort(rec.textureDeps.begin(), rec.textureDeps.end());
            ClothingAssetItem item;
            item.key = ref.partId;
            item.label = ref.label;
            item.record = std::move(rec);
            out.items.push_back(std::move(item));
        }
        out.warnings.insert(out.warnings.end(), issues.begin(), issues.end());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace cardbridge::card
