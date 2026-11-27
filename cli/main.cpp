// cardbridge - Koikatsu character card to PMX, offline.
//
//   cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir> [options]
//   cardbridge inspect <card.png> [--library <dir>] [--mods <dir>] [--json]
//   cardbridge measure <card.png> [--library <dir>] [--mods <dir>] [--json]
//   cardbridge transfer <reference.png> <target.png> <out.json> [--select height,head,bust,...] [--library <dir>] [--mods <dir>]
//   cardbridge clothes <card.png> [--outfit N] [--name <name>] [--out <file.png>] [options]
//   cardbridge library import --game <install> --library <dir> [--minimal]
//   cardbridge library check  --library <dir> [--hashes]
//   cardbridge pmx info <model.pmx> [--json]
//   cardbridge pmx roundtrip <in.pmx> <out.pmx>
//
// Only `library import` ever reads a game install. Every other command works
// from the card, the CardBridge library folder and the user's mods folder.

#include "core/Version.h"
#include "shader/Dxbc.h"
#include "shader/UnityShader.h"
#include "shader/Sm4.h"
#include <cstring>
#include <cctype>
#include <nlohmann/json.hpp>
#include "assemble/Assembler.h"
#include "assemble/JiggleSettings.h"
#include "assemble/Pose.h"
#include "assemble/Proportions.h"
#include "assemble/ProportionTransfer.h"
#include "measure/TransferMeasure.h"
#include "card/Abmx.h"
#include "card/CardFile.h"
#include "card/ClothingExport.h"
#include "core/FileIo.h"
#include "core/MemoryProbe.h"
#include "pmx/PmxReader.h"
#include "pmx/PmxBodyRestore.h"
#include "pmx/PmxAssetApply.h"
#include "pmx/PmxToScene.h"
#include "pmx/PmxTransform.h"
#include "assemble/KkBodyTemplate.h"
#include "core/Timing.h"
#include "export/ColourGrade.h"
#include "export/Exporter.h"
#include "export/ModelExport.h"
#include "material/MaterialWorkbench.h"
#include <stb_image_write.h>

#include "export/GpuView.h"
#include "export/SkinBench.h"
#include "export/OutfitIcon.h"
#include "export/SkeletonOverlay.h"
#include "export/Preview.h"
#include "export/BakeQuality.h"
#include "export/TextureUpscaler.h"
#include "export/SkinShade.h"
#include "export/SkinAoBake.h"
#include "mesh/Simplify.h"
#include "library/Library.h"
#include "library/AssetCatalog.h"
#include "resolve/ListTable.h"
#include "resolve/ModIndex.h"
#include "resolve/Resolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace cardbridge;

namespace {

constexpr const char* kVersion = CARDBRIDGE_VERSION;

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opt;
    std::vector<std::string> flags;
    bool has(const std::string& f) const { return std::find(flags.begin(), flags.end(), f) != flags.end(); }
    std::string get(const std::string& k, const std::string& def = {}) const {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    }
};

Args parseArgs(int argc, char** argv, const std::vector<std::string>& boolFlags) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string key = s.substr(2);
            if (std::find(boolFlags.begin(), boolFlags.end(), key) != boolFlags.end()) a.flags.push_back(key);
            else if (i + 1 < argc) a.opt[key] = argv[++i];
            else a.opt[key] = "";
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

// Preset first, then an explicit --bake-size overrides just the resolution. Keeping the two
// separate means --bake-quality stays a one-knob choice while --bake-size remains an escape hatch.
bool BakeQualityFromArgs(const Args& a, exporter::BakeQuality& out, std::string& error) {
    const std::string preset = a.get("bake-quality", "standard");
    if (!exporter::ResolveBakePreset(preset, out)) {
        error = "unknown --bake-quality '" + preset + "' (expected standard, high or ultra)";
        return false;
    }
    if (a.opt.count("bake-size")) {
        const std::string s = a.get("bake-size");
        char* end = nullptr;
        const long n = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || n <= 0) {
            error = "--bake-size must be a positive integer (64..8192)";
            return false;
        }
        out.maxSize = exporter::ClampBakeSize(static_cast<int>(n));
    }
    return true;
}

#ifdef _WIN32
// argv arrives in the ANSI code page; re-read the command line as UTF-16 so
// Japanese or bracketed paths survive.
std::vector<std::string> utf8Args() {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(s);
    }
    LocalFree(w);
    return out;
}
#endif

void usage() {
    std::puts(
        "cardbridge " CARDBRIDGE_VERSION " - Koikatsu character card to PMX, offline\n"
        "\n"
        "  cardbridge convert <card.png> --library <dir> --mods <dir> --out <dir>\n"
        "             [--format pmx|glb|gltf|obj]        export target (default: pmx, unchanged). glb/gltf/obj are WIP:\n"
        "                                             they consume the same assembled character, in metres, Y-up, but are\n"
        "                                             still being hardened in Blender; no game or Blender needed at export.\n"
        "             [--no-textures] [--no-armature] [--no-skinning] [--no-morphs] [--no-mtl] [--combine-objects]\n"
        "                                             format-specific export options (ignored for pmx)\n"
        "  cardbridge measure <card.png> [--library <dir>] [--mods <dir>] [--json]\n"
        "                                             print the 14 transferable body measurements of one card\n"
        "  cardbridge transfer <reference.png> <target.png> <spec.json> [--select height,head,bust,...] [--refine]\n"
        "                                             --refine opts into slow measured correction (off by default)\n"
        "                                             measure both cards and write a transfer spec for --transfer\n"
        "             [--outfit N]                    which outfit of the card to convert (default: the card's current one)\n"
        "             [--clothes <file.png>]          an outfit source applied to that outfit in memory for this\n"
        "                                             export only; the card on disk is never saved. A regular outfit file\n"
        "                                             (KKCLOTHING) merges as usual; a Koikatsu Coordinate PNG (a character\n"
        "                                             card used as an outfit source) is detected and its outfit contents\n"
        "                                             (clothes, accessories, makeup) are extracted first - the source's\n"
        "                                             identity, face, body and personality are never applied. Hair worn\n"
        "                                             as accessories is kept (see --replace-accessories)\n"
        "             [--replace-accessories]         replace the outfit's accessories wholesale (may delete hair carried\n"
        "                                             as accessories; off by default, hair is always kept)\n"
        "             [--apose-degrees N] [--tpose]   the rest pose has the arms N degrees down from the T-pose (40 by default,\n"
        "                                             what a MikuMikuDance motion expects); --tpose keeps the T-pose\n"
        "             [--pose <file.json>]            a pose saved by the viewer (bone edits and the A-pose switch), baked in\n"
        "             [--materials <file.json>]      a material workbench file (cardbridge-materials v1), applied to the export\n"
        "             [--unity-facing]                the front toward +Z as built (the PMX default is -Z, MikuMikuDance's front)\n"
        "             [--transfer <spec.json>]        apply a measured proportion-transfer spec written by `cardbridge transfer`.\n"
        "                                             The target keeps its face, hair, clothes, textures and accessories; only the\n"
        "                                             selected body measurements change. Tracked in the manifest.\n"
        "             [--proportions <file.json>]     apply an optional corrective layer (cardbridge-proportions v1:\n"
        "                                             {\"enabled\": true, \"ops\": [{\"kind\": 0|1|2, \"index\": N, \"value\": F, \"bone\": \"name\", \"scale\": [x,y,z], \"label\": \"...\", \"why\": \"...\", \"safety\": 0|1|2, \"scope\": \"body|face|head\"}]}).\n"
        "                                             kind 0 = body slider (0..43), 1 = face slider (0..51), 2 = bone scale.\n"
        "             [--normalize-head-proportions] [--normalize-body-proportions] [--normalize-face-proportions] [--normalize-overall]\n"
        "                                             generate and apply the corresponding proportion suggestions (optional;\n"
        "                                             a starting point, not a recommendation - review with the viewer). Without\n"
        "                                             these flags the export is faithful to the card. Tracked in the manifest.\n"
        "             [--print-diagnostics]          print the proportion diagnostics for this card (head/body/face sliders and ABMX)\n"
        "             [--no-mmd-names]                keep the rig's own bone names; by default the body bones get MMD's\n"
        "                                             standard (Japanese) names so a motion (VMD) finds them\n"
        "             [--no-physics]                  no rigid bodies or joints: hair and skirt stay rigid, nothing can shiver\n"
        "             [--auto-cloth]                  opt-in: synthesize loose chains for hanging cloth missing authored physics\n"
        "                                            (disabled by default: synthesised chains were collapsing cloth onto the body;\n"
        "                                            --no-auto-cloth / CARDBRIDGE_NO_AUTO_CLOTH=1 keep it off)\n"
        "             [--fix-garment-intersections]   turn the garment-vs-body clipping correction ON (it is OFF by default: the export keeps\n"
        "                                            the card's own garment vertices, exactly the drape the viewer shows). The correction\n"
        "                                            pushes any garment vertex that pierces the body back out along the body's own surface by\n"
        "                                            its own penetration depth (plus a small clearance); the silhouette is preserved. Body,\n"
        "                                            head, hair, the nude-morph skin pieces and footwear are never touched; the corrected count\n"
        "                                            is written to the manifest as issues. CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=1 also turns\n"
        "                                            it on; =0 forces it off. ([--no-fix-garment-intersections] is kept for compatibility: the default)\n"
        "             [--no-jiggle-bust] [--jiggle-butt] [--jiggle-preset subtle|very-subtle|very-very-subtle|natural|dynamic] [--sag F]\n"
        "                                             [--jiggle-mass F] [--jiggle-stiffness F] [--jiggle-damping F]\n"
        "                                             [--jiggle-gravity F] [--jiggle-amplitude-vertical F]\n"
        "                                             [--jiggle-amplitude-horizontal F] [--jiggle-amplitude-depth F]\n"
        "                                             [--jiggle-rotation-strength F] [--jiggle-strength F] [--jiggle-sim-hz F]\n"
        "                                             breast jiggle is on by default. The butt jiggle is OFF by default:\n"
        "                                             the card's butt sway is large and swings behind the skirt's back panel so\n"
        "                                             the whole region reads as vibrating. [--jiggle-butt] enables it for a card\n"
        "                                             that wants it; [--no-jiggle-butt] is accepted and does the same thing (default).\n"
        "             [--no-bra-physics]            Bra-Aware Breast Physics is ON by default: a confidently detected supportive bra\n"
        "                                             gets a firmer, better damped breast profile; no bra, an ordinary shirt, an\n"
        "                                             uncertain garment or this switch leave the existing physics exactly unchanged.\n"
        "                                             CARDBRIDGE_NO_BRA_PHYSICS=1 is the environment equivalent.\n"
        "                                             The jiggle is a real\n"
        "                                             mass-spring-damper, one set of physical numbers driving both the preview and\n"
        "                                             the PMX. The preset picks a tuned set (default: natural); the --jiggle-* values\n"
        "                                             override it one by one; 0 amplitude locks that axis and strength scales the\n"
        "                                             whole effect (--jiggle-intensity is the old name for --jiggle-strength);\n"
        "                                             units: mass kg, stiffness N/m, damping N s/m, gravity x9.81 m/s^2, simulation_hz\n"
        "                                             is the fixed preview rate; sag scales the static hang (1 = the card's own, 0 = none, 2 = double)\n"
        "             [--height-cm F]               scale everything so the model stands F cm tall (measured head to feet)\n"
        "             [--simplify N]               reduce the written model's triangles by N percent (0-80; off by default), for PMX, glTF and OBJ. The face and\n"
        "                                             eyes are kept nearly as-is, the hair and body a little, the clothes and accessories\n"
        "                                             the most; UVs, material boundaries, bone weights and blend shapes are preserved\n"
        "             [--lods] [--lod-targets A,B,C,D]\n"
        "                                             also write <name>_lod1.pmx .. _lod4.pmx beside the main PMX (default targets:\n"
        "                                             60/30/15/5 percent of the triangles left, --lod-targets overrides, comma-separated,\n"
        "                                             e.g. --lod-targets 70,40,20,10). Each LOD is generated from the original geometry,\n"
        "                                             never from another level; they share the export's textures and are listed in the\n"
        "                                             manifest. PMX itself has no runtime LOD switching: pick a file per distance\n"
        "             [--skin-shade F]              automatic cleavage shading strength baked into the skin (0 = off,\n"
        "                                             1 = default, up to 2; breast region only)\n"
        "             [--skin-underboob F]          strength of the fold under the breasts (0 = off, 1 = default, up to 2)\n"
        "             [--skin-top F]                top extent of the cleavage: 0 = nothing, 1 = the full breast line (top of the\n"
        "                                             bust), 2 = a full bust length higher, toward the base of the neck; 0..2\n"
        "             [--skin-width F]              how wide the cleavage shading runs: 0.1 = a hairline, 1 = the default\n"
        "             [--skin-ao F]                 Koikatsu's own look baked into the skin (cavity AO from the base body normal map,\n"
        "                                             Koikatsu's ink lines from the card's body detailId; 0 = off, 1 = default, up to 2,\n"
        "                                             complements --skin-shade, never stacks with it\n"
        "             [--nipple-colour R,G,B]       nipple / areola tint, 0..255 each (default: a natural warm salmon-red)\n"
        "             [--card-nipples]              keep the card's own nipColor instead of the automatic pink tint\n"
        "             [--hide a,b] [--nude]          untick parts (outfit0.top,...) like the viewer's list: they are not\n"
        "                                             in the preview or the PMX at all, bones included; --nude ticks the\n"
        "                                             Nude body box (skin under the clothes shows)\n"
        "             [--nose-ridge F]                head-mod centre nose ridge shading: 1 = as authored (default), 0 = off, 2 = stronger\n"
        "             [--nose-ridge-opacity F]        independent opacity layer for that ridge: 0 = invisible, 1 = fully visible (default)\n"
        "             [--gpu auto|hardware|warp|software] [--gpu-skin gpu|cpu|auto]  which device the --gpu-view picture and the skinning use (auto prefers the best-suited hardware adapter; warp and software are the CPU fallbacks; skin auto uses the CPU below --skin-threshold verts)\n"
        "             [--skin-threshold N]           'auto' skin threshold in vertices (default 20000, or CARDBRIDGE_SKIN_THRESHOLD)\n"
         "             [--bake-quality standard|high|ultra]\n"
         "                                             shader-bake texture quality (default: standard). standard = 1024, one sample per\n"
         "                                             texel, byte-identical to older exports. high = 2048 with 2x internal supersampling,\n"
         "                                             island-safe padding and area-resampled shader maps. ultra = 4096 with the same\n"
         "                                             treatment; very large sources are capped internally so conversion stays practical.\n"
         "                                             Higher presets never upscale small source textures. Bake time and memory grow with\n"
         "                                             the preset; only high-res source detail can benefit.\n"
         "             [--bake-size N]                override just the baked texture resolution (64..8192), e.g. --bake-size 2048.\n"
         "             [--upscale N]                 upscale exported colour textures N times (2..8) with waifu2x-ncnn-vulkan (MIT).\n"
         "                                             Open source, GPU (Vulkan), optional: without the tool the export is unchanged.\n"
         "             [--upscaler <exe>]               path to waifu2x-ncnn-vulkan.exe (else CARDBRIDGE_UPSCALER,\n"
         "             [--upscale-model waifu2x|realesrgan]\n"
         "                                             which open-source model to use (default: waifu2x). realesrgan uses the\n"
         "                                             x4plus-anime model, usually sharper on line art but slower.\n"
         "                                             tools/upscaler/, or PATH). Toon and sphere maps are never upscaled.\n"
         "                                             Takes precedence over --bake-quality resolution; the other quality settings stay.\n"
        "  cardbridge skin-bench [--repeats N] [--gpu auto|hardware|warp]   CPU vs GPU skinning on synthetic workloads (10k/50k/150k/500k verts)\n"
        "  cardbridge simplify-bench [--subdiv N]   time the triangle reduction on a synthetic closed mesh (subdiv 6 = 81920 faces; the same path LODs use)\n"
        "  cardbridge clothes <card.png> [--outfit N] [--name <name>] [--out <file.png>]\n"
        "                                     [--thumbnail <file.png>] [--render-thumbnail] [--json] [--overwrite]\n"
        "                                     [--hide a,b,c] [--include-hidden] [--with-hair-accessories] [--with-abmx]\n"
        "                                             --with-hair-accessories keeps hair-type accessories (type 122/123) in the file\n"
        "                                             and shows the character's hair on the mannequin icon (off by default);\n"
        "                                             export one outfit of a character card as an outfit file (【KoiKatuClothes】\n"
        "                                             .png — the same file --clothes applies); default out: <card stem> - <outfit name>.png\n"
        "                                             next to the card; the card on disk is never modified\n"
        "                                             --render-thumbnail: the icon is rendered as a faceless, bald mannequin wearing\n"
        "                                             the outfit (like Koikatsu's own coordinate files); needs --library and --mods\n"
        "  cardbridge inspect <card.png> [--library <dir>] [--mods <dir>] [--json]\n"
        "  cardbridge library import --game <install> --library <dir> [--minimal]\n"
        "  cardbridge library check --library <dir> [--hashes]\n"
        "  cardbridge pmx info <model.pmx> [--json]      report a PMX's format and feature counts\n"
        "  cardbridge pmx roundtrip <in.pmx> <out.pmx>   import, re-export and verify structural preservation\n"
        "  cardbridge pmx restore-body <in.pmx> <out.pmx> --library <dir>\n"
        "                                             append the KK/KKS body from the asset library (append-only PMX edit)\n"
        "  cardbridge pmx apply-asset <in.pmx> <out.pmx> --library <dir> --key <assetKey> [--mods <dir>]\n"
        "                                             apply one catalog asset with a compatibility report (append-only PMX edit)\n"
        "                                             [--no-textures] [--json-report] [--map src>dst] [--tx..--sz] [--auto]\n"
        "  cardbridge assets build --library <dir> --mods <dir>   index KK/KKS clothes, hair and accessories\n"
        "  cardbridge assets list  --library <dir> --mods <dir> [--category clothing|hair|accessories|body|textures|other]\n"
        "                          [--search <text>] [--limit N] [--json] [--cache <file>]\n"
        "\n"
        "Only 'library import' reads a game install (once, to copy files into the library).\n"
        "Exit codes: 0 ok, 1 usage, 2 not a supported card, 3 conversion failed.");
}

