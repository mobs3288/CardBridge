// Internal header: the outfit-merge machinery shared by the clothing import (the whole-outfit
// merge in CardFile.cpp) and the selective import (SelectiveImport.cpp). Declarations only; the
// definitions live next to ApplyClothing in CardFile.cpp so both paths use one implementation.
// Not a public header: nothing here is installed or documented as API.

#pragma once

#include "card/CardFile.h"
#include "core/Value.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cardbridge::card {

// Maps a "outfitN.ChaFileClothes.<Slot>" (or "...Sub<A/B/C>") resolver-row property to the
// clothes slot it names: 0..8 for the nine slots (top 0 .. shoes_outer 8), 0 for the top's
// jacket/sailor sub-parts, -1 when the property is not a clothes-slot row.
int clothesSlotOfProperty(const std::string& property);

// The shared id/pattern translation of a clothes Value (clothes ids and the pattern slots of
// colorInfo entries are rebased local -> author slot, and subPartsId with them). Bumps
// `rep.clothesReplaced` for every slot whose id changed against `oldParts`. Identical work for
// the whole-file merge (every slot counts) and the selective merge (callers re-count afterwards).
void translateOutfitClothesIds(Value& newClothes, const std::map<int64_t, int64_t>& localToSlot,
                               const Value& oldParts, ApplyClothingReport& rep);

bool isEmptyAcc(const Value& part);
void setPartId(Value& part, int64_t id);
Value emptyAccPart(const Value& a, const Value& b);
int accessoryIndexOfProperty(const std::string& property);
std::string withOutfitPrefix(const std::string& property, int outfit);

// The universalautoresolver bin re-sync. `replacedAcc` (when not nullptr) names the merged
// accessory slots whose rows are dropped; nullptr drops every accessory row of the outfit.
// `clothesSlots` (when not nullptr) restricts the clothes-row drop to those slots;
// nullptr drops every clothes row of the outfit (the whole-outfit behaviour).
void syncExtendedResolvedRows(Card& card, int dstOutfit, const std::vector<ResolveRow>& appended,
                              const std::set<int>* replacedAcc, const std::set<int>* clothesSlots = nullptr);

void mergeAbmxPayloads(Card& card, const Value& srcExtended, std::vector<std::string>& notes,
                       int srcCoordinate, int dstOutfit, int dstOutfits, bool coordinateMode);

void mergeMaterialEditorPayloads(Card& card, const Value& srcExtended, int dstOutfit,
                                 const std::vector<int>& clothToMerged, int srcCoordinate,
                                 const std::set<int>& replacedAcc, bool dropAllAccRows,
                                 std::vector<std::string>& notes,
                                 const std::set<int>* clothesSlots = nullptr);

// `importedClothesSlots` (when not nullptr) keeps the target coordinate's garment paint for
// garments that were NOT imported (the selective import); nullptr replaces the whole
// coordinate's overlays with the source's (the whole-outfit behaviour). `accessoryImported`
// governs the overlays whose garment is not a clothes garment (almost always accessory paint);
// it is ignored when importedClothesSlots is nullptr.
void mergeKcoxPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                       std::vector<std::string>& notes,
                       const std::set<int>* importedClothesSlots = nullptr,
                       bool accessoryImported = true);

void mergeAccessoryStatesPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                                  const std::vector<int>& clothToMerged, bool coordinateImport,
                                  std::vector<std::string>& notes);

void mergeDynamicBonePayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                              const std::vector<int>& clothToMerged, bool coordinateImport,
                              const std::set<int>& replacedAcc,
                              std::vector<std::string>& notes);

void mergeHairAccessoryPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                                const std::vector<int>& clothToMerged, bool coordinateImport,
                                const std::set<int>& replacedAcc,
                                std::vector<std::string>& notes);

void mergePushupPayloads(Card& card, const Value& srcExtended, int srcCoordinate, int dstOutfit,
                         std::vector<std::string>& notes);

void mergeAdditionalCardInfo(Card& card, const Value& srcExtended, int srcCoordinate, int srcOutfits, int dstOutfit,
                             int dstOutfits, bool coordinateImport, std::vector<std::string>& notes);

void sexMismatchNotes(const Card& src, const Card& dst, int srcCoordinate, std::vector<std::string>& notes);

} // namespace cardbridge::card
