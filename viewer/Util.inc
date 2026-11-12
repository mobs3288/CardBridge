// viewer fragment: dialogs, settings, paths, library helpers (included in order by main.cpp)

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Slashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
double NowMs() {
    static const double perMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart) / 1000.0; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / perMs;
}
std::string SafeStem(std::string s) {
    for (char& c : s)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
            static_cast<unsigned char>(c) < 32)
            c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? std::string("character") : s;
}

std::string SettingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/viewer.json";
}
std::string ModCachePath() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/modindex.json";
}

// The folder the mod index was last built for (the cache stores it), so the viewer finds the mods without asking.
std::string CachedModsRoot() {
    try {
        Bytes b = fileio::ReadFile(ModCachePath());
        auto j = nlohmann::json::parse(std::string(b.begin(), b.end()));
        return Slashes(j.value("root", ""));
    } catch (...) { return {}; }
}

struct Settings {
    std::string library, mods;
    std::string exportDir;   // where exports go without asking; empty = ask every time
    std::string layers;      // an extra folder of shader layers (.hlsl)
    std::string gpu = "auto";   // "auto" (discrete GPU first), "hardware", "warp" (CPU), "software" (software renderer); next start
    std::string skin = "gpu";   // "gpu" (compute shader) or "cpu" (CPU skinning loop)
    bool noMods = false;     // the user chose "no mods folder": do not fall back to the one the index cache remembers
    std::string viewMode = "lit";   // the viewport diagnostic mode (spec S13): see exporter::ViewModeName/FromName
    // Automatic skin shading (garment of the body): independent settings, persisted here for new cards and the CLI:
    // how strong the cleavage is, how strong the fold under the breasts is, how far up the chest the cleavage shading
    // reaches, and how wide the cleavage line runs.
    float skinShade = 1.5f;          // cleavage shading strength (0 = off, up to 2)
    float skinShadeUnder = 1.0f;     // under-breast fold shading strength (0 = off, up to 2)
    float skinShadeTop = 1.2f;       // cleavage extent: 0 = none, 1 = the full breast line, 2 = toward the base of the neck
    float skinShadeWidth = 0.6f;     // cleavage width: 0.1 = hairline, 1 = the default, 2 = a broad gradient
    float skinAo = 0.85f;              // Koikatsu's own look baked into the skin (base body normal lighting + the detailId line mask); 0 = off
};
Settings LoadSettings() {
    Settings s;
    try {
        auto j = nlohmann::json::parse([] { Bytes b = fileio::ReadFile(SettingsPath()); return std::string(b.begin(), b.end()); }());
        s.library = j.value("library", "");
        s.mods = j.value("mods", "");
        s.exportDir = j.value("exportDir", "");
        s.layers = j.value("layers", "");
        s.gpu = j.value("gpu", "auto");
        s.skin = j.value("skin", "gpu");
        s.noMods = j.value("noMods", false);
        s.viewMode = j.value("viewMode", "lit");
        s.skinShade = static_cast<float>(j.value("skinShade", 1.5));
        s.skinShadeUnder = static_cast<float>(j.value("skinShadeUnder", 1.0));
        s.skinShadeTop = static_cast<float>(j.value("skinShadeTop", 1.2));
        s.skinShadeWidth = static_cast<float>(j.value("skinShadeWidth", 0.6));
        s.skinAo = static_cast<float>(j.value("skinAo", 0.85));
    } catch (...) {}
    return s;
}
void SaveSettings(const Settings& s) {
    try {
        fileio::CreateDirectories(SettingsPath().substr(0, SettingsPath().find_last_of('/')));
        nlohmann::json j;
        j["library"] = s.library;
        j["mods"] = s.mods;
        j["exportDir"] = s.exportDir;
        j["layers"] = s.layers;
        j["gpu"] = s.gpu;
        j["skin"] = s.skin;
        j["noMods"] = s.noMods;
        j["viewMode"] = s.viewMode;
        j["skinShade"] = s.skinShade;
        j["skinShadeUnder"] = s.skinShadeUnder;
        j["skinShadeTop"] = s.skinShadeTop;
        j["skinShadeWidth"] = s.skinShadeWidth;
        j["skinAo"] = s.skinAo;
        fileio::WriteTextFile(SettingsPath(), j.dump(2));
    } catch (...) {}
}

// ---- dialogs and first-run defaults ------------------------------------------------------------------------------------
std::string PickFolder(HWND owner, const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return {};
    wchar_t path[MAX_PATH];
    std::string r;
    if (SHGetPathFromIDListW(pidl, path)) r = Slashes(WideToUtf8(path));
    CoTaskMemFree(pidl);
    return r;
}

std::string PickCard(HWND owner) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = owner;
    of.lpstrFilter = L"Koikatsu cards (*.png)\0*.png\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = 1024;
    of.lpstrTitle = L"Open a Koikatsu character card";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&of) ? Slashes(WideToUtf8(file)) : std::string();
}

bool Exists(const std::string& p) { return GetFileAttributesW(Utf8ToWide(p).c_str()) != INVALID_FILE_ATTRIBUTES; }

// Where a library the viewer builds itself goes (beside its settings), so the single exe needs no folder of its own.
std::string DefaultLibraryDir() {
    const char* base = std::getenv("LOCALAPPDATA");
    return std::string(base ? Slashes(base) : ".") + "/CardBridge/library";
}

// The asset library normally sits in the CardBridge folder (library_full, else library): look from the exe upwards, then in the
// viewer's own folder under %LOCALAPPDATA%.
std::string FindLibrary() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string dir = Slashes(WideToUtf8(exe));
    for (int up = 0; up < 5; ++up) {
        dir = dir.substr(0, dir.find_last_of('/'));
        for (const char* sub : {"/cardbridge/library_full", "/library_full", "/cardbridge/library", "/library"})
            if (Exists(dir + sub + "/abdata") || Exists(dir + sub + "/library.json")) return dir + sub;
        if (dir.find('/') == std::string::npos) break;
    }
    const std::string own = DefaultLibraryDir();
    if (Exists(own + "/abdata")) return own;
    return {};
}

// Where the extra shader layers live: CARDBRIDGE_LAYERS, a layers folder in the library, then the repository's cardbridge/layers
// found from the exe upwards.
std::vector<std::string> LayerFolders(const std::string& library, const std::string& extra = std::string()) {
    std::vector<std::string> out;
    if (!extra.empty()) out.push_back(extra);
    if (const char* env = std::getenv("CARDBRIDGE_LAYERS")) out.push_back(Slashes(env));
    if (!library.empty()) out.push_back(library + "/layers");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string dir = Slashes(WideToUtf8(exe));
    for (int up = 0; up < 5; ++up) {
        dir = dir.substr(0, dir.find_last_of('/'));
        for (const char* sub : {"/cardbridge/layers", "/layers"})
            if (Exists(dir + sub)) out.push_back(dir + sub);
        if (dir.find('/') == std::string::npos) break;
    }
    return out;
}