std::string defaultCache() {
    const char* la = std::getenv("LOCALAPPDATA");
    return la ? std::string(la) + "/CardBridge/modindex.json" : std::string();
}

std::string normalizeSlashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// The breast / hip jiggle settings: --jiggle-preset picks a tuned set of real physical parameters, and the --jiggle-*
// values override it one by one. --jiggle-intensity is the old name for --jiggle-strength. Everything is clamped to ranges
// the fixed-step solver and the PMX exporter can handle. False with `error` when the preset name is wrong.
bool JiggleFromArgs(const Args& a, assemble::JiggleSettings& out, std::string& error) {
    assemble::JiggleSettings s;
    if (a.opt.count("jiggle-preset")) {
        const std::string preset = a.get("jiggle-preset");
        if (preset == "subtle") s = assemble::JigglePresetSettings(assemble::JigglePreset::Subtle);
        else if (preset == "very-subtle" || preset == "verysubtle") s = assemble::JigglePresetSettings(assemble::JigglePreset::VerySubtle);
        else if (preset == "very-very-subtle" || preset == "veryverysubtle" || preset == "very-very") s = assemble::JigglePresetSettings(assemble::JigglePreset::VeryVerySubtle);
        else if (preset == "natural") s = assemble::JigglePresetSettings(assemble::JigglePreset::Natural);
        else if (preset == "dynamic") s = assemble::JigglePresetSettings(assemble::JigglePreset::Dynamic);
        else { error = "unknown --jiggle-preset '" + preset + "' (subtle|very-subtle|very-very-subtle|natural|dynamic)"; return false; }
    }
    auto num = [&a, &s](const char* key, float assemble::JiggleSettings::*member) {
        const auto it = a.opt.find(key);
        if (it != a.opt.end()) s.*member = static_cast<float>(std::atof(it->second.c_str()));
    };
    num("jiggle-mass", &assemble::JiggleSettings::mass);
    num("jiggle-stiffness", &assemble::JiggleSettings::stiffness);
    num("jiggle-damping", &assemble::JiggleSettings::damping);
    num("jiggle-gravity", &assemble::JiggleSettings::gravity);
    num("jiggle-amplitude-vertical", &assemble::JiggleSettings::amplitudeVertical);
    num("jiggle-amplitude-horizontal", &assemble::JiggleSettings::amplitudeHorizontal);
    num("jiggle-amplitude-depth", &assemble::JiggleSettings::amplitudeDepth);
    num("jiggle-rotation-strength", &assemble::JiggleSettings::rotationStrength);
    num("jiggle-strength", &assemble::JiggleSettings::strength);
    num("jiggle-intensity", &assemble::JiggleSettings::strength);   // the old name
    num("jiggle-sim-hz", &assemble::JiggleSettings::simulationHz);
    out = assemble::SanitizeJiggleSettings(s);
    return true;
}

void printIssues(const std::vector<std::string>& v, const char* prefix) {
    for (const auto& s : v) std::fprintf(stderr, "%s%s\n", prefix, s.c_str());
}

// A one-line text progress bar: "label [####------] 42% (item 3 / 10)".
// fraction < 0 = indeterminate spinner. Skipped when CARDBRIDGE_NO_PROGRESS=1.
void printProgress(double fraction, const std::string& stage) {
    if (std::getenv("CARDBRIDGE_NO_PROGRESS")) return;
    constexpr int W = 24;
    std::string bar(W, '-');
    char pct[8] = {0};
    if (fraction >= 0.0) {
        const double f = std::clamp(fraction, 0.0, 1.0);
        const int filled = static_cast<int>(f * W);
        for (int i = 0; i < filled; ++i) bar[static_cast<size_t>(i)] = '#';
        std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(std::lround(f * 100.0)));
    } else {
        static int tick = 0;
        ++tick;
        const int pos = tick % W;
        bar[static_cast<size_t>(pos)] = '*';
        std::snprintf(pct, sizeof pct, "  ");
    }
    std::fprintf(stderr, "\r[%s] %s %s", bar.c_str(), pct, stage.c_str());
    std::fflush(stderr);
}

// Shared by convert and inspect: mods index, base lists, resolver.
struct Context {
    resolve::ModIndex mods;
    resolve::ListTable lists;
    std::unique_ptr<resolve::Resolver> resolver;
    library::GameInfo game;
    bool haveLibrary = false;
};

bool buildContext(const Args& a, Context& ctx) {
    std::vector<std::string> issues;
    std::string modsDir = normalizeSlashes(a.get("mods"));
    if (!modsDir.empty()) {
        std::string cache = a.get("mod-cache", defaultCache());
        if (!cache.empty()) {
            std::string dir = cache.substr(0, cache.find_last_of("/\\"));
            if (!dir.empty() && dir != cache) { try { fileio::CreateDirectories(dir); } catch (...) {} }
        }
        resolve::ModIndex::Stats st;
        std::fprintf(stderr, "indexing mods in %s ...\n", modsDir.c_str());
        ctx.mods = resolve::ModIndex::Build(modsDir, normalizeSlashes(cache), &issues, &st, [](size_t d, size_t n) {
            printProgress(n ? static_cast<double>(d) / static_cast<double>(n) : -1.0,
                          "indexing mods " + std::to_string(d) + " / " + std::to_string(n));
        });
        std::fprintf(stderr, "\n  %zu mods files (%zu from cache, %zu read, %zu unreadable), %zu distinct GUIDs\n",
                     st.zips, st.reused, st.opened, st.failed, ctx.mods.guidCount());
        printIssues(issues, "  note: ");
        issues.clear();
    }
    std::string lib = normalizeSlashes(a.get("library"));
    std::string abdata;
    if (!lib.empty()) {
        auto chk = library::CheckLibrary(lib, false);
        if (!chk.ok) {
            printIssues(chk.issues, "library: ");
            return false;
        }
        abdata = library::AbdataRoot(lib);
        ctx.game = chk.game;
        size_t rows = ctx.lists.loadBaseGame(abdata, &issues);
        printIssues(issues, "  note: ");
        std::fprintf(stderr, "library: %s profile", chk.profile.c_str());
        if (ctx.game.game != library::SourceGame::Unknown) {
            std::fprintf(stderr, " (%s", ctx.game.name());
            if (!ctx.game.unityVersion.empty()) std::fprintf(stderr, ", Unity %s", ctx.game.unityVersion.c_str());
            if (ctx.game.serializedFormat) std::fprintf(stderr, ", serialized %d", ctx.game.serializedFormat);
            std::fprintf(stderr, ")");
        }
        std::fprintf(stderr, ", %zu list rows\n", rows);
        ctx.haveLibrary = true;
    }
    ctx.mods.setGame(ctx.game);
    ctx.resolver = std::make_unique<resolve::Resolver>(ctx.mods, ctx.lists, abdata, ctx.game);
    return true;
}

// A KKS card must be converted against a KKS library (and a KK card against a
// KK one) to find its base-game assets. Older libraries have no game field, so
// Unknown is silent. This is a diagnostic, never a hard failure: a user may
// deliberately convert a card with a library that has only mod items.
void noteGameMismatch(const card::Card& c, const Context& ctx) {
    if (!ctx.haveLibrary || ctx.game.game == library::SourceGame::Unknown) return;
    const bool kksCard = c.family == card::CardFamily::KoikatsuSunshine;
    const bool kksLib = ctx.game.game == library::SourceGame::KoikatsuSunshine;
    if (kksCard != kksLib)
        std::fprintf(stderr, "note: card is %s but the library was imported from %s; base-game assets may not resolve\n",
                     card::FamilyName(c.family), ctx.game.name());
}

int cmdLibrary(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    const std::string& sub = a.positional[1];
    std::string lib = normalizeSlashes(a.get("library"));
    if (lib.empty()) { std::fprintf(stderr, "--library <dir> is required\n"); return 1; }
    if (sub == "import") {
        std::string game = normalizeSlashes(a.get("game"));
        if (game.empty()) { std::fprintf(stderr, "--game <install folder> is required for import\n"); return 1; }
        library::ImportOptions o;
        o.gameRoot = game;
        o.libraryRoot = lib;
        o.profile = a.has("minimal") ? library::Profile::Minimal : library::Profile::Full;
        std::fprintf(stderr, "copying game data into %s (the game folder is only read) ...\n", lib.c_str());
        auto r = library::ImportLibrary(o, [](size_t d, size_t n) {
            printProgress(n ? static_cast<double>(d) / static_cast<double>(n) : -1.0,
                          "copying game data " + std::to_string(d) + " / " + std::to_string(n));
        });
        std::fprintf(stderr, "\n");
        printIssues(r.issues, "  problem: ");
        std::printf("library import: %zu files (%zu copied, %zu already present), %.1f MB\n", r.files, r.copied, r.unchanged,
                    static_cast<double>(r.bytes) / 1048576.0);
        return r.issues.empty() ? 0 : 3;
    }
    if (sub == "check") {
        auto r = library::CheckLibrary(lib, a.has("hashes"));
        printIssues(r.issues, "  problem: ");
        std::printf("library %s: %s, %zu files%s\n", lib.c_str(), r.ok ? "OK" : "PROBLEMS", r.files, r.profile.empty() ? "" : (" (" + r.profile + " profile)").c_str());
        return r.ok ? 0 : 3;
    }
    usage();
    return 1;
}

bool loadCard(const std::string& path, card::Card& c, int& code) {
    try {
        c = card::LoadCard(path);
        return true;
    } catch (const card::CardError& e) {
        std::fprintf(stderr, "%s: %s [%s]\n", path.c_str(), e.what(), card::ErrorKindName(e.kind));
        code = 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), e.what());
        code = 3;
    }
    return false;
}

