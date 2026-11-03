#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cardbridge::library {

// The CardBridge asset library: a folder holding the base-game data the
// converter needs (skeleton and body, item list tables, and optionally the
// base-game item bundles), laid out like the game's own `abdata` folder:
//
//     <library>/library.json
//     <library>/abdata/chara/oo_base.unity3d
//     <library>/abdata/list/characustom/*.unity3d
//     ...
//
// At run time CardBridge reads ONLY this folder (plus the card and the user's
// mods) - never the game install. ImportLibrary is the one place that reads a
// game install, once, to copy files in.
//
// The library holds copies of the game's data files, which belong to their
// authors: it is kept out of the repository and is the library owner's
// responsibility to share or not.

enum class Profile {
    Minimal,   // skeleton, body, "none" prefabs and list tables only (~5 MB): items must come from mods
    Full,      // plus every base-game item bundle in abdata/chara (~0.9 GB)
};

struct ImportOptions {
    std::string gameRoot;      // folder containing abdata/
    std::string libraryRoot;   // destination
    Profile profile = Profile::Full;
};

struct ImportResult {
    size_t files = 0, copied = 0, unchanged = 0;
    uint64_t bytes = 0;
    std::vector<std::string> issues;
};

// The game's colour-grading table (AmplifyColor's "Lut_Amount", a 1024x32 strip) lives in one of the map scene bundles. This finds it and
// writes <library>/colorgrade/lut_amount.png, which the viewer and the exporter use to match the character maker's colours (ImportLibrary
// calls it, so a library made by `library import` has it). False, with the reason, when no bundle holds the table.
bool ExtractColourGrade(const std::string& gameRoot, const std::string& libraryRoot, std::string* why = nullptr);

// The all-in-one release carries the library inside the exe: a zip (entries "library/...") appended to it, then a 24-byte footer
// ("CBLIBPK1", the zip's offset and size as little-endian 64-bit numbers). True when `exePath` has one.
bool HasEmbeddedLibrary(const std::string& exePath);
// Unpacks it into `destParent` (the entries start with "library/", so the library ends up in destParent/library). Files already there with
// the right size are left alone, so an interrupted unpack resumes. False, with the reason, on failure.
bool ExtractEmbeddedLibrary(const std::string& exePath, const std::string& destParent, const std::function<void(size_t, size_t)>& progress, std::string* why = nullptr);

ImportResult ImportLibrary(const ImportOptions& opt, const std::function<void(size_t, size_t)>& progress = {});

struct CheckResult {
    bool ok = false;
    std::string profile;
    size_t files = 0;
    std::vector<std::string> issues;
};

// Verifies library.json against the files on disk (presence, size, FNV-1a hash).
CheckResult CheckLibrary(const std::string& libraryRoot, bool verifyHashes);

// <library>/abdata - what the resolver is given.
std::string AbdataRoot(const std::string& libraryRoot);

} // namespace cardbridge::library
