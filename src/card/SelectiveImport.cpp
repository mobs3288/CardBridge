#include "card/SelectiveImport.h"
#include "card/ClothingMerge.h"

#include <algorithm>
#include <cstdio>

namespace cardbridge::card {

namespace {

const char* kSlot[9] = {"top", "bottom", "bra", "shorts", "gloves", "pantyhose", "socks", "shoes_inner", "shoes_outer"};
const char* kCategory[9] = {"Tops", "Bottoms", "Innerwear", "Innerwear", "Gloves", "Socks", "Socks", "Shoes", "Shoes"};
const char* kLabel[9] = {"Top", "Bottom", "Bra", "Shorts", "Gloves", "Stockings / pantyhose", "Socks",
                         "Shoes (inner)", "Shoes (outer)"};

// Replaces member `key` of a clothes map with `value` (creates it when missing).
void replaceClothesField(Value& clothesMap, const char* key, const Value& value) {
    if (!clothesMap.isMap()) return;
    bool found = false;
    for (auto& kv : clothesMap.map())
        if (kv.first.isStr() && kv.first.asStr() == key) { kv.second = value; found = true; }
    if (!found) clothesMap.map().emplace_back(Value(std::string(key)), value);
}

// Writes `part` into slot `index` of a clothes map's "parts" array.
void setClothesPart(Value& clothesMap, size_t index, const Value& part) {
    if (!clothesMap.isMap()) return;
    for (auto& kv : clothesMap.map())
        if (kv.first.isStr() && kv.first.asStr() == "parts" && kv.second.isArray()) {
            auto& arr = kv.second.array();
            if (index < arr.size()) arr[index] = part;
            return;
        }
}

// The shared enumeration worker: the target's counterpart (if any) is matched by slot, never by
// name. Source ids are translated local -> author-slot (a file stores session-local ids).
std::vector<SelectiveComponent> enumerateSelectiveComponents(const Card& target, int targetOutfit,
                                                             const Value& sourceClothes, const Value& sourceAccessory,
                                                             const std::vector<ResolveRow>& sourceRows,
                                                             int sourceOutfit) {
    std::map<int64_t, int64_t> localToSlot;
    for (const auto& rr : sourceRows)
        if (rr.localSlot != 0) localToSlot[rr.localSlot] = rr.slot;
    auto translate = [&](int64_t id) -> int64_t {
        auto it = localToSlot.find(id);
        return it == localToSlot.end() ? id : it->second;
    };

    const Outfit& dst = target.outfits[static_cast<size_t>(targetOutfit)];
    std::vector<SelectiveComponent> out;

    const Value& cp = sourceClothes.at("parts");
    const Value& tp = dst.clothes.at("parts");
    for (size_t i = 0; i < cp.size() && i < 9; ++i) {
        const int64_t sid = cp.at(i).at("id").asInt(0);
        if (sid == 0) continue;   // the source carries nothing here: nothing to offer
        SelectiveComponent c;
        c.kind = 0;
        c.slot = static_cast<int>(i);
        c.key = "clothes." + std::to_string(i);
        c.slotName = kSlot[i];
        c.category = kCategory[i];
        c.label = kLabel[i];
        c.sourceId = translate(sid);
        const int64_t tid = tp.isArray() && i < tp.size() ? tp.at(i).at("id").asInt(0) : 0;
        c.existingId = tid;
        c.hasExisting = tid != 0;
        c.action = SuggestedAction(c);
        out.push_back(std::move(c));
    }

    const Value& ap = sourceAccessory.at("parts");
    const Value& tap = dst.accessory.at("parts");
    for (size_t i = 0; i < ap.size(); ++i) {
        const Value& p = ap.at(i);
        if (isEmptyAcc(p)) continue;   // an empty outfit slot is not a component
        SelectiveComponent c;
        c.kind = 1;
        c.slot = static_cast<int>(i);
        char num[8];
        std::snprintf(num, sizeof num, "%02zu", i);
        c.key = std::string("acc.") + num;
        c.slotName = std::string("acc") + num;
        c.category = "Accessories";
        c.label = std::string("Accessory ") + num;
        c.sourceId = translate(p.at("id").asInt(0));
        c.sourceHair = IsHairAccessoryPart(p, sourceRows, sourceOutfit, static_cast<int>(i));
        const Value& q = tap.isArray() && i < tap.size() ? tap.at(i) : Value::nil();
        if (!tap.isArray() || i >= tap.size() || isEmptyAcc(q)) {
            c.existingId = 0;
            c.hasExisting = false;
        } else {
            c.existingId = q.at("id").asInt(0);
            c.hasExisting = true;
            c.existingHair = IsHairAccessoryPart(q, target.rows, targetOutfit, static_cast<int>(i));
        }
        c.action = SuggestedAction(c);
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace

const char* SelectiveActionName(SelectiveAction a) {
    switch (a) {
    case SelectiveAction::Replace:      return "replace";
    case SelectiveAction::Add:          return "add";
    case SelectiveAction::KeepExisting: return "keep existing";
    case SelectiveAction::Skip:         return "skip";
    }
    return "?";
}

SelectiveAction SuggestedAction(const SelectiveComponent& c) {
    if (c.hasExisting && c.existingHair) return SelectiveAction::KeepExisting;   // never delete the character's hair by default
    if (c.hasExisting) return SelectiveAction::Replace;
    return SelectiveAction::Add;
}

SelectiveAction ResolveAction(const SelectiveComponent& c, const SelectiveDecisions& d) {
    const auto it = d.actions.find(c.key);
    return it == d.actions.end() ? SuggestedAction(c) : it->second;
}

std::vector<SelectiveComponent> EnumerateSelectiveComponents(const Card& target, const ClothingFile& source, int outfit) {
    int t = outfit;
    if (t < 0) t = target.status.isMap() ? static_cast<int>(target.status.at("coordinateType").asInt()) : 0;
    if (t < 0 || static_cast<size_t>(t) >= target.outfits.size()) t = 0;
    return enumerateSelectiveComponents(target, t, source.outfit.clothes, source.outfit.accessory, source.rows, source.srcCoordinate);
}

std::vector<SelectiveComponent> EnumerateSelectiveComponents(const Card& target, const Card& sourceCard, int sourceOutfit, int outfit) {
    int t = outfit;
    if (t < 0) t = target.status.isMap() ? static_cast<int>(target.status.at("coordinateType").asInt()) : 0;
    if (t < 0 || static_cast<size_t>(t) >= target.outfits.size()) t = 0;
    int s = sourceOutfit;
    if (s < 0) s = sourceCard.status.isMap() ? static_cast<int>(sourceCard.status.at("coordinateType").asInt()) : 0;
    if (s < 0 || static_cast<size_t>(s) >= sourceCard.outfits.size()) s = 0;
    const Outfit& so = sourceCard.outfits[static_cast<size_t>(s)];
    return enumerateSelectiveComponents(target, t, so.clothes, so.accessory, sourceCard.rows, s);
}

SelectivePlan BuildSelectivePlan(const std::vector<SelectiveComponent>& components, const SelectiveDecisions& decisions) {
    SelectivePlan plan;
    for (const auto& c : components) {
        const SelectiveAction a = ResolveAction(c, decisions);
        SelectivePlan::Entry e;
        e.key = c.key;
        e.label = c.label;
        e.action = a;
        plan.entries.push_back(std::move(e));
        switch (a) {
        case SelectiveAction::Replace:      ++plan.replaced; break;
        case SelectiveAction::Add:          ++plan.added; break;
        case SelectiveAction::KeepExisting: ++plan.kept; break;
        case SelectiveAction::Skip:         ++plan.skipped; break;
        }
    }
    return plan;
}

ApplyClothingReport ApplySelectiveClothing(Card& card, const ClothingFile& clothes, const SelectiveDecisions& decisions,
                                           const ApplyClothingOptions& opt) {
    if (!clothes.supported)
        throw FormatError(std::string("outfit file not supported: ") + clothes.supportNote);
    if (card.outfits.empty()) throw FormatError("the character has no outfits");
    int outfit = opt.outfit;
    if (outfit < 0) outfit = card.status.isMap() ? static_cast<int>(card.status.at("coordinateType").asInt()) : 0;
    if (outfit < 0 || static_cast<size_t>(outfit) >= card.outfits.size()) outfit = 0;

    ApplyClothingReport rep;
    rep.outfit = outfit;

    const std::vector<SelectiveComponent> components =
        EnumerateSelectiveComponents(card, clothes, outfit);
    std::map<std::string, SelectiveAction> plan;
    for (const auto& c : components) plan[c.key] = ResolveAction(c, decisions);
    auto actionOf = [&](int kind, int slot) -> SelectiveAction {
        char key[32];
        if (kind == 0) std::snprintf(key, sizeof key, "clothes.%d", slot);
        else std::snprintf(key, sizeof key, "acc.%02d", slot);
        const auto it = plan.find(key);
        return it == plan.end() ? SelectiveAction::Skip : it->second;   // a component the source no longer carries: not imported
    };
    std::set<int> importedClothes;
    for (const auto& c : components)
        if (c.kind == 0 && (plan[c.key] == SelectiveAction::Replace || plan[c.key] == SelectiveAction::Add)) importedClothes.insert(c.slot);

    // Hair slots are never touched (a clothing file has no hair; the coordinate keeps the character's).
    {
        rep.hairSlotsPreserved = 0;
        if (card.hair.isMap() && card.hair.at("parts").isArray())
            for (const auto& p : card.hair.at("parts").asArray())
                if (p.at("id").asInt(0) != 0) ++rep.hairSlotsPreserved;
    }

    // Files store mod ids as session-local ids; translate like the whole-file merge does.
    std::map<int64_t, int64_t> localToSlot;
    for (const auto& rr : clothes.rows)
        if (rr.localSlot != 0) localToSlot[rr.localSlot] = rr.slot;
    auto translate = [&](int64_t id) -> int64_t {
        auto it = localToSlot.find(id);
        return it == localToSlot.end() ? id : it->second;
    };

    Outfit& target = card.outfits[static_cast<size_t>(outfit)];

    // --- accessories first: they only read the target's outfit, and the summary needs their count ---
    const Value& charAcc = target.accessory;
    const Value& clothAcc = clothes.outfit.accessory;
    Value::Array charParts, clothParts;
    if (charAcc.isMap() && charAcc.at("parts").isArray()) charParts = charAcc.at("parts").asArray();
    if (clothAcc.isMap() && clothAcc.at("parts").isArray()) clothParts = clothAcc.at("parts").asArray();
    for (auto& p : clothParts)
        if (p.isMap() && p.at("id").asInt(0) != 0) setPartId(p, translate(p.at("id").asInt()));

    Value::Array merged = charParts;
    const size_t nCloth = clothParts.size();
    const Value empty = emptyAccPart(charAcc, clothAcc);
    while (merged.size() < nCloth) merged.push_back(empty);
    std::vector<int> clothToMerged(nCloth, -1);
    std::set<int> replacedAcc;   // merged slots that received an imported accessory (their stale paint is dropped)
    for (size_t i = 0; i < nCloth; ++i) {
        if (isEmptyAcc(clothParts[i])) continue;   // an empty outfit slot never clears the character
        const SelectiveAction a = actionOf(1, static_cast<int>(i));
        if (a != SelectiveAction::Add && a != SelectiveAction::Replace) continue;   // Keep/Skip: the imported piece stays out
        const bool charEmpty = i >= charParts.size() || isEmptyAcc(charParts[i]);
        int dest = static_cast<int>(i);
        if (a == SelectiveAction::Add && !charEmpty) {
            // Add preserves whatever already occupies the slot(s): the imported piece lands in the
            // first free slot (the transform travels with the part, so this is placement, not a loss).
            size_t j = merged.size();
            for (size_t k = 0; k < merged.size(); ++k)
                if (isEmptyAcc(merged[k])) { j = k; break; }
            if (j == merged.size()) merged.push_back(empty);
            dest = static_cast<int>(j);
            if (dest != static_cast<int>(i)) ++rep.accessoriesRelocated;
        }
        merged[static_cast<size_t>(dest)] = clothParts[i];
        clothToMerged[i] = dest;
        replacedAcc.insert(dest);
        ++rep.accessoriesAdded;
    }
    for (size_t i = 0; i < clothToMerged.size(); ++i)
        if (clothToMerged[i] >= 0) {
            char num[8];
            std::snprintf(num, sizeof num, "%02zu", i);
            rep.placedAccessorySlots[std::string("acc.") + num] = clothToMerged[i];
        }
    // Count every character hair accessory that is still there (kept in place or around an imported one).
    for (size_t i = 0; i < charParts.size(); ++i)
        if (!isEmptyAcc(charParts[i]) && IsHairAccessoryPart(charParts[i], card.rows, outfit, static_cast<int>(i))) ++rep.hairPreserved;

    // --- clothes: only the chosen slots take the file's; the kept slots keep the character's ---
    const Value targetClothes = target.clothes;   // deep copy: the write-back below replaces it
    Value newClothes = clothes.outfit.clothes;
    {
        const Value& oldParts = targetClothes.at("parts");
        translateOutfitClothesIds(newClothes, localToSlot, oldParts, rep);
    }
    rep.clothesReplaced = 0;   // re-counted over the imported slots only (kept slots' "replacements" never happened)
    {
        const Value& tParts = targetClothes.at("parts");
        for (size_t i = 0; i < 9; ++i) {
            if (importedClothes.count(static_cast<int>(i))) continue;
            if (tParts.isArray() && i < tParts.size()) setClothesPart(newClothes, i, tParts.at(i));   // a malformed target clothes offers nothing to keep: the (translated) source part stands
        }
        if (!importedClothes.count(0)) replaceClothesField(newClothes, "subPartsId", targetClothes.at("subPartsId"));   // the kept top keeps the character's sub-parts
        const Value& rp = newClothes.at("parts");
        for (size_t i = 0; i < rp.size() && i < 9; ++i)
            if (importedClothes.count(static_cast<int>(i))) {
                const int64_t oldId = tParts.isArray() && i < tParts.size() ? tParts.at(i).at("id").asInt(0) : -1;
                if (rp.at(i).at("id").asInt(0) != oldId) ++rep.clothesReplaced;
            }
    }
    const bool anythingImported = !importedClothes.empty() || rep.accessoriesAdded > 0;

    // Write the clothes (identity when nothing clothes-related was imported).
    target.clothes = std::move(newClothes);

    // Write the accessory list back into the target outfit, keeping its version.
    if (target.accessory.isMap()) {
        for (auto& kv : target.accessory.map())
            if (kv.first.isStr() && kv.first.asStr() == "parts") kv.second = Value(merged);
    } else {
        Value::Map acc;
        acc.emplace_back(Value(std::string("version")), clothAcc.at("version"));
        acc.emplace_back(Value(std::string("parts")), Value(merged));
        target.accessory = Value(std::move(acc));
    }

    // --- makeup: follow the file's own switch, only when something was imported ---
    if (anythingImported && clothes.outfit.enableMakeup) {
        target.enableMakeup = true;
        target.makeup = clothes.outfit.makeup;
    }

    if (!anythingImported) {
        rep.notes.emplace_back("selective: nothing was selected for import; the character was not modified");
        // provenance (hasClothing / clothingPartIds) is left untouched
        return rep;
    }

    // --- resolver rows: drop only what was replaced/imported, add the file's rows for those ---
    const std::string prefix = "outfit" + std::to_string(outfit) + ".";
    {
        std::vector<ResolveRow> keptRows;
        for (const auto& rr : card.rows) {
            bool drop = false;
            if (rr.property.rfind(prefix, 0) == 0 && rr.property.find("ChaFileClothes.") != std::string::npos) {
                const int s = clothesSlotOfProperty(rr.property);
                if (s >= 0 && importedClothes.count(s)) drop = true;   // an unclassified clothes row is never silently dropped
            }
            if (!drop && rr.property.rfind(prefix, 0) == 0 && rr.property.find("ChaFileAccessory.") != std::string::npos) {
                const int idx = accessoryIndexOfProperty(rr.property);
                if (idx >= 0 && replacedAcc.count(idx)) drop = true;
            }
            if (!drop) keptRows.push_back(rr);
        }
        card.rows = std::move(keptRows);
        std::vector<ResolveRow> appended;
        for (const auto& rr : clothes.rows) {
            const bool isClothesRow = rr.property.find("ChaFileClothes.") != std::string::npos;
            const bool isAccRow = rr.property.find("ChaFileAccessory.") != std::string::npos;
            if (!isClothesRow && !isAccRow) continue;
            ResolveRow nr = rr;
            if (isClothesRow) {
                const int s = clothesSlotOfProperty(rr.property);
                if (s < 0 || !importedClothes.count(s)) continue;
                nr.property = withOutfitPrefix(rr.property, outfit);
            } else {
                const int orig = accessoryIndexOfProperty(rr.property);
                if (orig < 0 || static_cast<size_t>(orig) >= clothToMerged.size() || clothToMerged[static_cast<size_t>(orig)] < 0) continue;
                nr.property = prefix + "accessory" + std::to_string(clothToMerged[static_cast<size_t>(orig)]) + ".ChaFileAccessory.PartsInfo.id";
            }
            bool exists = false;
            for (const auto& e : card.rows) if (e.property == nr.property && e.slot == nr.slot) { exists = true; break; }
            if (!exists) {
                appended.push_back(nr);
                card.rows.push_back(std::move(nr));
            }
        }
        // Keep the extended universalautoresolver payload consistent with the merged rows.
        syncExtendedResolvedRows(card, outfit, appended, &replacedAcc, &importedClothes);
    }
    // --- plugin payloads: only the components the user chose travel ---
    {
        // A real outfit file IS its one coordinate; a Coordinate PNG's synthesised file records which.
        const int srcC = clothes.singleCoordinateSource ? -1 : clothes.srcCoordinate;
        mergeAbmxPayloads(card, clothes.extended, rep.notes, clothes.srcCoordinate, outfit,
                          static_cast<int>(card.outfits.size()), /*coordinateMode=*/false);
        mergeMaterialEditorPayloads(card, clothes.extended, outfit, clothToMerged, srcC, replacedAcc,
                                    /*dropAllAccRows=*/false, rep.notes, &importedClothes);
        mergeKcoxPayloads(card, clothes.extended, srcC, outfit, rep.notes, &importedClothes, !replacedAcc.empty());
        mergeDynamicBonePayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                 /*coordinateImport=*/false, replacedAcc, rep.notes);
        mergeHairAccessoryPayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                   /*coordinateImport=*/false, replacedAcc, rep.notes);
        mergeAccessoryStatesPayloads(card, clothes.extended, srcC, outfit, clothToMerged,
                                     /*coordinateImport=*/false, rep.notes);
        mergePushupPayloads(card, clothes.extended, srcC, outfit, rep.notes);
        mergeAdditionalCardInfo(card, clothes.extended, srcC, clothes.srcOutfitCount, outfit,
                                static_cast<int>(card.outfits.size()), /*coordinateImport=*/false, rep.notes);
    }

    // --- added-clothing provenance: only the imported parts ---
    card.hasClothing = true;
    card.clothingName = clothes.name.empty() ? clothes.path : clothes.name;
    card.clothingPath = clothes.path;
    card.clothingOutfit = outfit;
    card.clothingPartIds.clear();
    {
        const Value& cp = target.clothes.at("parts");
        if (cp.isArray())
            for (size_t i = 0; i < cp.size() && i < 9; ++i) {
                if (!importedClothes.count(static_cast<int>(i))) continue;
                if (cp.at(i).at("id").asInt(0) == 0) continue;
                card.clothingPartIds.insert("outfit" + std::to_string(outfit) + "." + kSlot[i]);
            }
        for (size_t i = 0; i < clothToMerged.size(); ++i) {
            const int mergedSlot = clothToMerged[i];
            if (mergedSlot < 0) continue;
            char num[8];
            std::snprintf(num, sizeof num, "%02d", mergedSlot);
            card.clothingPartIds.insert("outfit" + std::to_string(outfit) + ".acc" + num);
        }
    }
    rep.notes.emplace_back("selective: " + std::to_string(rep.clothesReplaced) + " clothes slot(s), " +
                           std::to_string(rep.accessoriesAdded) + " accessory(ies) imported" +
                           (rep.accessoriesRelocated ? ", " + std::to_string(rep.accessoriesRelocated) + " relocated around the character's own" : ""));
    return rep;
}

SelectiveOutfitReport ApplySelectiveOutfitSource(Card& card, const std::string& path, const ApplyClothingOptions& opt,
                                                 const SelectiveDecisions& decisions) {
    SelectiveOutfitReport out;
    const ImageSourceKind kind = DetectImageSource(path);
    int dst = opt.outfit;
    if (dst < 0) dst = card.status.isMap() ? static_cast<int>(card.status.at("coordinateType").asInt()) : 0;
    if (dst < 0 || static_cast<size_t>(dst) >= card.outfits.size()) dst = 0;
    if (kind == ImageSourceKind::Coordinate) {
        // A Coordinate PNG (a character card used as an outfit source): extract the outfit only,
        // then run the selective merge over it. Identity, face, body, other coordinates and
        // unrelated KKEx plugins never travel.
        Card src = LoadCard(path);
        if (!src.supported)
            throw FormatError(std::string("the coordinate source is not supported: ") + src.supportNote);
        if (src.outfits.empty()) throw FormatError("the coordinate source has no coordinates");
        ClothingFile cl = ExtractClothingFromCard(src, opt.srcOutfit);
        out.fromCoordinate = true;
        out.srcOutfit = cl.srcCoordinate;
        out.sourceName = cl.name;
        out.plan = BuildSelectivePlan(EnumerateSelectiveComponents(card, cl, dst), decisions);
        ApplyClothingOptions copt;
        copt.outfit = dst;
        copt.srcOutfit = cl.srcCoordinate;
        out.apply = ApplySelectiveClothing(card, cl, decisions, copt);
        sexMismatchNotes(src, card, cl.srcCoordinate, out.apply.notes);
        return out;
    }
    if (kind == ImageSourceKind::RegularClothing) {
        const ClothingFile cl = LoadClothingFile(path);
        if (!cl.supported)
            throw FormatError(cl.supportNote.empty() ? std::string("outfit file not supported") : cl.supportNote);
        out.sourceName = cl.name.empty() ? path : cl.name;
        out.srcOutfit = -1;
        out.plan = BuildSelectivePlan(EnumerateSelectiveComponents(card, cl, dst), decisions);
        ApplyClothingOptions copt;
        copt.outfit = dst;
        copt.srcOutfit = -1;
        out.apply = ApplySelectiveClothing(card, cl, decisions, copt);
        return out;
    }
    throw CardError(kind == ImageSourceKind::Invalid ? CardErrorKind::UnknownMarker : CardErrorKind::NoPayload,
                    "not an outfit source: " + path);
}

} // namespace cardbridge::card