// Applies an outfit source to a character in memory (never writes a card file). The source is detected
// from its bytes: a regular outfit file merges through the clothing import merge; a Koikatsu
// Coordinate PNG (a character card used as an outfit source) is first stripped down to its outfit
// contents and then merged the same way (identity, face, body, personality never travel).
// Returns the merged outfit index, or -1 with `code` set.
int applyClothes(const Args& a, card::Card& c) {
    if (a.get("clothes").empty()) return -1;
    card::ApplyClothingOptions opt;
    if (a.opt.count("outfit")) opt.outfit = std::atoi(a.get("outfit").c_str());
    opt.keepHairAccessories = !a.has("replace-accessories");
    card::OutfitSourceReport rep;
    try {
        rep = card::ApplyOutfitSource(c, normalizeSlashes(a.get("clothes")), opt);
    } catch (const card::CardError& e) {
        std::fprintf(stderr, "%s: %s [%s]\n", normalizeSlashes(a.get("clothes")).c_str(), e.what(), card::ErrorKindName(e.kind));
        throw std::runtime_error("outfit source failed");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        throw;
    }
    const auto& ar = rep.apply;
    if (rep.fromCoordinate)
        std::fprintf(stderr, "  coordinate (as clothing): %s (coordinate %d) -> outfit %d (%d clothes slot(s) replaced, %d accessory(ies) added%s, %d hair accessories kept)\n",
                     rep.sourceName.c_str(), rep.srcOutfit, ar.outfit, ar.clothesReplaced, ar.accessoriesAdded,
                     ar.accessoriesRelocated ? (", " + std::to_string(ar.accessoriesRelocated) + " moved to keep hair").c_str() : "",
                     ar.hairPreserved);
    else
        std::fprintf(stderr, "  clothing: %s -> outfit %d (%d clothes slot(s) replaced, %d accessory(ies) added%s, %d hair accessories kept)\n",
                     rep.sourceName.c_str(), ar.outfit, ar.clothesReplaced, ar.accessoriesAdded,
                     ar.accessoriesRelocated ? (", " + std::to_string(ar.accessoriesRelocated) + " moved to keep hair").c_str() : "",
                     ar.hairPreserved);
    return ar.outfit;
}

int cmdInspect(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    // An outfit file inspects as an outfit (name, ids, resolver rows), not as
    // "not a supported card": --clothes is the merge spelling elsewhere.
    if (a.get("clothes").empty()) {
        try {
            card::ClothingFile cl = card::LoadClothingFile(normalizeSlashes(a.positional[1]));
            nlohmann::json j;
            j["file"] = fileio::FileName(cl.path);
            j["sha256"] = cl.sha256;
            j["kind"] = "outfit";
            j["marker"] = cl.marker;
            j["format_version"] = cl.formatVersion;
            j["outfit_name"] = cl.name;
            j["supported"] = cl.supported;
            if (!cl.supportNote.empty()) j["note"] = cl.supportNote;
            if (cl.supported) {
                j["resolver_rows"] = cl.rows.size();
                std::set<std::string> guids;
                for (const auto& r : cl.rows) guids.insert(r.modId);
                j["mods_named_by_outfit"] = guids.size();
                j["clothes_ids"] = nlohmann::json::array();
                for (const auto& p : cl.outfit.clothes.at("parts").asArray()) j["clothes_ids"].push_back(p.at("id").asInt());
                j["sub_parts"] = nlohmann::json::array();
                for (const auto& e : cl.outfit.clothes.at("subPartsId").asArray()) j["sub_parts"].push_back(e.asInt());
                j["accessories"] = nlohmann::json::array();
                for (const auto& p : cl.outfit.accessory.at("parts").asArray()) {
                    if (p.at("type").asInt() == 120 || p.at("id").asInt() == 0) continue;
                    j["accessories"].push_back({{"type", p.at("type").asInt()}, {"id", p.at("id").asInt()}, {"parent", p.at("parentKey").asStr()}});
                }
            }
            std::puts(j.dump(2).c_str());
            return 0;
        } catch (const card::CardError&) {
            // Not an outfit file: fall through to the character path below.
        } catch (const std::exception&) {
            // Not an outfit file either.
        }
    }
    card::Card c;
    int code = 0;
    if (!loadCard(normalizeSlashes(a.positional[1]), c, code)) return code;
    if (!a.get("clothes").empty()) {
        try { applyClothes(a, c); }
        catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 3; }
    }

    nlohmann::json j;
    j["file"] = fileio::FileName(c.path);
    j["sha256"] = c.sha256;
    j["family"] = card::FamilyName(c.family);
    j["marker"] = c.marker;
    j["format_version"] = c.formatVersion;
    j["supported"] = c.supported;
    if (!c.supportNote.empty()) j["note"] = c.supportNote;
    j["blocks"] = nlohmann::json::array();
    for (const auto& b : c.blocks) j["blocks"].push_back({{"name", b.name}, {"version", b.version}, {"size", b.size}});
    if (c.supported) {
        j["character"] = c.displayName();
        j["outfits"] = c.outfits.size();
        j["resolver_rows"] = c.rows.size();
        std::set<std::string> guids;
        for (const auto& r : c.rows) guids.insert(r.modId);
        j["mods_named_by_card"] = guids.size();

        Context ctx;
        if (a.has("json") || !a.get("mods").empty() || !a.get("library").empty()) {
            if (!buildContext(a, ctx)) return 3;
            noteGameMismatch(c, ctx);
            if (ctx.haveLibrary) j["library_game"] = ctx.game.name();
        }
        int outfit = a.opt.count("outfit") ? std::atoi(a.get("outfit").c_str()) : static_cast<int>(c.status.at("coordinateType").asInt());
        j["outfit"] = outfit;
        j["items"] = nlohmann::json::array();
        for (const auto& ref : resolve::ItemsFromCard(c, outfit)) {
            nlohmann::json it = {{"id", ref.partId}, {"category", ref.category}, {"item", ref.id}};
            if (ctx.resolver && (ctx.haveLibrary || !a.get("mods").empty())) {
                auto r = ctx.resolver->resolve(c, ref);
                it["status"] = resolve::StatusName(r.status);
                if (!r.modGuid.empty()) it["mod"] = r.modGuid;
                if (!r.detail.empty()) it["detail"] = r.detail;
                if (r.ok()) it["bundle"] = r.bundleRel();
            }
            j["items"].push_back(std::move(it));
        }
    }
    std::puts(j.dump(2).c_str());
    return 0;
}

// Exports one outfit of a character card as a Koikatsu outfit file (【KoiKatuClothes】 .png),
// the same format --clothes applies. Never touches the source card.
int cmdClothes(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    const std::string cardPath = normalizeSlashes(a.positional[1]);
    card::Card c;
    int code = 0;
    if (!loadCard(cardPath, c, code)) return code;
    if (!c.supported) {
        std::fprintf(stderr, "%s: not a convertible card\n", cardPath.c_str());
        return 2;
    }
    card::ClothingExportOptions opt;
    if (a.opt.count("outfit")) {
        char* end = nullptr;
        const long v = std::strtol(a.get("outfit").c_str(), &end, 10);
        if (!end || *end != '\0' || v < 0) { std::fprintf(stderr, "--outfit must be a non-negative number\n"); return 1; }
        opt.outfit = static_cast<int>(v);
        if (opt.outfit >= static_cast<int>(c.outfits.size())) {
            std::fprintf(stderr, "outfit %d is out of range; the card has %zu outfit(s)\n", opt.outfit, c.outfits.size());
            return 1;
        }
    }
    if (!a.get("name").empty()) opt.name = a.get("name");
    if (!a.get("thumbnail").empty()) opt.thumbnailPngPath = normalizeSlashes(a.get("thumbnail"));
    if (a.has("render-thumbnail")) {
        if (a.get("library").empty()) { std::fprintf(stderr, "--render-thumbnail needs --library (it assembles the scene like convert does)\n"); return 1; }
        if (!a.has("no-mods") && a.get("mods").empty()) { std::fprintf(stderr, "--render-thumbnail needs --mods, or --no-mods when the card only uses base-game items\n"); return 1; }
        Context ctx;
        if (!buildContext(a, ctx)) return 3;
        card::Card mc = c;   // assemble mutates in place (pose baked, briefs re-coloured); never touch the caller's card
        assemble::AssembleOptions ao;
        ao.sourceGame = ctx.game;
        if (a.get("outfit").empty()) ao.outfit = -1;
        else ao.outfit = opt.outfit;
        if (!a.get("clothes").empty()) {
            try { const int t = applyClothes(a, mc); if (t >= 0) ao.outfit = t; }
            catch (const std::exception& e) { std::fprintf(stderr, "clothing: %s\n", e.what()); return 3; }
        }
        if (ao.outfit < 0) ao.outfit = mc.status.isMap() ? static_cast<int>(mc.status.at("coordinateType").asInt()) : 0;
        assemble::Scene scene = assemble::Assemble(mc, *ctx.resolver, ao);
        {   // the stock outfit-icon pose: hand on hip, so the icon shows the outfit as worn
            assemble::BakePose(scene, assemble::MakeHandsOnHipsPose(scene));
        }
        const int iconOutfit = ao.outfit >= 0 && ao.outfit < static_cast<int>(c.outfits.size()) ? ao.outfit : 0;
        const std::string prefix = "outfit" + std::to_string(iconOutfit) + ".acc";
        std::vector<char> base(scene.parts.size(), 0);
        for (size_t pi = 0; pi < scene.parts.size(); ++pi) {
            const std::string& id = scene.parts[pi].id;
            if (id.rfind(prefix, 0) != 0) continue;
            const std::string tail = id.substr(prefix.size());
            if (tail.empty()) continue;
            bool digits = true;
            for (unsigned char ch : tail) if (!std::isdigit(ch)) { digits = false; break; }
            if (digits && opt.hiddenSlots.count(std::atoi(tail.c_str()))) base[pi] = 1;
        }
        std::vector<char> flags = exporter::OutfitIconHiddenParts(scene, mc, iconOutfit, base, a.has("with-hair-accessories"));
        exporter::OutfitIconOptions ipo;
        ipo.hiddenParts = &flags;
        std::string iconNote;
        ipo.note = &iconNote;
        opt.thumbnailBytes = exporter::RenderOutfitIcon(scene, ipo);
        if (!iconNote.empty()) std::fprintf(stderr, "icon: %s\n", iconNote.c_str());
    }
    if (a.has("include-hidden")) opt.includeHidden = true;
    if (a.has("with-hair-accessories")) opt.withHairAccessories = true;
    if (a.has("with-abmx")) opt.withAbmx = true;
    if (!a.get("hide").empty()) {
        const std::string list = a.get("hide");
        size_t p = 0;
        while (p <= list.size()) {
            const size_t comma = list.find(',', p);
            std::string tok = list.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
            while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
            while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
            if (!tok.empty()) {
                const bool digits = std::all_of(tok.begin(), tok.end(), [](unsigned char c) { return std::isdigit(c); });
                if (!digits) { std::fprintf(stderr, "--hide takes comma-separated accessory slot numbers, got '%s'\n", tok.c_str()); return 1; }
                opt.hiddenSlots.insert(std::atoi(tok.c_str()));
            }
            if (comma == std::string::npos) break;
            p = comma + 1;
        }
    }
    std::string outPath = normalizeSlashes(a.get("out"));
    if (outPath.empty()) {
        int outfit = opt.outfit;
        if (outfit < 0) outfit = c.status.isMap() ? static_cast<int>(c.status.at("coordinateType").asInt()) : 0;
        std::string stem = fileio::Stem(fileio::FileName(cardPath));
        std::string name = c.outfitName(std::clamp(outfit, 0, static_cast<int>(c.outfits.size()) - 1));
        auto sanitise = [](std::string s) {
            for (char& ch : s) if (std::strchr("\\/:*?\"<>|", ch)) ch = '_';
            return s;
        };
        const size_t slash = cardPath.find_last_of("/\\");
        const std::string folder = slash == std::string::npos ? std::string() : cardPath.substr(0, slash + 1);
        outPath = folder + sanitise(stem) + " - " + sanitise(name) + ".png";
    }
    card::ClothingExportReport rep;
    try {
        rep = card::ExportClothingToFile(c, outPath, opt, a.has("overwrite"));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 3;
    }
    const uint64_t bytes = fileio::FileSize(outPath);
    if (a.has("json")) {
        nlohmann::json j;
        j["file"] = fileio::FileName(outPath);
        j["path"] = outPath;
        j["bytes"] = bytes;
        j["kind"] = "koikatsu_clothing_file";
        j["marker"] = "【KoiKatuClothes】";
        j["format_version"] = c.formatVersion.empty() ? "0.0.0" : c.formatVersion;
        j["outfit"] = rep.outfit;
        j["outfit_name"] = rep.outfitName;
        j["ids_localised"] = {{"clothes", rep.clothesIdsLocalized}, {"sub_parts", rep.subPartsLocalized}, {"accessories", rep.accessoryIdsLocalized}};
        j["resolver_rows_kept"] = rep.uarRowsKept;
        j["material_editor"] = {{"rows", rep.meListRows}, {"textures_kept", rep.meTexturesKept}};
        j["kcox_garments"] = rep.kcoxGarments;
        j["more_accessories_kept"] = rep.extraAccessoriesKept;
        j["hidden_accessories_dropped"] = rep.hiddenAccessoriesDropped;
        j["hair_accessories_dropped"] = rep.hairAccessoriesDropped;
        j["dropped_plugins"] = rep.droppedPlugins;
        j["warnings"] = rep.warnings;
        j["icon"] = a.has("render-thumbnail") ? "mannequin_render" : (a.get("thumbnail").empty() ? "card_face_thumbnail" : "custom_file");
        std::puts(j.dump(2).c_str());
        return 0;
    }
    std::printf("clothing: wrote %s (%llu bytes), outfit %d — %s\n", outPath.c_str(), (unsigned long long)bytes, rep.outfit + 1, rep.outfitName.c_str());
    if (a.has("render-thumbnail")) std::printf("  icon: rendered the outfit on a faceless mannequin\n");
    std::printf("  clothes re-localised: %d (+ %d sub-part id(s)), accessories: %d\n", rep.clothesIdsLocalized, rep.subPartsLocalized, rep.accessoryIdsLocalized);
    std::printf("  hidden/hair omitted: %d/%d   resolver rows kept: %d\n", rep.hiddenAccessoriesDropped, rep.hairAccessoriesDropped, rep.uarRowsKept);
    std::printf("  MaterialEditor: %d row(s) rebased, %d texture(s) kept   KCOX: %d garment(s)   MoreAccessories: %d extra(s)\n",
                rep.meListRows, rep.meTexturesKept, rep.kcoxGarments, rep.extraAccessoriesKept);
    if (!rep.droppedPlugins.empty()) {
        std::printf("  dropped plugin blocks:");
        for (const std::string& s : rep.droppedPlugins) std::printf(" %s", s.c_str());
        std::printf("\n");
    } else {
        std::printf("  dropped plugin blocks: none\n");
    }
    for (const std::string& s : rep.warnings) std::printf("  warning: %s\n", s.c_str());
    return 0;
}

