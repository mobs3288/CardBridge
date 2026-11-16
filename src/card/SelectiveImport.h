// Selective clothing import: the user chooses, per component of an outfit source, what happens to
// the outfit on screen - Add, Replace, Keep Existing or Skip. Everything the whole-outfit import
// does (id translation, hair preservation, resolver rows, ABMX/MaterialEditor/KCOX/... payloads)
// happens here too, scoped to the components the user chose. The Replace All import (ApplyClothing)
// is untouched and stays the one-click path.
//
// The flow is transactional: the viewer previews on a copy of the character (nothing in the loaded
// card changes), and only an explicit Apply runs the merge on the real card. The merge itself
// never writes a card file to disk.

#pragma once

#include "card/CardFile.h"

#include <map>
#include <string>
#include <vector>

namespace cardbridge::card {

// What the user chose for one component.
enum class SelectiveAction {
    Replace,       // import the source's component, replacing the existing occupant of its slot
    Add,           // import the source's component alongside the existing one(s): it goes into its
                   // slot when that is free, otherwise into the first free accessory slot (relocated,
                   // nothing existing is deleted or overwritten); for clothes slots Add and Replace
                   // are the same operation (a slot holds one garment)
    KeepExisting,  // keep the character's component; do not import the source's
    Skip,          // do not import the source's component at all
    AddLayer,      // import the source's clothes component as an EXTRA garment on top of the existing one(s):
                   // the clothes slot is never vacated or overwritten; the layer rides along for PMX assembly
                   // (a clothing file still carries one garment per slot, so layers never reach a saved card);
                   // for accessory components this is not offered (accessories already stack via Add)
};
const char* SelectiveActionName(SelectiveAction a);

// One component an outfit source carries that the user can choose to import. Enumerated from the
// source alone; the existing counterpart (if any) is matched by slot - clothes slot i maps to
// clothes slot i, accessory slot i to accessory slot i - never by name (different cards name
// equivalent garments differently).
struct SelectiveComponent {
    std::string key;       // stable identifier: "clothes.<0..8>" or "acc.<NN>"
    int kind = 0;          // 0 = clothes slot, 1 = accessory slot
    int slot = -1;         // slot index (clothes 0..8; accessory 0..N)
    std::string slotName;  // "top" ... "shoes_outer", or "acc03"
    std::string category;  // "Tops", "Bottoms", "Innerwear", "Socks", "Shoes", "Gloves", "Accessories", "Other"
    std::string label;     // short display label ("Top", "Accessory 03"); the viewer decorates it with resolved names
    int64_t sourceId = 0;      // the item id in the source (author-slot space; files are translated on the way in)
    int64_t existingId = 0;    // the item id in the target's outfit (0 = none)
    bool sourceHair = false;   // the source's component is hair (a hair accessory)
    bool existingHair = false; // the target's counterpart is hair (preserved by default)
    bool hasExisting = false;  // the target's slot holds something
    SelectiveAction action = SelectiveAction::Replace;   // the suggested default
};

// The suggested default for a component: keep a hair counterpart, otherwise replace what is
// there, or add what is missing.
SelectiveAction SuggestedAction(const SelectiveComponent& c);

struct SelectiveDecisions {
    std::map<std::string, SelectiveAction> actions;   // component key -> action
};

// The component's effective action: the user's choice from `decisions` (missing keys fall back to
// the suggested default).
SelectiveAction ResolveAction(const SelectiveComponent& c, const SelectiveDecisions& d);

// What a decision set would do, in a fixed order (source order). For the summary line and the
// commit check: a plan with nothing to import must not touch the character.
struct SelectivePlan {
    int replaced = 0, added = 0, kept = 0, skipped = 0, layers = 0;
    struct Entry { std::string key, label; SelectiveAction action; };
    std::vector<Entry> entries;
    bool importsNothing() const { return replaced + added + layers == 0; }
    int total() const { return replaced + added + kept + skipped + layers; }
};

// Wholesale enumeration of an outfit source against the target's outfit. `outfit` is the target
// outfit index; -1 resolves to the target's current outfit. Empty source slots are not listed
// (there is nothing to import). Only the components the source carries are listed.
std::vector<SelectiveComponent> EnumerateSelectiveComponents(const Card& target, const ClothingFile& source, int outfit);
// A Coordinate PNG's coordinate as the source (the identity never travels; only its outfit does).
std::vector<SelectiveComponent> EnumerateSelectiveComponents(const Card& target, const Card& sourceCard, int sourceOutfit, int outfit);

SelectivePlan BuildSelectivePlan(const std::vector<SelectiveComponent>& components, const SelectiveDecisions& decisions);

// The merge: applies the chosen components onto `card`'s outfit in memory. Never writes a card
// file; the card on disk is untouched. Throws FormatError when the outfit index is out of range
// or the source is unreadable.
ApplyClothingReport ApplySelectiveClothing(Card& card, const ClothingFile& clothes,
                                           const SelectiveDecisions& decisions,
                                           const ApplyClothingOptions& opt);

// The outfit-source path generalized: detects the source from its bytes (an outfit file, or a
// Coordinate PNG whose coordinate is extracted first - identity, face, body, other coordinates
// and unrelated KKEx plugins never travel) and merges only the chosen components.
struct SelectiveOutfitReport {
    bool fromCoordinate = false;   // the source was a character card (a Coordinate PNG)
    int srcOutfit = -1;            // which coordinate of the source (Coordinate sources only)
    std::string sourceName;        // the source's own display name
    SelectivePlan plan;            // what was committed
    ApplyClothingReport apply;     // the underlying merge report
};
SelectiveOutfitReport ApplySelectiveOutfitSource(Card& card, const std::string& path,
                                                 const ApplyClothingOptions& opt,
                                                 const SelectiveDecisions& decisions);

} // namespace cardbridge::card
