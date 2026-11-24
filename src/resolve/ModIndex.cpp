#include "resolve/ModIndex.h"

#include "core/Bytes.h"
#include "core/FileIo.h"
#include "core/ZipArchive.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

namespace cardbridge::resolve {

namespace {

// Text between <tag> and </tag>, trimmed; empty if absent. manifest.xml is a
// flat, machine-written file, so this is enough without an XML library.
std::string tagText(const std::string& xml, const std::string& tag) {
    auto a = xml.find("<" + tag + ">");
    if (a == std::string::npos) return {};
    a += tag.size() + 2;
    auto b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos) return {};
    std::string s = xml.substr(a, b - a);
    auto l = s.find_first_not_of(" \t\r\n");
    auto r = s.find_last_not_of(" \t\r\n");
    return l == std::string::npos ? std::string() : s.substr(l, r - l + 1);
}

bool readManifest(const std::string& path, ModEntry& e) {
    ZipArchive z(path);
    Bytes b = z.read("manifest.xml");
    size_t start = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    std::string xml(reinterpret_cast<const char*>(b.data()) + start, b.size() - start);
    e.guid = tagText(xml, "guid");
    e.name = tagText(xml, "name");
    e.version = tagText(xml, "version");
    e.author = tagText(xml, "author");
    // Sideloader manifest: <game>Koikatsu Sunshine</game> (one per supported
    // game). No tags = legacy mod, loaded by both games.
    e.games.clear();
    size_t gp = 0;
    while ((gp = xml.find("<game>", gp)) != std::string::npos) {
        const size_t end = xml.find("</game>", gp);
        if (end == std::string::npos) break;
        std::string v = xml.substr(gp + 6, end - (gp + 6));
        const size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) { v = v.substr(a, b - a + 1); if (!v.empty()) e.games.push_back(v); }
        gp = end + 7;
    }
    // <MaterialEditor><Shader Name="IBL_Shader_alpha" AssetBundle="chara/IBLShader_alpha.unity3d">: the shader the
    // game's Shader.Find would bind. Flat machine-written XML, so a tag scan is enough.
    e.shaders.clear();
    {
        size_t p = xml.find("<MaterialEditor");
        const size_t stop = p == std::string::npos ? std::string::npos : xml.find("</MaterialEditor>", p);
        auto attrOf = [&xml](size_t tagStart, size_t tagEnd, const char* name) {
            const std::string needle = std::string(name) + "=\"";
            size_t a = xml.find(needle, tagStart);
            if (a == std::string::npos || a >= tagEnd) return std::string{};
            a += needle.size();
            size_t b = xml.find('"', a);
            return b == std::string::npos ? std::string{} : xml.substr(a, b - a);
        };
        while (p != std::string::npos && p < stop) {
            size_t s = xml.find("<Shader ", p);
            if (s == std::string::npos || s >= stop) break;
            size_t end = xml.find('>', s);
            if (end == std::string::npos) break;
            const std::string n = attrOf(s, end, "Name"), b = attrOf(s, end, "AssetBundle");
            if (!n.empty() && !b.empty()) e.shaders.emplace_back(n, b);
            p = end + 1;
        }
    }
    e.listIds.clear();
    for (const auto& entry : z.entries()) {   // the (category, id) rows of the mod's character lists
        std::string lower = entry.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("list/characustom/") == std::string::npos || lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".csv") != 0) continue;
        Bytes raw = z.read(entry);
        size_t pos = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        int64_t category = -1;
        for (size_t line = 0; pos < raw.size(); ++line) {
            size_t end = pos;
            while (end < raw.size() && raw[end] != '\n') ++end;
            if (line == 0 || line >= 4) {
                size_t k = pos;
                int64_t v = 0;
                bool digits = false;
                for (; k < end && raw[k] >= '0' && raw[k] <= '9'; ++k) { v = v * 10 + (raw[k] - '0'); digits = true; }
                if (line == 0) { if (!digits) break; category = v; }
                else if (digits) { e.listIds.push_back(category); e.listIds.push_back(v); }
            }
            pos = end + 1;
        }
    }
    return !e.guid.empty();
}

} // namespace