int cmdShader(const Args& a) {
    // cardbridge shader <blob>: the programs of a decompressed Unity shader blob with their bindings, and the forward pass.
    if (a.positional.size() < 2) { std::fprintf(stderr, "usage: cardbridge shader <blob>\n"); return 1; }
    Bytes data = fileio::ReadFile(a.positional[1]);
    std::vector<std::string> why;
    auto progs = shader::ParseUnityShaderBlob(data, &why);
    for (const auto& w : why) std::fprintf(stderr, "  skipped: %s\n", w.c_str());
    if (progs.empty()) { std::fprintf(stderr, "no programs found in %s\n", a.positional[1].c_str()); return 2; }
    size_t pi = 0, vi = 0;
    bool fwd = shader::PickForwardPass(progs, pi, vi);
    for (size_t n = 0; n < progs.size(); ++n) {
        const auto& d = progs[n];
        std::printf("program %zu: %s_%u_%u, %zu tokens%s\n", n, d.isPixel() ? "ps" : d.isVertex() ? "vs" : "xs", d.code.major, d.code.minor, d.code.tokens.size(),
                    fwd && (n == pi || n == vi) ? "   <== forward pass" : "");
        if (!d.keywords.empty()) { std::printf("  keywords:"); for (const auto& k : d.keywords) std::printf(" %s", k.c_str()); std::printf("\n"); }
        for (const auto& cb : d.cbuffers) {
            std::printf("  cb%d %s (%u bytes)\n", cb.slot, cb.name.c_str(), cb.size);
            for (const auto& v : cb.params) std::printf("    +%-4u %ux%u%s %s\n", v.offset, v.rows, v.cols, v.isMatrix ? " matrix" : "", v.name.c_str());
        }
        for (const auto& t : d.textures) std::printf("  t%u s%u dim%u %s\n", t.reg, t.sampler, t.dimension, t.name.c_str());
    }
    return 0;
}

int cmdShaderRun(const Args& a) {
    // cardbridge shader-run <blob>: runs the forward pixel program once with synthetic inputs (white textures, unit
    // vectors) to prove the interpreter handles every instruction of the shader; prints the result colour.
    if (a.positional.size() < 2) { std::fprintf(stderr, "usage: cardbridge shader-run <blob>\n"); return 1; }
    Bytes data = fileio::ReadFile(a.positional[1]);
    auto progs = shader::ParseUnityShaderBlob(data, nullptr);
    size_t pi = 0, vi = 0;
    if (!shader::PickForwardPass(progs, pi, vi)) { std::fprintf(stderr, "no forward pass\n"); return 2; }
    const auto& ps = progs[pi];
    shader::Sm4Program sm;
    std::string why;
    if (!shader::DecodeSm4(ps.code.tokens, sm, &why)) { std::fprintf(stderr, "decode: %s\n", why.c_str()); return 2; }
    std::printf("decoded %zu instructions, %u temps\n", sm.code.size(), sm.tempCount);
    struct Tex : shader::ISm4Textures {
        const shader::UnityProgram* p;
        void Sample(int t, int, const float*, float, float out[4]) override {
            bool normal = false;
            for (const auto& b : p->textures) if (static_cast<int>(b.reg) == t && b.name.find("Normal") != std::string::npos) normal = true;
            out[0] = normal ? 0.5f : 1.0f; out[1] = normal ? 0.5f : 1.0f; out[2] = 1.0f; out[3] = 1.0f;
        }
    } tex;
    tex.p = &ps;
    std::vector<std::vector<float>> cbs(8);
    shader::Sm4Io io;
    for (const auto& cb : ps.cbuffers) {
        if (cb.slot < 0 || cb.slot >= 8) continue;
        cbs[static_cast<size_t>(cb.slot)].assign(cb.size / 4 + 4, 0.0f);
        for (const auto& pm : cb.params) {
            float* f = &cbs[static_cast<size_t>(cb.slot)][pm.offset / 4];
            if (pm.name.size() > 3 && pm.name.compare(pm.name.size() - 3, 3, "_ST") == 0) { f[0] = 1; f[1] = 1; }
            else if (pm.name == "_LightColor0" || pm.name == "_ShadowColor") { f[0] = f[1] = f[2] = 0.9f; f[3] = 1; }
            else if (pm.name == "_alpha_a" || pm.name == "_alpha_b") f[0] = 1;
            else if (pm.name == "_WorldSpaceLightPos0") { f[1] = 1; }
        }
    }
    io.constantBuffers.resize(8, nullptr);
    for (size_t i = 0; i < 8; ++i) if (!cbs[i].empty()) io.constantBuffers[i] = cbs[i].data();
    io.textures = &tex;
    for (const auto& in : ps.code.inputs) {
        if (in.reg >= 32) continue;
        float* v = io.input[in.reg];
        if (in.semantic == "SV_IsFrontFace") { uint32_t m = 0xFFFFFFFFu; std::memcpy(v, &m, 4); }
        else { v[0] = 0.3f; v[1] = 0.5f; v[2] = 0.8f; v[3] = 1.0f; }
    }
    if (!shader::RunSm4(sm, io, &why)) { std::fprintf(stderr, "run: %s\n", why.c_str()); return 2; }
    std::printf("%s -> %.4f %.4f %.4f %.4f\n", io.discarded ? "discarded" : "colour", io.output[0][0], io.output[0][1], io.output[0][2], io.output[0][3]);
    return 0;
}

