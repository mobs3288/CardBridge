#include "resolve/ListTable.h"

#include "core/FileIo.h"
#include "core/MsgPack.h"
#include "core/ZipArchive.h"
#include "unity/UnityScene.h"

#include <algorithm>

namespace cardbridge::resolve {

std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuote) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; }
            else if (c == '"') inQuote = false;
            else cur.push_back(c);
        } else if (c == '"') {
            inQuote = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

size_t ListTable::loadBaseGame(const std::string& abdataRoot, std::vector<std::string>* issues) {
    size_t added = 0;
    std::string dir = fileio::Join(abdataRoot, "list/characustom");
    for (const std::string& f : fileio::ListFiles(dir, {".unity3d"}, issues)) {
        try {
            auto bundle = unity::UnityBundle::Open(fileio::ReadFile(f), fileio::FileName(f));
            unity::UnityFile uf(bundle);
            for (const auto& [name, bytes] : uf.readTextAssets()) {
                Value doc = MsgPackParse(bytes);
                int category = static_cast<int>(doc.at("categoryNo").asInt());
                std::vector<std::string> keys;
                for (const auto& k : doc.at("lstKey").asArray()) keys.push_back(k.asStr());
                for (const auto& [idv, rowv] : doc.at("dictList").asMap()) {
                    ListItem it;
                    it.origin = ListItem::Origin::BaseGame;
                    it.category = category;
                    it.container = abdataRoot;
                    const auto& row = rowv.asArray();
                    for (size_t i = 0; i < keys.size() && i < row.size(); ++i) it.keys[keys[i]] = row[i].asStr();
                    it.id = idv.asInt();
                    it.name = it.get("Name");
                    auto [pos, inserted] = base_.emplace(std::make_pair(category, it.id), it);
                    if (inserted) ++added;
                }
            }
        } catch (const std::exception& e) {
            if (issues) issues->push_back(f + ": " + e.what());
        }
    }
    return added;
}

size_t ListTable::addMod(const std::string& zipPath, const std::string& guid, std::vector<std::string>* issues) {
    size_t added = 0;
    try {
        ZipArchive z(zipPath);
        for (const auto& e : z.entries()) {
            std::string lower = e.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.find("list/characustom/") == std::string::npos || lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".csv") != 0)
                continue;
            Bytes raw = z.read(e);
            size_t start = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
            std::string text(reinterpret_cast<const char*>(raw.data()) + start, raw.size() - start);
            std::vector<std::string> lines;
            size_t p = 0;
            while (p <= text.size()) {
                size_t q = text.find('\n', p);
                std::string ln = text.substr(p, q == std::string::npos ? std::string::npos : q - p);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                lines.push_back(ln);
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (lines.size() < 4) continue;
            int category = 0;
            try { category = std::stoi(SplitCsvLine(lines[0])[0]); } catch (...) { continue; }
            std::vector<std::string> keys = SplitCsvLine(lines[3]);
            for (size_t i = 4; i < lines.size(); ++i) {
                if (lines[i].empty()) continue;
                std::vector<std::string> row = SplitCsvLine(lines[i]);
                if (row.empty() || row[0].empty()) continue;
                ListItem it;
                it.origin = ListItem::Origin::Mod;
                it.category = category;
                it.container = zipPath;
                it.modGuid = guid;
                for (size_t k = 0; k < keys.size() && k < row.size(); ++k) it.keys[keys[k]] = row[k];
                try { it.id = std::stoll(row[0]); } catch (...) { continue; }
                it.name = it.get("Name");
                if (mods_[guid].emplace(std::make_pair(category, it.id), it).second) ++added;
            }
        }
    } catch (const std::exception& e) {
        if (issues) issues->push_back(zipPath + ": " + e.what());
    }
    return added;
}

const ListItem* ListTable::findBase(int category, int64_t id) const {
    auto it = base_.find({category, id});
    return it == base_.end() ? nullptr : &it->second;
}

const ListItem* ListTable::findMod(const std::string& guid, int category, int64_t id) const {
    auto m = mods_.find(guid);
    if (m == mods_.end()) return nullptr;
    auto it = m->second.find({category, id});
    return it == m->second.end() ? nullptr : &it->second;
}

} // namespace cardbridge::resolve