ModIndex ModIndex::Build(const std::string& root, const std::string& cachePath, std::vector<std::string>* issues,
                         Stats* stats, const std::function<void(size_t, size_t)>& progress,
                         const std::function<bool()>& cancelled) {
    Stats st;
    std::vector<std::string> files = fileio::ListFiles(root, {".zipmod", ".zip"}, issues);
    st.zips = files.size();

    std::map<std::string, ModEntry> cached;   // by path
    if (!cachePath.empty() && fileio::Exists(cachePath)) {
        try {
            Bytes raw = fileio::ReadFile(cachePath);
            auto j = nlohmann::json::parse(raw.begin(), raw.end());
            if (j.value("version", 0) == 4)
                for (const auto& e : j["entries"]) {
                    ModEntry m;
                    m.path = e.value("path", "");
                    m.guid = e.value("guid", "");
                    m.name = e.value("name", "");
                    m.version = e.value("version", "");
                    m.author = e.value("author", "");
                    m.size = e.value("size", uint64_t(0));
                    m.mtime = e.value("mtime", int64_t(0));
                    m.listIds = e.value("ids", std::vector<int64_t>{});
                    if (e.contains("shaders") && e["shaders"].is_array())
                        for (const auto& s : e["shaders"])
                            if (s.is_array() && s.size() >= 2) m.shaders.emplace_back(s[0].get<std::string>(), s[1].get<std::string>());
                    m.games = e.value("games", std::vector<std::string>{});
                    cached[m.path] = m;
                }
        } catch (const std::exception&) {
            cached.clear();   // a damaged cache is just a cold start
        }
    }

    std::vector<ModEntry> results(files.size());
    std::vector<char> ok(files.size(), 0);
    std::mutex issueMutex;
    std::atomic<size_t> next{0}, done{0}, reused{0}, opened{0}, failed{0};
    std::atomic<bool> stop{false};
    auto worker = [&]() {
        while (true) {
            if (stop.load()) return;
            if (cancelled && cancelled()) { stop.store(true); return; }
            size_t i = next.fetch_add(1);
            if (i >= files.size()) return;
            const std::string& p = files[i];
            ModEntry e;
            e.path = p;
            try {
                e.size = fileio::FileSize(p);
                e.mtime = fileio::LastWriteTicks(p);
                auto it = cached.find(p);
                if (it != cached.end() && it->second.size == e.size && it->second.mtime == e.mtime) {
                    e = it->second;
                    ok[i] = 1;
                    ++reused;
                } else if (readManifest(p, e)) {
                    ok[i] = 1;
                    ++opened;
                } else {
                    ++failed;
                    std::lock_guard<std::mutex> lk(issueMutex);
                    if (issues) issues->push_back(p + ": manifest.xml has no <guid>");
                }
            } catch (const std::exception& ex) {
                ++failed;
                std::lock_guard<std::mutex> lk(issueMutex);
                if (issues) issues->push_back(p + ": " + ex.what());
            }
            results[i] = std::move(e);
            size_t d = ++done;
            if (progress && (d % 50 == 0 || d == files.size())) {
                std::lock_guard<std::mutex> lk(issueMutex);
                progress(d, files.size());
            }
            if (cancelled && cancelled()) { stop.store(true); return; }
        }
    };
    unsigned n = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (stop.load()) throw Cancelled{};

    ModIndex idx;
    nlohmann::json out;
    out["version"] = 4;
    out["root"] = root;
    auto& arr = out["entries"] = nlohmann::json::array();
    for (size_t i = 0; i < results.size(); ++i) {
        if (!ok[i]) continue;
        const ModEntry& e = results[i];
        auto& vec = idx.byGuid_[e.guid];
        vec.push_back(e);
        ++idx.count_;
        for (size_t k = 0; k + 1 < e.listIds.size(); k += 2) {
            auto& at = idx.byListId_[{static_cast<int>(e.listIds[k]), e.listIds[k + 1]}];
            if (at.empty() || at.back().first != e.guid || at.back().second != vec.size() - 1) at.emplace_back(e.guid, vec.size() - 1);
        }
        const size_t myIdx = vec.size() - 1;
        for (const auto& kv : e.shaders) idx.byShaderName_.emplace(kv.first, std::make_pair(e.guid, myIdx));
        nlohmann::json entry{{"path", e.path}, {"guid", e.guid}, {"name", e.name}, {"version", e.version},
                             {"author", e.author}, {"size", e.size}, {"mtime", e.mtime}, {"ids", e.listIds}};
        if (!e.shaders.empty()) {
            nlohmann::json sh = nlohmann::json::array();
            for (const auto& kv : e.shaders) sh.push_back({kv.first, kv.second});
            entry["shaders"] = std::move(sh);
        }
        if (!e.games.empty()) entry["games"] = e.games;
        arr.push_back(std::move(entry));
    }
    if (!cachePath.empty()) {
        try {
            fileio::WriteTextFile(cachePath, out.dump());
        } catch (const std::exception& ex) {
            if (issues) issues->push_back(std::string("could not write the mod index cache: ") + ex.what());
        }
    }
    st.reused = reused;
    st.opened = opened;
    st.failed = failed;
    if (stats) *stats = st;
    return idx;
}