int cmdConvert(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    std::string out = normalizeSlashes(a.get("out"));
    if (out.empty() || a.get("library").empty()) { std::fprintf(stderr, "--library and --out are required\n"); return 1; }
    exporter::BakeQuality bakeQuality;
    {
        std::string err;
        if (!BakeQualityFromArgs(a, bakeQuality, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    }
    // Shader maps are decoded during assembly, before the bake, so the preset cap has to be
    // set here. Standard (and a raw --bake-size without a preset) keeps the historical 1024.
    assemble::SetShaderTextureMaxSize(bakeQuality.areaShaderMaps ? std::max(1024, bakeQuality.maxSize) : 1024);
    card::Card c;
    int code = 0;
    LogMemory("startup");
    {
        cbtime::Scope _("card load");
        if (!loadCard(normalizeSlashes(a.positional[1]), c, code)) return code;
    }
    LogMemory("after card load");
    if (!c.supported) {
        std::fprintf(stderr, "%s: %s\n", c.path.c_str(), c.supportNote.c_str());
        return 2;
    }
    Context ctx;
    {
        cbtime::Scope _("context (lists + resolver)");
        if (!buildContext(a, ctx)) return 3;
    }
    LogMemory("after context (lists)");

    assemble::AssembleOptions ao;
    ao.sourceGame = ctx.game;
    noteGameMismatch(c, ctx);
    if (a.opt.count("outfit")) ao.outfit = std::atoi(a.get("outfit").c_str());
    if (!a.get("clothes").empty()) {
        try {
            const int target = applyClothes(a, c);
            if (target >= 0) ao.outfit = target;
        } catch (const std::exception& e) { std::fprintf(stderr, "clothing: %s\n", e.what()); return 3; }
    }
    // --body "4=0.8,13=0.1" / --face "..." set sliders (index=value, 0..1 is the maker's range); --nipple-inside pushes the nipples in.
    for (const char* which : {"body", "face"}) {
        std::string spec = a.get(which);
        std::vector<float>& vals = std::string(which) == "body" ? ao.bodyShape : ao.faceShape;
        const Value& card0 = std::string(which) == "body" ? c.body.at("shapeValueBody") : c.face.at("shapeValueFace");
        for (size_t i = 0; i < card0.size(); ++i) vals.push_back(static_cast<float>(card0.at(i).asFloat()));
        for (size_t at = 0; at < spec.size();) {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            std::string item = spec.substr(at, end - at);
            at = end + 1;
            size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            size_t idx = static_cast<size_t>(std::atoi(item.c_str()));
            if (idx < vals.size()) vals[idx] = static_cast<float>(std::atof(item.c_str() + eq + 1));
        }
    }
    std::string transferJson;
    if (!a.get("transfer").empty()) {
        try {
            const auto bytes = fileio::ReadFile(a.get("transfer"));
            const std::string text(bytes.begin(), bytes.end());
            assemble::transfer::Spec spec;
            std::string err;
            if (!assemble::transfer::FromJson(text, spec, &err)) { std::fprintf(stderr, "transfer: %s\n", err.c_str()); return 3; }
            assemble::ProportionLayer empty;
            ao.proportionLayer = assemble::transfer::Merge(empty, spec.ops, true);
            transferJson = text;
            std::fprintf(stderr, "transfer: %zu op(s) from %s\n", ao.proportionLayer.ops.size(), spec.referenceName.c_str());
            for (const auto& note : spec.notes) std::fprintf(stderr, "  note: %s\n", note.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: cannot read transfer file %s: %s\n", a.get("transfer").c_str(), e.what());
            return 3;
        }
    }
    if (a.has("normalize-head-proportions") || a.has("normalize-body-proportions") || a.has("normalize-face-proportions") ||
        a.has("normalize-overall") || !a.get("proportions").empty()) {
        assemble::ProportionLayer layer;
        if (!a.get("proportions").empty()) {
            std::string err;
            std::vector<uint8_t> bytes;
            try {
                bytes = fileio::ReadFile(a.get("proportions"));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "error: cannot read proportions file %s: %s\n", a.get("proportions").c_str(), e.what());
                return 3;
            }
            const std::string text(bytes.begin(), bytes.end());
            if (!assemble::ProportionLayerFromJson(text, layer, &err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                return 3;
            }
            std::fprintf(stderr, "proportions: layer loaded (%zu op(s), enabled=%d)\n", layer.ops.size(), layer.enabled ? 1 : 0);
        }
        if (a.has("normalize-head-proportions")) layer.ops = assemble::PresetNormalizeHead(c);
        if (a.has("normalize-body-proportions")) layer.ops = assemble::PresetNormalizeBody(c);
        if (a.has("normalize-face-proportions")) layer.ops = assemble::PresetNormalizeFace(c);
        if (a.has("normalize-overall")) layer.ops = assemble::PresetNormalizeOverall(c);
        ao.proportionLayer = assemble::transfer::Merge(layer, ao.proportionLayer.ops, ao.proportionLayer.enabled || layer.enabled);
    }
    if (a.has("print-diagnostics")) {
        const auto findings = assemble::AnalyzeCard(c);
        std::printf("proportion diagnostics for %s (informational):\n", c.displayName().c_str());
        if (findings.empty()) std::printf("  none — this card's sliders and ABMX table look proportionate.\n");
        for (const auto& f : findings) {
            const char* tag = f.safety == assemble::kSafetySafe ? "safe" : f.safety == assemble::kSafetyApproximate ? "approx" : "manual";
            std::printf("  [%s] %s: %s (current %s)%s\n", tag, f.property.c_str(), f.reason.c_str(), f.current.c_str(),
                        f.suggestion.empty() ? "" : ("; suggestion: " + f.suggestion).c_str());
        }
    }
    ao.nippleInside = a.has("nipple-inside");
    if (a.opt.count("nipple-offset")) ao.nippleOffset = static_cast<float>(std::atof(a.get("nipple-offset").c_str()));
    if (a.opt.count("skin-shade")) ao.skinShade = std::clamp(static_cast<float>(std::atof(a.get("skin-shade").c_str())), 0.0f, 2.0f);
    if (a.opt.count("skin-underboob")) ao.skinShadeUnder = std::clamp(static_cast<float>(std::atof(a.get("skin-underboob").c_str())), 0.0f, 2.0f);
    if (a.opt.count("skin-top")) ao.skinShadeTop = std::clamp(static_cast<float>(std::atof(a.get("skin-top").c_str())), 0.0f, 2.0f);
    if (a.opt.count("skin-width")) ao.skinShadeWidth = std::clamp(static_cast<float>(std::atof(a.get("skin-width").c_str())), 0.1f, 2.0f);
    if (a.opt.count("skin-ao")) ao.skinAo = std::clamp(static_cast<float>(std::atof(a.get("skin-ao").c_str())), 0.0f, 2.0f);
    if (a.has("card-nipples")) {
        ao.nippleColour = Vec4{0, 0, 0, 0};   // keep the card's own nipColor
    } else if (a.opt.count("nipple-colour")) {
        const std::string spec = a.get("nipple-colour");
        float rgb[3] = {ao.nippleColour.x, ao.nippleColour.y, ao.nippleColour.z};
        int got = 0;
        for (size_t at = 0; at < spec.size() && got < 3;) {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            rgb[got++] = std::clamp(static_cast<float>(std::atof(spec.substr(at, end - at).c_str())), 0.0f, 255.0f) / 255.0f;
            at = end + 1;
        }
        if (got == 3) ao.nippleColour = Vec4{rgb[0], rgb[1], rgb[2], 1.0f};
        else std::fprintf(stderr, "  (--nipple-colour needs R,G,B in 0..255; keeping the default pink)\n");
    }
    ao.autoCloth = a.has("auto-cloth") && !a.has("no-auto-cloth") && !std::getenv("CARDBRIDGE_NO_AUTO_CLOTH") &&
                   std::string(std::getenv("CARDBRIDGE_AUTO_CLOTH") ? std::getenv("CARDBRIDGE_AUTO_CLOTH") : "1") != "0";
    ao.log = [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); };
    ao.progress = [](double f, const std::string& stage) { printProgress(f, "assembling: " + stage); };
    auto t0 = std::chrono::steady_clock::now();
    assemble::Scene scene;
    {
        cbtime::Scope _("assemble");
        scene = assemble::Assemble(c, *ctx.resolver, ao);
    }
    auto t1 = std::chrono::steady_clock::now();
    // Material workbench originals, captured straight after assembly so every
    // later edit (including --materials below) has an untouched baseline.
    material::WorkbenchState workbench;
    material::CaptureOriginals(workbench, scene);
    nlohmann::json materialWorkbenchJson;
    LogMemory("after assemble");
    // --hide / --nude hide parts for the export: hiding the heeled shoe must drop the foot
    // pitch at once (the viewer's hide/show does the same live). Sync the shoes' preview
    // visibility so the heel check sees the export state, then re-shape (which re-poses).
    {
        std::string list = "," + a.get("hide") + ",";
        bool shoeHidden = false;
        for (size_t i = 0; i < scene.parts.size(); ++i) {
            bool hid = list.find("," + scene.parts[i].id + ",") != std::string::npos;
            if (scene.parts[i].nudeSwitch) hid = a.has("nude") ? false : true;
            if (scene.parts[i].id.find(".shoes") != std::string::npos && hid) {
                scene.parts[i].visibleInPreview = false;
                shoeHidden = true;
            }
        }
        if (shoeHidden) { std::fprintf(stderr, "  (heel re-pose after --hide: reshaping...)\n"); fflush(stderr); if (!assemble::Reshape(scene, c, *ctx.resolver, ao)) std::fprintf(stderr, "  (heel re-pose after --hide needs a re-assemble)\n"); std::fprintf(stderr, "  (heel re-pose done)\n"); fflush(stderr); }
    }
    if (scene.skinShade.geometryDirty) {   // a re-shape moved the body: keep the skin shading (and the AO bake) on it
        exporter::ApplySkinShade(scene, ao.skinShade, ao.skinShadeUnder, ao.skinShadeTop, ao.skinShadeWidth, true);
        if (!scene.aoBake.baseNormal.empty() && (ao.skinAo > 0.0f || scene.aoBake.applied >= 0.0f))
            exporter::ApplySkinAoBake(scene, ao.skinAo, true);
    }
    std::fprintf(stderr, "  bundle cache: %.1f MB\n", static_cast<double>(ctx.resolver->cacheBytes()) / 1048576.0); fflush(stderr);
    size_t parts = 0, failed = 0;
    for (const auto& p : scene.parts) {
        if (p.underPiece || p.nudeSwitch) continue;   // derived from other parts
        ++parts;
        bool ok = p.status == "resolved-base" || p.status == "resolved-mod";
        if (!ok) ++failed;
        std::fprintf(stderr, "  %-22s %-18s %zu mesh(es)%s%s\n", p.id.c_str(), p.status.c_str(), p.meshes.size(),
                     p.modName.empty() ? "" : "  mod: ", p.modName.c_str());
    }
    for (const auto& is : scene.issues)
        std::fprintf(stderr, "  %s%s%s: %s\n", is.severity.c_str(), is.part.empty() ? "" : " [", is.part.empty() ? "" : (is.part + "]").c_str(), is.message.c_str());
    for (const auto& is : scene.issues)
        if (is.severity == "error") { std::fprintf(stderr, "conversion failed.\n"); return 3; }

    std::vector<Mat4> liveSkin;
    // The arms come down from the T-pose (clavicle and upper arm), 40 degrees by default (--apose-degrees N), and the PMX is written in that pose:
    // MikuMikuDance's motions are keyed for it. --tpose keeps the T-pose. --pose <file.json> bakes a pose saved by the viewer (bone edits and the
    // A-pose switch, which then decides); --apose brings the arms down as well.
    cbtime::Scope poseTimer("pose + settle");
    assemble::PoseFile poseFile;
    if (a.opt.count("pose")) {
        std::string err;
        try {
            const Bytes raw = fileio::ReadFile(a.get("pose"));
            if (!assemble::PoseFileFromJson(std::string(raw.begin(), raw.end()), poseFile, &err)) {
                std::fprintf(stderr, "error: %s: %s\n", a.get("pose").c_str(), err.c_str());
                return 1;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: cannot read the pose file %s: %s\n", a.get("pose").c_str(), e.what());
            return 1;
        }
    }
    if (!a.opt.count("pose") || a.has("apose")) poseFile.aPose = true;
    if (a.has("tpose")) poseFile.aPose = false;
    if (a.opt.count("apose-degrees")) poseFile.aPoseDegrees = static_cast<float>(std::atof(a.get("apose-degrees").c_str()));
    if (poseFile.aPose || !poseFile.bones.empty()) {
        std::vector<std::string> unknown;
        const assemble::Pose pose = assemble::MakePose(scene, poseFile, &unknown);
        for (const auto& name : unknown) std::fprintf(stderr, "  pose: this rig has no bone '%s'; skipped\n", name.c_str());
        if (poseFile.aPose && pose.base.empty()) std::fprintf(stderr, "  a-pose: this rig has no arm bones to turn\n");
        // Test aid: CARDBRIDGE_GPU_SKIN_TEST=1 leaves the scene at rest and poses the GPU view through its quick re-skin (what the viewer does live).
        if (std::getenv("CARDBRIDGE_GPU_SKIN_TEST")) liveSkin = assemble::SkinMatrices(scene, pose);
        else assemble::BakePose(scene, pose);
        if (!pose.base.empty()) std::fprintf(stderr, "  a-pose: arms down %.0f degrees\n", poseFile.aPoseDegrees);
        if (!poseFile.bones.empty()) std::fprintf(stderr, "  pose: %zu bone edit(s) from %s\n", poseFile.bones.size() - unknown.size(), a.get("pose").c_str());
    }
    // The physical jiggle settings, shared by the settle below, the preview and the PMX export. Parsed up front so a bad flag
    // stops the run before any work is done.
    assemble::JiggleSettings jiggle;
    {
        std::string error;
        if (!JiggleFromArgs(a, jiggle, error)) { std::fprintf(stderr, "error: %s\n", error.c_str()); return 1; }
    }
    if (!std::getenv("CARDBRIDGE_GPU_SKIN_TEST")) {
        if (a.opt.count("height-cm")) {
            const float targetCm = static_cast<float>(std::atof(a.get("height-cm").c_str()));
            const float baseCm = assemble::SceneHeightMeters(scene) * 100.0f;
            if (baseCm > 1e-4f && targetCm > 1e-4f) {
                const float f = targetCm / baseCm;
                if (std::fabs(f - 1.0f) > 1e-4f) {
                    assemble::Pose hp;
                    for (size_t i = 0; i < scene.bones.size(); ++i) {
                        if (scene.bones[i].parent != -1) continue;
                        auto& e = hp.edit(i);
                        e.scl = {f, f, f};
                    }
                    assemble::BakePose(scene, hp);
                    std::fprintf(stderr, "  height: %.1f cm -> %.1f cm (x%.3f)\n", baseCm, targetCm, f);
                }
            }
        }
        // Settle the body's own jiggle (breast and, when on, hips) under gravity, so the exported and previewed pose shows the
        // bust as it hangs in the game instead of pulled straight out of the chest; the game draw's live sim settles again
        // from there in the viewer / MMD. CARDBRIDGE_NO_SETTLE keeps the rest pose.
        const bool settleBust = !a.has("no-jiggle-bust"), settleButt = a.has("jiggle-butt");   // butt jiggle off by default (the card's butt sway reads as a vibrating skirt)
        const float bustSize = ao.bodyShape.size() > 4 ? ao.bodyShape[4] : 0.5f;
        const float settleSag = a.opt.count("sag") ? static_cast<float>(std::atof(a.get("sag").c_str()))
                                                     : assemble::CardSagDefault(scene, bustSize);
        assemble::BakeSettledPhysics(scene, settleBust, settleButt, jiggle, settleSag);
        // The outfit's cloth (capes, kimono sleeves, jacket panels, skirt hems) keeps the authored drape: the PMX's joints are now
        // sized to hold the card's rest pose against MMD's world gravity, so no hang is baked into the mesh.
        // Garment-vs-body clipping correction, OFF by default: the export carries the card's own garment vertices, exactly the drape
        // the viewer shows. --fix-garment-intersections (or CARDBRIDGE_FIX_GARMENT_INTERSECTIONS=1) pushes a garment vertex buried in
        // the skin back out along the body's own surface; =0 forces it off.
        const char* fixEnv = std::getenv("CARDBRIDGE_FIX_GARMENT_INTERSECTIONS");
        bool fixGarments = a.has("fix-garment-intersections");
        if (a.has("no-fix-garment-intersections")) fixGarments = false;
        if (fixEnv && *fixEnv) fixGarments = std::string(fixEnv) != "0";
        if (fixGarments)
            assemble::FixGarmentIntersections(scene);
    }
    poseTimer.Stop();

    // --materials <file.json>: apply workbench overrides after the pose bake
    // (so the shader bake below sees the edited colours), before the part
    // state is fixed for export.
    if (!a.get("materials").empty()) {
        std::string err;
        try {
            const Bytes raw = fileio::ReadFile(a.get("materials"));
            material::MaterialFile file;
            if (!material::MaterialFileFromJson(std::string(raw.begin(), raw.end()), file, &err)) {
                std::fprintf(stderr, "error: %s: %s\n", a.get("materials").c_str(), err.c_str());
                return 1;
            }
            if (!file.cardSha256.empty() && !c.sha256.empty() && file.cardSha256 != c.sha256)
                std::fprintf(stderr, "  materials: the file was made for a different card (card_sha256 mismatch); applying anyway\n");
            std::vector<std::string> unknown;
            const int applied = material::ApplyMaterialFile(scene, workbench, file, &unknown);
            for (const auto& u : unknown) std::fprintf(stderr, "  materials: '%s' is not in this scene; skipped\n", u.c_str());
            material::ReportUnsupported(scene, workbench);
            std::fprintf(stderr, "  materials: %d override(s) from %s\n", applied, a.get("materials").c_str());
            materialWorkbenchJson = nlohmann::json::parse(material::MaterialFileToJson(file), nullptr, false);
            if (materialWorkbenchJson.is_discarded()) materialWorkbenchJson["file"] = fileio::FileName(a.get("materials"));
            else materialWorkbenchJson["file"] = fileio::FileName(a.get("materials"));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: cannot read the materials file %s: %s\n", a.get("materials").c_str(), e.what());
            return 1;
        }
    }

    // --hide outfit0.top,outfit0.bra unticks parts and --nude ticks the Nude body box: the picture and the PMX start that way (the viewer's checkboxes); a hidden item is not in the PMX at all, bones included (1:1 with the viewer)
    std::vector<char> hiddenParts(scene.parts.size(), 0);
    {
        std::string list = "," + a.get("hide") + ",";
        for (size_t i = 0; i < scene.parts.size(); ++i) {
            if (list.find("," + scene.parts[i].id + ",") != std::string::npos) hiddenParts[i] = 1;
            if (scene.parts[i].nudeSwitch) hiddenParts[i] = a.has("nude") ? 0 : 1;
        }
    }
    exporter::ApplyPartStateForExport(scene, hiddenParts);
    const std::vector<char> effHidden = exporter::EffectiveHiddenParts(scene, hiddenParts);

    // The bake and the export read only the assembled scene: the bundles it
    // was built from are no longer needed, so drop them before the bake's
    // mip chains and baked textures allocate beside the scene.
    ctx.resolver->clearCaches();
    LogMemory("after bundle cache clear");
    exporter::LoadColourGrade(a.get("library"));   // the game's colour grading, when the library holds it
    if (a.has("no-grade")) exporter::SetColourGradeEnabled(false);
    // Per-card presentation: the head mod's centre nose shading ridge can read like a nose line. 1 keeps the
    // authored amount, 0 removes it, values between/above make it subtler or stronger. Cards with no detected
    // ridge are untouched, so this never paints a feature the source does not have.
    if (a.opt.count("nose-ridge") || a.opt.count("nose-ridge-opacity"))
        assemble::SetNoseRidgeStrength(scene,
                                       a.opt.count("nose-ridge") ? static_cast<float>(std::atof(a.get("nose-ridge").c_str())) : scene.noseRidge.strength,
                                       a.opt.count("nose-ridge-opacity") ? static_cast<float>(std::atof(a.get("nose-ridge-opacity").c_str())) : scene.noseRidge.opacity);
    // Default: colour only (the viewer or engine lights it, over a white material) with the preview's grading baked in.
    // --bake-lit puts the toon shading in the texture too (the material is then unlit: black in an engine that lights it itself).
    if (!a.has("no-bake")) {   // the game's shaders run over every material's UV layout, so the PMX textures carry their colour
        std::string bakeNote;
        auto tb0 = std::chrono::steady_clock::now();
        {
            cbtime::Scope _("shader bake");
            exporter::BakeShaderTextures(scene, bakeQuality.maxSize, a.has("bake-lit"), &bakeNote,
                                     [](const std::string& m) {
                                         std::fprintf(stderr, "  %s\n", m.c_str());
                                         const std::string key = "baked texture ";
                                         const size_t at = m.find(key);
                                         if (at != std::string::npos) {
                                             char* end = nullptr;
                                             const long n = std::strtol(m.c_str() + at + key.size(), &end, 10);
                                             if (end && end[0] == '/') {
                                                 const long tot = std::strtol(end + 1, nullptr, 10);
                                                 if (tot > 0) printProgress(static_cast<double>(n) / static_cast<double>(tot), m);
                                             }
                                         }
                                     }, true, !a.has("no-toon-ramp"), {},
                                      bakeQuality.supersample, bakeQuality.islandPadding);
        }
        std::fprintf(stderr, "\n  %s (%.1fs)\n", bakeNote.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - tb0).count());
        LogMemory("after bake");
    }

    std::vector<uint8_t> gpuRgba, plainRgba;   // written after the export, which with --overwrite clears the folder first
    if (a.has("gpu-view")) {   // debugging aid: the same scene through Direct3D 11, written next to the other pictures
        const bool gpuDbg = std::getenv("CARDBRIDGE_DEBUG_GPU") != nullptr;
        if (gpuDbg) std::fprintf(stderr, "[gpu] card %s: family=%s materials=%zu textures=%zu shaders=%zu meshes=%zu parts=%zu\n",
                                 c.path.c_str(), card::FamilyName(c.family), scene.materials.size(),
                                 scene.textures.size(), scene.shaders.size(), scene.meshes.size(), scene.parts.size());
        exporter::GpuView gpu;
        std::string why;
        exporter::GpuOptions go;
        go.preference = a.get("gpu", "auto");
        go.skin = a.get("gpu-skin", "gpu");
        if (a.opt.count("skin-threshold")) go.skinThresholdVerts = static_cast<size_t>(std::atoll(a.get("skin-threshold").c_str()));
        if (!gpu.Init(go, &why)) std::fprintf(stderr, "  gpu view: device init failed: %s\n", why.c_str());
        else if (!gpu.Load(scene, &why)) std::fprintf(stderr, "  gpu view: load failed: %s\n", why.c_str());
        else {
            if (gpuDbg) std::fprintf(stderr, "[gpu] init and load complete; entering first render\n");
            if (!liveSkin.empty()) gpu.SetSkin(&liveSkin);
            std::fprintf(stderr, "  gpu view: %s (%s, skin %s)\n", gpu.adapter().c_str(), gpu.kind() == exporter::GpuKind::Hardware ? "GPU" : "CPU fallback", gpu.gpuSkin() ? "on GPU" : "on CPU");
            if (a.opt.count("layers-dir")) {   // the extra shader layers in that folder, all switched on for the picture
                std::string report;
                auto defs = exporter::LoadShaderLayers({normalizeSlashes(a.get("layers-dir"))});
                gpu.SetLayers(defs, &report);
                for (size_t i = 0; i < gpu.layerCount(); ++i) gpu.SetLayerEnabled(i, true);
                std::fprintf(stderr, "  gpu view: %zu layer(s)%s%s\n", defs.size(), report.empty() ? "" : ", problems: ", report.c_str());
            }
            exporter::ViewCamera cam;
            cam.pitchDeg = 4;
            // Test aid: CARDBRIDGE_GPU_CAM=yaw,pitch,zoom,panX,panY frames the picture (a close-up of the chest, say 0,4,3.5,0,0.2).
            if (const char* gc = std::getenv("CARDBRIDGE_GPU_CAM")) std::sscanf(gc, "%f,%f,%f,%f,%f", &cam.yawDeg, &cam.pitchDeg, &cam.zoom, &cam.panX, &cam.panY);
            std::vector<uint32_t> px;
            auto g0 = std::chrono::steady_clock::now();
            bool ok = gpu.Render(cam, 640, 800, &effHidden, px);
            if (gpuDbg) std::fprintf(stderr, "[gpu] first render returned %d\n", ok ? 1 : 0);
            ok = ok && gpu.Render(cam, 640, 800, &effHidden, px);
            if (ok && a.has("gpu-bones")) {   // the skeleton over the picture (--gpu-bones-helpers: the driven, shape and attach nodes too)
                std::vector<Vec3> posed;
                if (!liveSkin.empty()) for (size_t i = 0; i < scene.bones.size(); ++i) posed.push_back(liveSkin[i].point(scene.bones[i].pos));
                exporter::DrawSkeletonOverlay(scene, posed, gpu, px, 640, 800, -1, -1, a.has("gpu-bones-helpers"), nullptr);
            }
            std::fprintf(stderr, "  gpu view: %s, second frame %.1f ms, %d material(s) without a shader\n", ok ? "ok" : "failed",
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g0).count() / 2, gpu.skippedMaterials());
            for (const auto& m : scene.materials) if (m.shaderIndex < 0) std::fprintf(stderr, "    no shader: %s (%s)\n", m.name.c_str(), m.shader.c_str());
            if (ok && !a.has("no-bake")) {   // the baked textures on plain materials: what a PMX viewer shows (the viewer's X key)
                gpu.SetPlain(true);
                std::vector<uint32_t> pp;
                if (gpu.Render(cam, 640, 800, &effHidden, pp)) {
                    plainRgba.resize(pp.size() * 4);
                    for (size_t i = 0; i < pp.size(); ++i) { plainRgba[i * 4] = (pp[i] >> 16) & 255; plainRgba[i * 4 + 1] = (pp[i] >> 8) & 255; plainRgba[i * 4 + 2] = pp[i] & 255; plainRgba[i * 4 + 3] = 255; }
                }
                gpu.SetPlain(false);
            }
            if (ok) {
                gpuRgba.resize(px.size() * 4);
                for (size_t i = 0; i < px.size(); ++i) { gpuRgba[i * 4] = (px[i] >> 16) & 255; gpuRgba[i * 4 + 1] = (px[i] >> 8) & 255; gpuRgba[i * 4 + 2] = px[i] & 255; gpuRgba[i * 4 + 3] = 255; }
            }
        }
    }

    exporter::ExportFormat format = exporter::ExportFormat::Pmx;
    if (!exporter::ParseExportFormat(a.get("format").empty() ? "pmx" : a.get("format"), format)) {
        std::fprintf(stderr, "--format must be pmx, glb, gltf or obj\n");
        return 1;
    }
    if (format != exporter::ExportFormat::Pmx) {
        // New interchange formats share the same assembled scene and the same bake/pose
        // preparation as PMX; only the serializer differs. PMX keeps its own full option set.
        exporter::ModelExportOptions mo;
        mo.format = format;
        mo.outDir = out;
        mo.overwrite = a.has("overwrite");
        mo.exportTextures = !a.has("no-textures");
        mo.exportArmature = !a.has("no-armature");
        mo.exportSkinning = !a.has("no-skinning");
        mo.exportMorphs = !a.has("no-morphs");
        mo.exportAnimation = a.has("animation");
        mo.separateObjects = !a.has("combine-objects");
        mo.exportMtl = !a.has("no-mtl");
        mo.scale = a.opt.count("scale") ? static_cast<float>(std::atof(a.get("scale").c_str())) : 1.0f;
        if (a.opt.count("simplify")) {
            const double pct = std::atof(a.get("simplify").c_str());
            if (!std::isfinite(pct)) { std::fprintf(stderr, "--simplify needs a number (percent, 0..80)\n"); return 1; }
            mo.simplifyPercent = static_cast<float>(std::clamp(pct, 0.0, 80.0));
        }
        mo.log = [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); };
        mo.progress = [](double f, const std::string& stage) { printProgress(f, "exporting: " + stage); };
        try {
            cbtime::Scope _("export");
            const exporter::ModelExportResult r = exporter::ExportModel(scene, c, mo);
            std::printf("wrote %s (%s)\n  %zu vertices, %zu triangles, %zu materials, %zu bones, %zu textures, %zu morphs\n  manifest: %s\n",
                        r.primaryPath.c_str(), exporter::FormatName(format), r.vertices, r.triangles,
                        r.materials, r.bones, r.textures, r.morphs, r.manifestPath.c_str());
            for (const auto& w : r.warnings) std::fprintf(stderr, "  note: %s\n", w.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "export failed: %s\n", e.what());
            return 3;
        }
        return 0;
    }

    exporter::ExportOptions eo;
    eo.outDir = out;
    eo.overwrite = a.has("overwrite");
    eo.faceMinusZ = !a.has("unity-facing");
    eo.correctionLayer = ao.proportionLayer;   // faithful when empty; corrected when --proportions/--normalize-* were given
    eo.bodyTransferJson = transferJson;
    eo.physics = !a.has("no-physics");
    eo.jiggleBust = !a.has("no-jiggle-bust");          // default on
    eo.jiggleButt = a.has("jiggle-butt");             // default off: the card's butt sway reads as the skirt vibrating
    eo.braAwarePhysics = !a.has("no-bra-physics") && !std::getenv("CARDBRIDGE_NO_BRA_PHYSICS");   // ON by default
    eo.jiggle = jiggle;
    eo.mmdNames = !a.has("no-mmd-names");
    eo.scale = a.opt.count("scale") ? static_cast<float>(std::atof(a.get("scale").c_str())) : 12.5f;
    eo.outfit = ao.outfit >= 0 ? ao.outfit : static_cast<int>(c.status.at("coordinateType").asInt());
    eo.assetLibrary = a.get("library");
    eo.modsFolder = a.get("mods");
    eo.materialFile = a.get("materials");
    if (a.opt.count("upscale")) {
        const int n = std::atoi(a.get("upscale").c_str());
        if (n < 2 || n > 8) { std::fprintf(stderr, "--upscale needs an integer factor 2..8\n"); return 1; }
        eo.upscaleTextures = true;
        eo.upscaleScale = n;
    }
    eo.upscalerExe = a.get("upscaler");
    eo.upscalerModel = a.opt.count("upscale-model") ? a.get("upscale-model") : "waifu2x";
    if (!exporter::IsKnownUpscaler(eo.upscalerModel)) { std::fprintf(stderr, "--upscale-model must be waifu2x or realesrgan\n"); return 1; }
    if (!materialWorkbenchJson.is_null()) eo.materialWorkbench = materialWorkbenchJson;
    if (a.opt.count("simplify")) {
        const double pct = std::atof(a.get("simplify").c_str());
        if (!std::isfinite(pct)) { std::fprintf(stderr, "--simplify needs a number (percent, 0..80)\n"); return 1; }
        eo.simplifyPercent = static_cast<float>(std::clamp(pct, 0.0, 80.0));
    }
    if (a.opt.count("lod-targets")) {   // e.g. --lod-targets 70,40,20,10
        std::string csv = a.get("lod-targets");
        std::vector<float> levels;
        size_t pos = 0;
        while (pos <= csv.size()) {
            const size_t comma = csv.find(',', pos);
            const std::string tok = csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) { const double v = std::atof(tok.c_str()); if (std::isfinite(v)) levels.push_back(static_cast<float>(std::clamp(v, 1.0, 99.0)) / 100.0f); }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (levels.empty()) { std::fprintf(stderr, "--lod-targets needs comma-separated percentages (e.g. 60,30,15,5)\n"); return 1; }
        size_t n = 0;
        for (const float remaining : levels) eo.lodTargets.push_back({"LOD " + std::to_string(++n), remaining});
    } else if (a.has("lods")) {
        eo.lodTargets = {{"LOD 1", 0.6f}, {"LOD 2", 0.3f}, {"LOD 3", 0.15f}, {"LOD 4", 0.05f}};
    }
    eo.log = [](const std::string& m) { std::fprintf(stderr, "  %s\n", m.c_str()); };
    eo.progress = [](double f, const std::string& stage) { printProgress(f, "exporting: " + stage); };
    try {
        exporter::ExportResult r;
        {
            cbtime::Scope _("export");
            r = exporter::ExportScene(scene, c, eo);
        }
        LogMemory("after export");
        if (!gpuRgba.empty()) stbi_write_png((out + "/gpu_view.png").c_str(), 640, 800, 4, gpuRgba.data(), 640 * 4);
        if (!plainRgba.empty()) stbi_write_png((out + "/gpu_plain.png").c_str(), 640, 800, 4, plainRgba.data(), 640 * 4);
        double secs = std::chrono::duration<double>(t1 - t0).count();
        std::printf("wrote %s\n  %zu vertices, %zu triangles, %zu materials, %zu bones, %zu textures; %zu of %zu card items loaded (assembled in %.1fs)\n  manifest: %s\n",
                    r.pmxPath.c_str(), r.vertices, r.triangles, r.materials, r.bones, r.textures, parts - failed - 0, parts, secs, r.manifestPath.c_str(), r.previewPath.c_str());
        for (const auto& lod : r.lods)
            std::printf("  %s: %s (%zu triangles, %zu vertices)\n", lod.label.c_str(), lod.file.c_str(), lod.triangles, lod.vertices);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "export failed: %s\n", e.what());
        return 3;
    }
    return 0;
}

