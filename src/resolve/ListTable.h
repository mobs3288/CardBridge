#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cardbridge::resolve {

// One row of a character list table: an item the character maker offers
// (a hair style, a jacket, an accessory ...). Rows come from the base game's
// MessagePack `ChaListData` tables or from a zipmod's .csv lists; the keys
// (MainAB, MainData, Parent, ...) are the table's own column names.
struct ListItem {
    enum class Origin { BaseGame, Mod };
    Origin origin = Origin::BaseGame;
    int category = 0;
    int64_t id = 0;
    std::string name;
    std::map<std::string, std::string> keys;
    std::string container;   // abdata folder (base) or the .zipmod path (mod)
    std::string modGuid;     // mods only
    std::string get(const std::string& key) const {
        auto it = keys.find(key);
        return it == keys.end() ? std::string() : it->second;
    }
};

class ListTable {
public:
    // Reads abdata/list/characustom/*.unity3d. Returns the number of rows added.
    size_t loadBaseGame(const std::string& abdataRoot, std::vector<std::string>* issues);
    // Reads the list .csv files of one zipmod. Returns the number of rows added.
    size_t addMod(const std::string& zipPath, const std::string& guid, std::vector<std::string>* issues);

    const ListItem* findBase(int category, int64_t id) const;
    const ListItem* findMod(const std::string& guid, int category, int64_t id) const;
    size_t baseCount() const { return base_.size(); }

private:
    std::map<std::pair<int, int64_t>, ListItem> base_;
    std::map<std::string, std::map<std::pair<int, int64_t>, ListItem>> mods_;
};

// CSV line splitter with double-quote handling (names may contain commas).
std::vector<std::string> SplitCsvLine(const std::string& line);

} // namespace cardbridge::resolve