std::vector<const ModEntry*> ModIndex::findByListId(int category, int64_t id) const {
    std::vector<const ModEntry*> out;
    auto it = byListId_.find({category, id});
    if (it == byListId_.end()) return out;
    for (const auto& [guid, idx] : it->second) {
        auto g = byGuid_.find(guid);
        if (g != byGuid_.end() && idx < g->second.size() && g->second[idx].compatibleWith(game_)) out.push_back(&g->second[idx]);
    }
    return out;
}

std::vector<const ModEntry*> ModIndex::allEntries() const {
    std::vector<const ModEntry*> out;
    out.reserve(count_);
    for (const auto& [guid, entries] : byGuid_)
        for (const ModEntry& e : entries) out.push_back(&e);
    return out;
}

const std::vector<ModEntry>* ModIndex::find(const std::string& guid) const {
    auto it = byGuid_.find(guid);
    return it == byGuid_.end() ? nullptr : &it->second;
}

const ModEntry* ModIndex::findShader(const std::string& name, std::string* bundle) const {
    auto it = byShaderName_.find(name);
    if (it == byShaderName_.end()) return nullptr;
    auto g = byGuid_.find(it->second.first);
    if (g == byGuid_.end() || it->second.second >= g->second.size()) return nullptr;
    const ModEntry& e = g->second[it->second.second];
    if (!e.compatibleWith(game_)) return nullptr;
    for (const auto& kv : e.shaders)
        if (kv.first == name) { if (bundle) *bundle = kv.second; return &e; }
    return nullptr;
}

std::string ModSourceFingerprint(const std::string& root, std::vector<std::string>* issues) {
    std::vector<std::string> files;
    if (!root.empty()) files = fileio::ListFiles(root, {".zipmod", ".zip"}, issues);
    std::sort(files.begin(), files.end());
    std::string fp;
    for (const auto& p : files) {
        fp += p;
        fp += '|';
        try {
            fp += std::to_string(fileio::FileSize(p));
            fp += '|';
            fp += std::to_string(fileio::LastWriteTicks(p));
        } catch (const std::exception&) {
            fp += '?';   // a file that vanished mid-scan must look "changed"
        }
        fp += ';';
    }
    return fp;
}

const ModIndex& ModIndexCache::get(const std::string& root, const std::string& cachePath,
                                   std::vector<std::string>* issues, ModIndex::Stats* stats,
                                   const Progress& progress, const Cancelled& cancelled) {
    std::lock_guard<std::mutex> lk(mu_);   // a concurrent caller waits, then reuses what this build produced
    if (stats) *stats = ModIndex::Stats{};
    const library::GameInfo game = index_.game();   // the card's game is not an indexing dependency

    if (root.empty()) {
        if (!valid_ || root_ != root || cachePath_ != cachePath) {
            index_ = ModIndex{};
            index_.setGame(game);
            root_ = root;
            cachePath_ = cachePath;
            fingerprint_.clear();
            valid_ = true;
        }
        return index_;
    }

    // Don't pass the caller's `issues` here: Build reports the same problems,
    // and a fingerprint that could not stat a file simply forces a rebuild.
    const std::string fp = ModSourceFingerprint(root, nullptr);
    if (valid_ && root_ == root && cachePath_ == cachePath && fingerprint_ == fp) return index_;

    ModIndex built = ModIndex::Build(root, cachePath, issues, stats, progress, cancelled);
    built.setGame(game);
    index_ = std::move(built);
    root_ = root;
    cachePath_ = cachePath;
    fingerprint_ = fp;
    valid_ = true;
    ++builds_;
    return index_;
}

void ModIndexCache::invalidate(bool clearDiskCache, const std::string& cachePath) {
    std::lock_guard<std::mutex> lk(mu_);
    valid_ = false;   // the index stays in memory so any resolver still using it remains valid
    fingerprint_.clear();
    if (clearDiskCache && !cachePath.empty()) fileio::RemoveFile(cachePath);
}

} // namespace cardbridge::resolve