// Benchmark the triangle-reduction path on a synthetic closed mesh (a subdivided icosahedron projected
// onto a sphere): the same code path the LODs take, deterministic, no card or library needed.
int cmdSimplifyBench(const Args& a) {
    const int subdiv = a.opt.count("subdiv") ? std::atoi(a.get("subdiv").c_str()) : 6;
    if (subdiv < 0 || subdiv > 8) { std::fprintf(stderr, "--subdiv 0..8\n"); return 1; }
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    std::vector<std::array<double, 3>> verts = {
        {-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
    };
    for (auto& v : verts) {
        const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        v = {v[0] / n, v[1] / n, v[2] / n};
    }
    std::vector<std::array<int, 3>> faces = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
    };
    for (int s = 0; s < subdiv; ++s) {
        std::map<uint64_t, int> midpoint;
        auto mid = [&](int a, int b) {
            const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | static_cast<uint64_t>(std::max(a, b));
            auto it = midpoint.find(key);
            if (it != midpoint.end()) return it->second;
            const auto& va = verts[static_cast<size_t>(a)], & vb = verts[static_cast<size_t>(b)];
            std::array<double, 3> m{(va[0] + vb[0]) * 0.5, (va[1] + vb[1]) * 0.5, (va[2] + vb[2]) * 0.5};
            const double n = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            verts.push_back({m[0] / n, m[1] / n, m[2] / n});
            midpoint.emplace(key, static_cast<int>(verts.size()) - 1);
            return static_cast<int>(verts.size()) - 1;
        };
        std::vector<std::array<int, 3>> next;
        next.reserve(faces.size() * 4);
        for (const auto& f : faces) {
            const int a = mid(f[0], f[1]), b = mid(f[1], f[2]), c = mid(f[2], f[0]);
            next.push_back({f[0], a, c});
            next.push_back({a, f[1], b});
            next.push_back({c, b, f[2]});
            next.push_back({a, b, c});
        }
        faces = std::move(next);
    }
    assemble::SceneMesh m;
    m.name = "bench";
    m.reshapable = false;
    for (const auto& v : verts) {
        assemble::SceneVertex sv;
        sv.pos = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
        sv.normal = sv.pos;
        sv.uv = {static_cast<float>(v[0] * 0.5 + 0.5), static_cast<float>(v[2] * 0.5 + 0.5)};
        sv.uv1 = sv.uv; sv.uv2 = sv.uv; sv.uv3 = sv.uv;
        sv.bone[0] = 0; sv.weight[0] = 1.0f;
        m.vertices.push_back(sv);
    }
    assemble::SceneSubmesh sm;
    sm.material = 0;
    for (const auto& f : faces) { sm.indices.push_back(static_cast<uint32_t>(f[0])); sm.indices.push_back(static_cast<uint32_t>(f[1])); sm.indices.push_back(static_cast<uint32_t>(f[2])); }
    m.submeshes.push_back(std::move(sm));
    std::printf("simplify-bench: %zu triangles (subdiv %d)\n", faces.size(), subdiv);
    for (const double pct : {20.0, 40.0, 60.0, 80.0}) {
        assemble::SceneMesh copy = m;
        mesh::SimplifyOptions so;
        so.maxReduction = static_cast<float>(pct / 100.0);
        const auto t0 = std::chrono::steady_clock::now();
        const mesh::SimplifyMeshStats st = mesh::SimplifyMesh(copy, so, nullptr);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  -%.0f%%: %zu -> %zu triangles in %.3fs (%.1f Mtri/s), %zu collapsed\n", pct, st.originalTriangles, st.resultTriangles, secs,
                    st.originalTriangles / 1e6 / std::max(secs, 1e-9), st.collapses);
    }
    return 0;
}

} // namespace

nlohmann::json pmxDescribe(const pmx::Model& m) {
    nlohmann::json j;
    j["version"] = m.format.version;
    j["encoding"] = m.format.encoding();
    j["globals"] = std::vector<int>(std::begin(m.format.globals), std::end(m.format.globals));
    j["additionalUv"] = m.format.additionalUv();
    j["trailingBytes"] = static_cast<size_t>(m.format.trailing.size());
    j["counts"] = {
        {"vertices", m.vertices.size()}, {"indices", m.indices.size()},
        {"textures", m.textures.size()}, {"materials", m.materials.size()},
        {"bones", m.bones.size()}, {"morphs", m.morphs.size()},
        {"frames", m.frames.size()}, {"rigidBodies", m.rigidBodies.size()},
        {"joints", m.joints.size()},
    };
    size_t frameItems = 0;
    for (const auto& f : m.frames) frameItems += f.items.size();
    j["counts"]["frameItems"] = frameItems;
    std::map<std::string, int> deform, morphs, joints;
    for (const auto& v : m.vertices) deform[std::to_string(static_cast<int>(v.deformType))]++;
    for (const auto& mo : m.morphs) morphs[std::to_string(static_cast<int>(mo.type))]++;
    for (const auto& jt : m.joints) joints[std::to_string(static_cast<int>(jt.type))]++;
    j["deformTypes"] = deform;
    j["morphTypes"] = morphs;
    j["jointTypes"] = joints;
    int localAxis = 0, fixedAxis = 0, ikBones = 0, appendBones = 0;
    for (const auto& b : m.bones) {
        if (b.flags & 0x0800) ++localAxis;
        if (b.flags & 0x0400) ++fixedAxis;
        if (b.flags & 0x0020) ++ikBones;
        if (b.flags & 0x0300) ++appendBones;
    }
    j["boneFlags"] = {{"localAxis", localAxis}, {"fixedAxis", fixedAxis}, {"ik", ikBones}, {"append", appendBones}};
    return j;
}

void pmxPrintDescribe(const nlohmann::json& j) {
    const auto& c = j["counts"];
    std::printf("PMX %.2f, encoding %d, globals", j["version"].get<float>(), j["encoding"].get<int>());
    for (int g : j["globals"].get<std::vector<int>>()) std::printf(" %d", g);
    std::printf("; additional UV %d; trailing %zu bytes\n", j["additionalUv"].get<size_t>(), j["trailingBytes"].get<size_t>());
    std::printf("vertices %zu indices %zu textures %zu materials %zu bones %zu morphs %zu frames %zu (%zu items) rigid bodies %zu joints %zu\n",
                c["vertices"].get<size_t>(), c["indices"].get<size_t>(), c["textures"].get<size_t>(),
                c["materials"].get<size_t>(), c["bones"].get<size_t>(), c["morphs"].get<size_t>(),
                c["frames"].get<size_t>(), c["frameItems"].get<size_t>(),
                c["rigidBodies"].get<size_t>(), c["joints"].get<size_t>());
    auto printMap = [](const char* label, const nlohmann::json& map) {
        std::printf("%s:", label);
        for (auto it = map.begin(); it != map.end(); ++it) std::printf(" %s=%d", it.key().c_str(), it.value().get<int>());
        std::printf("\n");
    };
    printMap("deform types", j["deformTypes"]);
    printMap("morph types", j["morphTypes"]);
    printMap("joint types", j["jointTypes"]);
    const auto& bf = j["boneFlags"];
    std::printf("bone flags: localAxis=%d fixedAxis=%d IK=%d append=%d\n",
                bf["localAxis"].get<int>(), bf["fixedAxis"].get<int>(),
                bf["ik"].get<int>(), bf["append"].get<int>());
}

int cmdPmx(const Args& a) {
    if (a.positional.size() < 3) { usage(); return 1; }
    const std::string& sub = a.positional[1];
    const std::string path = normalizeSlashes(a.positional[2]);
    try {
        Bytes original = fileio::ReadFile(path);
        pmx::Model m = pmx::ReadPmx(original.data(), original.size());
        nlohmann::json info = pmxDescribe(m);
        if (sub == "info") {
            if (a.has("json")) std::printf("%s\n", info.dump(2).c_str());
            else pmxPrintDescribe(info);
            return 0;
        }
        if (sub == "roundtrip") {
            if (a.positional.size() < 4) { usage(); return 1; }
            const std::string out = normalizeSlashes(a.positional[3]);
            Bytes written = pmx::WritePmx(m);
            fileio::WriteFile(out, written.data(), written.size());
            pmx::Model again = pmx::ReadPmx(written.data(), written.size());
            nlohmann::json info2 = pmxDescribe(again);
            const bool same = info == info2;
            std::printf("read %zu bytes -> wrote %zu bytes (%s); reimport structure: %s\n",
                        original.size(), written.size(),
                        original == written ? "byte-identical" : "re-encoded", same ? "preserved" : "CHANGED");
            if (!same) std::printf("before: %s\nafter:  %s\n", info.dump().c_str(), info2.dump().c_str());
            return same ? 0 : 3;
        }
        if (sub == "restore-body") {
            if (a.positional.size() < 4) { usage(); return 1; }
            const std::string out = normalizeSlashes(a.positional[3]);
            assemble::BodyTemplateOptions opt;
            opt.libraryRoot = normalizeSlashes(a.get("library"));
            opt.modsRoot = normalizeSlashes(a.get("mods"));
            const char* la = std::getenv("LOCALAPPDATA");
            opt.modCachePath = la ? std::string(la) + "/CardBridge/modindex.json" : std::string();
            assemble::BodyTemplate body;
            std::string error;
            if (!assemble::LoadBodyTemplate(opt, body, error)) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 3;
            }
            const pmx::RestoreResult r = pmx::RestoreBody(m, body);
            for (const std::string& w : r.warnings) std::printf("  %s\n", w.c_str());
            if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 3; }
            const Bytes written = pmx::WritePmx(m);
            fileio::WriteFile(out, written.data(), written.size());
            std::printf("restored body from %s: +%zu vertices, +%zu indices -> %s\n",
                        body.source.c_str(), r.addedVertices, r.addedIndices, out.c_str());
            return 0;
        }
        if (sub == "apply-asset") {
            if (a.positional.size() < 4) { usage(); return 1; }
            const std::string out = normalizeSlashes(a.positional[3]);
            const std::string key = a.get("key");
            if (key.empty()) { std::fprintf(stderr, "error: --key <assetKey> is required (see 'assets list')\n"); return 1; }
            library::CatalogOptions catOpt;
            catOpt.libraryRoot = normalizeSlashes(a.get("library"));
            catOpt.modsRoot = normalizeSlashes(a.get("mods"));
            const char* la = std::getenv("LOCALAPPDATA");
            catOpt.cachePath = la ? std::string(la) + "/CardBridge/assetcatalog.json" : std::string();
            catOpt.modCachePath = la ? std::string(la) + "/CardBridge/modindex.json" : std::string();
            std::vector<std::string> issues;
            const library::AssetCatalog catalog = library::BuildCatalog(catOpt, &issues);
            const library::AssetRecord* found = nullptr;
            for (const auto& rec : catalog.records) if (rec.key == key) { found = &rec; break; }
            if (!found) { std::fprintf(stderr, "error: asset key not found in the catalog: %s\n", key.c_str()); return 1; }
            library::AssetTemplateOptions tOpt;
            tOpt.libraryRoot = catOpt.libraryRoot;
            tOpt.modsRoot = catOpt.modsRoot;
            tOpt.modCachePath = catOpt.modCachePath;
            library::AssetTemplate asset;
            std::string error;
            if (!library::LoadAssetTemplate(*found, tOpt, asset, error)) {
                std::fprintf(stderr, "error: %s\n", error.c_str());
                return 3;
            }
            pmx::AssetTransform tr;
            tr.translation = {static_cast<float>(std::atof(a.get("tx", "0").c_str())),
                              static_cast<float>(std::atof(a.get("ty", "0").c_str())),
                              static_cast<float>(std::atof(a.get("tz", "0").c_str()))};
            tr.rotationDeg = {static_cast<float>(std::atof(a.get("rx", "0").c_str())),
                              static_cast<float>(std::atof(a.get("ry", "0").c_str())),
                              static_cast<float>(std::atof(a.get("rz", "0").c_str()))};
            tr.scale = {static_cast<float>(std::atof(a.get("sx", "1").c_str())),
                        static_cast<float>(std::atof(a.get("sy", "1").c_str())),
                        static_cast<float>(std::atof(a.get("sz", "1").c_str()))};
            library::AssetTemplate aligned = pmx::ApplyTransform(asset, tr);
            pmx::BoneMatchOptions mo;
            mo.allowGeometric = a.has("auto") || a.has("geometric");
            pmx::AssetFitReport fit = pmx::AnalyzeAssetFit(aligned, m, mo);
            // Optional explicit bone mapping: --map "source>destination,..." (names, not indices).
            const std::string manual = a.get("map");
            if (!manual.empty()) {
                size_t start = 0;
                while (start < manual.size()) {
                    const size_t end = manual.find(',', start);
                    const std::string pair = manual.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    const size_t sep = pair.find('>');
                    if (sep != std::string::npos) {
                        const std::string src = pair.substr(0, sep), dst = pair.substr(sep + 1);
                        int si = -1, ti = -1;
                        for (size_t i = 0; i < aligned.boneNames.size(); ++i) if (aligned.boneNames[i] == src) { si = static_cast<int>(i); break; }
                        for (size_t i = 0; i < m.bones.size(); ++i)
                            if (m.bones[i].name == dst || m.bones[i].nameEn == dst) { ti = static_cast<int>(i); break; }
                        if (si >= 0 && ti >= 0 && static_cast<size_t>(si) < fit.mappings.size()) {
                            auto& mapping = fit.mappings[static_cast<size_t>(si)];
                            mapping.target = ti; mapping.method = pmx::MatchMethod::Alias;
                            mapping.confidence = 1.0f; mapping.ambiguous = false; mapping.note = "manual mapping";
                            fit.warnings.erase(std::remove_if(fit.warnings.begin(), fit.warnings.end(),
                                [&](const std::string& w) { return w.find(src) != std::string::npos; }), fit.warnings.end());
                        } else {
                            std::fprintf(stderr, "warning: manual mapping '%s' refers to unknown bones\n", pair.c_str());
                        }
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                fit.matchedBones = fit.ambiguousBones = fit.missingBones = 0;
                for (size_t i = 0; i < aligned.boneNames.size(); ++i) {
                    bool required = false;
                    for (const auto& v : aligned.vertices)
                        for (int k = 0; k < 4; ++k)
                            if (v.weight[k] > 0.0f && v.bone[k] == static_cast<int>(i)) { required = true; break; }
                    if (!required) continue;
                    const auto& mp = fit.mappings[i];
                    if (mp.ok()) ++fit.matchedBones;
                    else if (mp.ambiguous) ++fit.ambiguousBones;
                    else ++fit.missingBones;
                }
                if (fit.missingBones) { fit.status = pmx::FitStatus::Incompatible; fit.compatible = false; fit.reason = "incompatible skeleton after manual mapping"; }
                else if (fit.ambiguousBones) { fit.status = pmx::FitStatus::RequiresManualMapping; fit.compatible = false; fit.reason = "ambiguous mappings remain"; }
                else { fit.status = fit.warnings.empty() ? pmx::FitStatus::Compatible : pmx::FitStatus::CompatibleWithWarnings; fit.compatible = true; fit.reason.clear(); }
            }
            if (a.has("json-report")) {
                nlohmann::json j;
                j["asset"] = found->key;
                j["status"] = pmx::FitStatusName(fit.status);
                j["reason"] = fit.reason;
                j["required"] = fit.requiredBones;
                j["matched"] = fit.matchedBones;
                j["ambiguous"] = fit.ambiguousBones;
                j["missing"] = fit.missingBones;
                j["scale"] = fit.scale;
                j["hierarchy"] = fit.hierarchyCompatibility;
                j["indexCapacityOk"] = fit.indexCapacityOk;
                j["materialCompatibility"] = fit.materialCompatibility;
                j["materials"] = aligned.materials.size();
                j["textures"] = aligned.textures.size();
                j["warnings"] = fit.warnings;
                auto& rows = j["mappings"] = nlohmann::json::array();
                for (const auto& mp : fit.mappings) {
                    nlohmann::json e;
                    e["source"] = mp.source < 0 ? std::string() : aligned.boneNames[static_cast<size_t>(mp.source)];
                    e["target"] = mp.target < 0 ? std::string() : m.bones[static_cast<size_t>(mp.target)].name;
                    e["method"] = pmx::MatchMethodName(mp.method);
                    e["confidence"] = mp.confidence;
                    e["deviation"] = mp.deviation;
                    e["ambiguous"] = mp.ambiguous;
                    e["note"] = mp.note;
                    rows.push_back(std::move(e));
                }
                std::printf("%s\n", j.dump(2).c_str());
            } else {
                std::printf("asset %s: %s (%d/%d required bones, %d ambiguous, %d missing), scale %.3f, hierarchy %.2f\n",
                            found->name.c_str(), pmx::FitStatusName(fit.status), fit.matchedBones,
                            fit.requiredBones, fit.ambiguousBones, fit.missingBones, fit.scale,
                            fit.hierarchyCompatibility);
                for (const std::string& w : fit.warnings) std::printf("  %s\n", w.c_str());
            }
            if (!fit.compatible) {
                std::fprintf(stderr, "error: %s\n", fit.reason.c_str());
                return fit.status == pmx::FitStatus::RequiresManualMapping ? 4 : 3;
            }
            if (a.has("no-textures")) {
                for (auto& mat : aligned.materials) {
                    mat.diffuseTexture = mat.toonTexture = mat.sphereTexture = -1;
                }
                aligned.textures.clear();
            }
            // Transactional: only a fully validated, successfully applied copy
            // replaces the loaded document. A failure leaves `m` untouched.
            pmx::Model working = m;
            const pmx::AssetApplyResult r = pmx::ApplyAsset(working, aligned, fit);
            if (!r.ok) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 3; }
            for (const std::string& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
            if (r.importedTextureBytes.empty()) {
                const Bytes written = pmx::WritePmx(working);
                fileio::WriteFile(out, written.data(), written.size());
            } else {
                const size_t slash = out.rfind('/');
                const std::string parent = slash == std::string::npos ? std::string(".") : out.substr(0, slash);
                std::string stem = slash == std::string::npos ? out : out.substr(slash + 1);
                const size_t dot = stem.rfind('.');
                if (dot != std::string::npos) stem = stem.substr(0, dot);
                std::vector<Bytes> overrides(working.textures.size());
                for (size_t i = 0; i < r.importedTextureBytes.size(); ++i)
                    if (r.textureStart + i < overrides.size()) overrides[r.textureStart + i] = r.importedTextureBytes[i];
                const std::string folderPmx = pmx::ExportDocument(working, "", parent, stem, overrides);
                if (folderPmx != out) {   // keep the exact requested output path
                    const Bytes bytes = fileio::ReadFile(folderPmx);
                    fileio::WriteFile(out, bytes.data(), bytes.size());
                }
            }
            m = std::move(working);
            std::printf("applied %s: +%zu vertices, +%zu indices, +%zu materials, +%zu textures -> %s\n",
                        found->name.c_str(), r.addedVertices, r.addedIndices, r.addedMaterials,
                        r.addedTextures, out.c_str());
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
    usage();
    return 1;
}

namespace {

int transferIdFromName(const std::string& name) {
    for (int i = 0; i < measure::kTransferCount; ++i) {
        const std::string n = measure::TransferName(i);
        std::string flat;
        for (char c : n) if (c != ' ' && c != '-' && c != '/') flat.push_back(static_cast<char>(std::tolower(c)));
        if (flat == name || n == name || std::to_string(i) == name) return i;
    }
    return -1;
}

void printMeasurements(const measure::TransferSet& set) {
    for (int i = 0; i < measure::kTransferCount; ++i) {
        const auto& v = set[static_cast<size_t>(i)];
        std::printf("  %-20s %10.2f %-3s  %s%s\n", measure::TransferName(i), v.value, measure::TransferUnit(i),
                    v.valid ? "" : "unavailable: ", v.source.c_str());
    }
}

} // namespace

int cmdMeasure(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    card::Card c;
    int code = 0;
    if (!loadCard(normalizeSlashes(a.positional[1]), c, code)) return code;
    if (!c.supported) { std::fprintf(stderr, "%s: %s\n", c.path.c_str(), c.supportNote.c_str()); return 2; }
    Context ctx;
    if (!buildContext(a, ctx)) return 3;
    assemble::AssembleOptions ao;
    ao.sourceGame = ctx.game;
    const assemble::Scene scene = assemble::Assemble(c, *ctx.resolver, ao);
    const auto set = measure::MeasureTransfer(scene);
    std::printf("body measurements for %s:\n", c.displayName().c_str());
    printMeasurements(set);
    if (a.has("json")) {
        nlohmann::json j;
        for (int i = 0; i < measure::kTransferCount; ++i) {
            const auto& v = set[static_cast<size_t>(i)];
            j[measure::TransferName(i)] = {{"value", v.value}, {"unit", measure::TransferUnit(i)}, {"valid", v.valid}, {"source", v.source}};
        }
        std::printf("%s\n", j.dump(2).c_str());
    }
    return 0;
}

int cmdTransfer(const Args& a) {
    if (a.positional.size() < 4) { usage(); return 1; }
    const std::string outPath = normalizeSlashes(a.positional[3]);
    card::Card ref, tgt;
    int code = 0;
    if (!loadCard(normalizeSlashes(a.positional[1]), ref, code)) return code;
    if (!loadCard(normalizeSlashes(a.positional[2]), tgt, code)) return code;
    if (!ref.supported || !tgt.supported) {
        std::fprintf(stderr, "one of the cards is unsupported\n");
        return 2;
    }
    Context ctx;
    if (!buildContext(a, ctx)) return 3;
    assemble::AssembleOptions ao;
    ao.sourceGame = ctx.game;
    std::fprintf(stderr, "transfer: assembling reference %s\n", ref.displayName().c_str());
    const assemble::Scene refScene = assemble::Assemble(ref, *ctx.resolver, ao);
    std::fprintf(stderr, "transfer: assembling target %s\n", tgt.displayName().c_str());
    assemble::Scene tgtScene = assemble::Assemble(tgt, *ctx.resolver, ao);
    const auto refSet = measure::MeasureTransfer(refScene);
    const auto tgtSet = measure::MeasureTransfer(tgtScene);

    assemble::transfer::Spec spec;
    const std::string select = a.get("select");
    if (!select.empty()) {
        size_t pos = 0;
        while (pos <= select.size()) {
            const size_t comma = select.find(',', pos);
            const std::string token = select.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const int id = transferIdFromName(token);
            if (id >= 0) spec.selected[static_cast<size_t>(id)] = true;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    } else {
        for (int i = 0; i < measure::kTransferCount; ++i)
            spec.selected[static_cast<size_t>(i)] = refSet[static_cast<size_t>(i)].valid && tgtSet[static_cast<size_t>(i)].valid;
    }
    for (int i = 0; i < measure::kTransferCount; ++i) spec.refine[static_cast<size_t>(i)] = a.has("refine");
    std::vector<float> body = assemble::transfer::BodySlidersOf(tgt);
    const std::vector<float> cardBody = body;
    spec = assemble::transfer::Build(refScene, ref, refSet, tgtScene, tgt, tgtSet, body, spec);

    ao.bodyShape = body;
    const auto measureAt = [&](const std::vector<float>& trial) {
        ao.bodyShape = trial;
        if (!assemble::Reshape(tgtScene, tgt, *ctx.resolver, ao)) return measure::TransferSet{};
        return measure::MeasureTransfer(tgtScene);
    };
    assemble::transfer::Refine(spec, body, cardBody, measureAt);
    // Refinement only reports rows it actually solved. Measure the applied result once with the full
    // layer so the spec's "achieved" values are honest in the default (controls-only) path too.
    {
        std::vector<float> appliedBody = body;
        for (const auto& op : spec.ops)
            if (op.kind == assemble::kBodySlider && op.index >= 0 && static_cast<size_t>(op.index) < appliedBody.size())
                appliedBody[static_cast<size_t>(op.index)] = op.value;
        assemble::AssembleOptions finalAo = ao;
        finalAo.bodyShape = appliedBody;
        assemble::ProportionLayer none;
        finalAo.proportionLayer = assemble::transfer::Merge(none, spec.ops, true);
        if (assemble::Reshape(tgtScene, tgt, *ctx.resolver, finalAo)) {
            const auto applied = measure::MeasureTransfer(tgtScene);
            for (int i = 0; i < measure::kTransferCount; ++i)
                if (spec.selected[static_cast<size_t>(i)]) {
                    spec.achievedValue[static_cast<size_t>(i)] = applied[static_cast<size_t>(i)].value;
                    spec.achievedValid[static_cast<size_t>(i)] = applied[static_cast<size_t>(i)].valid;
                }
        }
    }
    const std::string text = assemble::transfer::ToJson(spec);
    fileio::WriteFile(outPath, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    std::printf("wrote %s (%zu transfer op(s))\n", outPath.c_str(), spec.ops.size());
    for (int i = 0; i < measure::kTransferCount; ++i)
        if (spec.selected[static_cast<size_t>(i)])
            std::printf("  %-20s reference %8.2f%s -> achieved %8.2f%s\n", measure::TransferName(i), spec.targetValue[i],
                        measure::TransferUnit(i), spec.achievedValue[i], measure::TransferUnit(i));
    for (const auto& note : spec.notes) std::printf("  note: %s\n", note.c_str());
    std::printf("apply with: cardbridge convert <target.png> --library ... --out ... --transfer %s\n", outPath.c_str());
    return 0;
}

int cmdAssets(const Args& a) {
    if (a.positional.size() < 2) { usage(); return 1; }
    const std::string& sub = a.positional[1];
    if (sub != "build" && sub != "list") { usage(); return 1; }
    const std::string library = normalizeSlashes(a.get("library"));
    const std::string mods = normalizeSlashes(a.get("mods"));
    if (library.empty()) { std::fprintf(stderr, "error: --library <dir> is required\n"); return 1; }

    std::string cache = normalizeSlashes(a.get("cache"));
    if (cache.empty()) {
        const char* la = std::getenv("LOCALAPPDATA");
        cache = la ? std::string(la) + "/CardBridge/assetcatalog.json" : std::string();
    }
    library::CatalogOptions opt;
    opt.libraryRoot = library;
    opt.modsRoot = mods;
    opt.cachePath = cache;
    opt.modCachePath = defaultCache();
    std::vector<std::string> issues;
    const library::AssetCatalog cat = library::BuildCatalog(opt, &issues,
        [](size_t done, size_t total) {
            if (total) std::fprintf(stderr, "indexing mods %zu / %zu\n", done, total);
        });
    for (const std::string& issue : issues)
        std::fprintf(stderr, "warning: %s\n", issue.c_str());

    const auto counts = library::CategoryCounts(cat);
    if (sub == "build") {
        if (a.has("json")) {
            nlohmann::json j;
            for (auto c : counts) j[library::CategoryName(c.first)] = c.second;
            j["total"] = cat.size();
            std::printf("%s\n", j.dump(2).c_str());
        } else {
            std::printf("asset catalog: %zu items (%zu duplicates removed)\n", cat.size(), cat.deduplicated);
            for (auto c : counts)
                std::printf("  %-12s %zu\n", library::CategoryName(c.first), c.second);
            std::printf("cache: %s\n", cache.c_str());
        }
        return 0;
    }

    library::AssetCategory category = library::AssetCategory::Count;
    if (!a.get("category").empty()) category = library::CategoryFromName(a.get("category"));
    const size_t limit = static_cast<size_t>(std::max(0, std::atoi(a.get("limit", "0").c_str())));
    const auto rows = library::Query(cat, category, a.get("search"), limit);
    if (a.has("json")) {
        nlohmann::json j = nlohmann::json::array();
        for (const library::AssetRecord* r : rows) {
            nlohmann::json e;
            e["key"] = r->key; e["category"] = library::CategoryName(r->category);
            e["subcategory"] = r->subcategory; e["name"] = r->name;
            e["id"] = r->id; e["mod"] = r->mod; e["modGuid"] = r->modGuid;
            e["modName"] = r->modName; e["bundle"] = r->bundle; e["asset"] = r->asset;
            e["parent"] = r->parent; e["textureDeps"] = r->textureDeps; e["flags"] = r->flags;
            j.push_back(std::move(e));
        }
        std::printf("%s\n", j.dump(2).c_str());
    } else {
        std::printf("%zu asset(s)\n", rows.size());
        for (const library::AssetRecord* r : rows)
            std::printf("  %-11s %-18s %-40s id=%lld %s%s\n", library::CategoryName(r->category),
                        r->subcategory.c_str(), r->name.c_str(), static_cast<long long>(r->id),
                        r->mod ? "mod:" : "base", r->mod ? r->modGuid.c_str() : "");
    }
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    std::vector<std::string> wide = utf8Args();
    std::vector<char*> ptrs;
    for (auto& s : wide) ptrs.push_back(s.data());
    argc = static_cast<int>(ptrs.size());
    argv = ptrs.data();
#endif
    Args a = parseArgs(argc, argv, {"overwrite", "minimal", "hashes", "json", "help", "version", "no-bake", "bake-flat", "bake-lit", "no-grade", "no-toon-ramp", "gpu-view", "nipple-inside", "nude", "apose", "tpose","unity-facing", "no-physics", "auto-cloth", "no-auto-cloth", "no-mmd-names", "gpu-bones", "gpu-bones-helpers", "no-jiggle-bust", "no-jiggle-butt", "jiggle-butt", "no-bra-physics", "replace-accessories", "card-nipples", "include-hidden", "with-hair-accessories", "with-abmx", "render-thumbnail", "no-mods", "lods", "normalize-head-proportions", "normalize-body-proportions", "normalize-face-proportions", "normalize-overall", "print-diagnostics", "fix-garment-intersections", "no-fix-garment-intersections", "refine", "auto", "geometric", "json-report", "no-textures"});
    if (a.has("version")) { std::printf("cardbridge %s\n", kVersion); return 0; }
    if (a.positional.empty() || a.has("help")) { usage(); return a.has("help") ? 0 : 1; }
    const std::string& cmd = a.positional[0];
    const auto tMain0 = std::chrono::steady_clock::now();
    int rc = 1;
    try {
        if (cmd == "convert") rc = cmdConvert(a);
        else if (cmd == "inspect") rc = cmdInspect(a);
        else if (cmd == "measure") rc = cmdMeasure(a);
        else if (cmd == "transfer") rc = cmdTransfer(a);
        else if (cmd == "clothes") rc = cmdClothes(a);
        else if (cmd == "library") rc = cmdLibrary(a);
        else if (cmd == "assets") rc = cmdAssets(a);
        else if (cmd == "pmx") rc = cmdPmx(a);
        else if (cmd == "shader") rc = cmdShader(a);
        else if (cmd == "shader-run") rc = cmdShaderRun(a);
        else if (cmd == "simplify-bench") { rc = cmdSimplifyBench(a); return rc; }
        else if (cmd == "skin-bench") {
            const int repeats = a.opt.count("repeats") ? std::atoi(a.get("repeats").c_str()) : 30;
            std::string summary;
            exporter::RunSkinBenchmark(repeats, &summary, a.get("gpu", "auto"));
            if (!summary.empty()) std::printf("(workloads are synthetic; see SkinBench for counts/distributions)\n");
            return 0;
        } else { usage(); return 1; }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        rc = 3;
    }
    cbtime::Report(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tMain0).count());
    return rc;
}
