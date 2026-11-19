#include "export/FaceMorphs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <stb_image_write.h>

#include "assemble/Scene.h"
#include "core/FileIo.h"
#include "core/Bytes.h"

namespace cardbridge::exporter {

// ---------------------------------------------------------------- semantic naming --------------------------------------------------

const std::map<std::string, std::string>& FaceStemLabels() {
    // The core words the Koikatsu head prefab's face blend shapes use, and what each means. Romanised Japanese game shorthand; where the
    // word is not a word at all (gimo, gyu is a guess at ギュッ), the honest answer is that the meaning is not documented anywhere this
    // tool can read, and those stay out of the table on purpose: an unknown morph keeps an "Unknown" name rather than a made-up one.
    // State (_op/_cl), set numbers (02), size tags (_s/_l/_ss) and the side tag (a trailing capital L/R) are parsed off before the lookup.
    static const std::map<std::string, std::string> kLabels = {
        {"def", "Neutral"},
        {"egao", "Smile"},          // 笑顔
        {"bisyou", "Bashful Smile"},   // 微笑
        {"uresi", "Happy"},         // 嬉しい
        {"doki", "Flustered"},      // ドキドキ (a pounding heart)
        {"ikari", "Angry"},         // 怒り
        {"sinken", "Serious"},      // 真剣
        {"keno", "Disgust"},        // 嫌悪
        {"sabisi", "Lonely"},       // 寂しい
        {"aseri", "Worried"},       // 焦り (also the game's own 心配する pattern for the standard morph)
        {"human", "Pout"},          // the game's ムッ pattern (verified against the standard morph table)
        {"akire", "Exasperated"},   // 呆れ
        {"odoro", "Surprised"},     // 驚く
        {"doya", "Grin"},           // ドヤ
        {"pero", "Tongue Out"},     // ぺろっ
        {"name", "Lick"},           // 舐め
        {"kuwae", "Bite"},          // 咥え
        {"kisu", "Kiss"},           // キス
        {"a", "A"}, {"i", "I"}, {"u", "U"}, {"e", "E"}, {"o", "O"}, {"n", "Mm"},   // あいうえおん
        {"neko", "Cat"},            // 猫 (ω)
        {"san", "Triangle"},        // 三角 (▲)
        {"niko", "Smile"},          // にこ (mouth corners up / にやり３)
        {"mogu", "Mumble"},         // モグモグ
        {"tabe", "Eat"},            // 食べる
        {"ahe", "Ahegao"},          // アヘ
        {"ahe_deep", "Ahegao Deep"},
        {"ahe_light", "Ahegao Light"},
        {"hyottoko", "Hyottoko"},   // ひょっとこ (the farce mask)
        {"gag", "Gag"},             // the gag-eyes pattern (spiral / x eyes objects)
        {"gyu", "Squeeze"},         // ギュッ (squeezed shut; the second set and the sides exist too)
        {"gyul", "Squeeze Left"},
        {"gyur", "Squeeze Right"},
        {"winkl", "Wink Left"},     // the rig's own left / right wink (in the eye blend shape names)
        {"winkr", "Wink Right"},
        {"oko", "Angry"},           // おこ (the childish-cute word for angry, in the brow list)
        {"koma", "Troubled"},       // 困る
        {"komaru", "Troubled"},     // 困る (in the eye list)
        {"rakutan", "Dismayed"},    // 落胆
        {"huan", "Uneasy"},         // 不安 (also read as 厄介, same face)
        {"tere", "Shy"},            // 照れ
        {"setunai", "Pained"},      // 切ない
        {"kurusi", "Anguished"},    // 苦しい
        {"kanasi", "Sad"},          // 悲しい
        {"naki", "Crying"},         // 泣き
        {"sian", "Pensive"},        // 思案
        {"tmara", "Jito-eye"},      // the "jitome" suspicious half-lidded look
        {"tumara", "Jito-eye"},
        {"i_alt", "I Alt"},         // an alternate い shape (the teeth position differs)
        // Deliberately NOT mapped: "gimo" (the brow list's gimoL / gimoR shapes: meaning not documented anywhere this tool can read).
    };
    return kLabels;
}

namespace {

std::string lower(const std::string& s) { std::string out = s; for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return out; }
bool allDigits(const std::string& s) { if (s.empty()) return false; for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false; return true; }

// Strips a trailing run of ASCII digits; returns the digits ("" when none) and leaves s without them.
std::string takeDigits(std::string& s) {
    size_t i = s.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
    if (i == s.size()) return {};
    std::string d = s.substr(i);
    s = s.substr(0, i);
    return d;
}

// The category word for a panel: 1 brows, 2 eyes, 3 mouth, anything else face.
const char* CategoryWord(int panel) { return panel == 1 ? "Brow" : panel == 2 ? "Eye" : panel == 3 ? "Mouth" : "Face"; }

struct NameAllocator {
    std::set<std::string> used;
    int unknownMorph[5] = {};
    int unknownShape[5] = {};
    std::string claim(const std::string& want) {
        if (used.insert(want).second) return want;
        for (int i = 2;; ++i) {
            const std::string n = want + " " + std::to_string(i);
            if (used.insert(n).second) return n;
        }
    }
    std::string unknownName(int panel, const char* kind) {
        const char* cat = CategoryWord(panel);
        const bool shape = kind[0] == 'S';
        for (;;) {
            int& c = shape ? unknownShape[panel < 0 ? 4 : panel > 4 ? 4 : panel] : unknownMorph[panel < 0 ? 4 : panel > 4 ? 4 : panel];
            ++c;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Unknown %s %s %02d", cat, kind, c);
            const std::string n = buf;
            if (used.insert(n).second) return n;
        }
    }
};

// Builds the English base name of a parsed stem, without the trailing state word (the state, when known, is part of the key and is appended
// separately so a pattern's open/closed pair reads as one family). Empty when the word is not in the registry.
std::string BaseLabel(const FaceStemParts& kp) {
    const auto& labels = FaceStemLabels();
    const auto it = labels.find(kp.word);
    if (it == labels.end()) return {};
    std::string n = it->second;
    if (kp.side > 0) n += " Left";
    else if (kp.side < 0) n += " Right";
    if (kp.set >= 2) n += ' ' + std::to_string(kp.set);
    if (kp.size == "small") n += " (Small)";
    else if (kp.size == "smaller") n += " (Smaller)";
    else if (kp.size == "large") n += " (Large)";
    return n;
}

std::string WithState(std::string n, const std::string& state) {
    if (state == "op") n += " Open";
    else if (state == "cl") n += " Closed";
    return n;
}

} // namespace

FaceStemParts ParseFaceStem(const std::string& key) {
    FaceStemParts kp;
    if (key.empty()) return kp;
    std::string s = key;
    // 1) the mesh section tag the game prefixes its blend shape names with (f00_ for the face, mayu00_ for the brows, ...)
    {
        static const char* kPrefixes[] = {"f00_", "mayu00_", "elu00_", "ell00_", "sL00_", "sR00_", "ha00_", "t00_"};
        for (const char* p : kPrefixes) {
            const size_t n = std::strlen(p);
            if (s.size() > n && s.compare(0, n, p) == 0) { kp.prefix = p; s = s.substr(n); break; }
        }
    }
    // 2) the state suffix, _op / _cl, optionally carrying a set number of its own (bisyou_op1)
    {
        const size_t at = s.rfind('_');
        if (at != std::string::npos && at > 0) {
            std::string t = s.substr(at + 1);
            const std::string digits = takeDigits(t);
            std::string word = lower(t);
            if (word == "op" || word == "cl") {
                kp.state = word;
                if (!digits.empty() && kp.set == 0) kp.set = std::atoi(digits.c_str());
                s = s.substr(0, at);
            }
        }
    }
    // 3) the side tag: a trailing capital L or R (mayu00_sianL_cl, gyul02_op keeps its lower-case pair)
    if (!s.empty() && (s.back() == 'L' || s.back() == 'R') && s.size() >= 2) {
        kp.side = s.back() == 'L' ? +1 : -1;
        s.pop_back();
    }
    // 4) a set number (ikari02 -> 2, gyu02 -> 2)
    {
        std::string digits = takeDigits(s);
        if (!digits.empty() && kp.set == 0) kp.set = std::atoi(digits.c_str());
    }
    // 5) the size tag (_s, _ss, _l), on a word boundary
    {
        if (s.size() > 3 && s.compare(s.size() - 3, 3, "_ss") == 0) { kp.size = "smaller"; s = s.substr(0, s.size() - 3); }
        else if (s.size() > 2 && s.compare(s.size() - 2, 2, "_s") == 0) { kp.size = "small"; s = s.substr(0, s.size() - 2); }
        else if (s.size() > 2 && s.compare(s.size() - 2, 2, "_l") == 0) { kp.size = "large"; s = s.substr(0, s.size() - 2); }
    }
    // a trailing separator is a residue of the above
    while (!s.empty() && s.back() == '_') s.pop_back();
    kp.word = lower(s);
    kp.known = FaceStemLabels().find(kp.word) != FaceStemLabels().end();
    return kp;
}

namespace {

// "Eyes 0 closed" -> prefix Eyes, number 0, state closed. False when the name is not an expression morph name.
bool ParseExpressionName(const std::string& name, std::string& prefix, int& number, std::string& state) {
    const size_t s1 = name.find(' ');
    if (s1 == std::string::npos) return false;
    const size_t s2 = name.find(' ', s1 + 1);
    if (s2 == std::string::npos) return false;
    prefix = name.substr(0, s1);
    if (prefix != "Brow" && prefix != "Eyes" && prefix != "Mouth") return false;
    const std::string num = name.substr(s1 + 1, s2 - s1 - 1);
    if (!allDigits(num)) return false;
    state = name.substr(s2 + 1);
    if (state != "open" && state != "closed") return false;
    number = std::atoi(num.c_str());
    return true;
}

int PanelForPrefix(const std::string& prefix) { return prefix == "Brow" ? 1 : prefix == "Eyes" ? 2 : 3; }

} // namespace

size_t ApplySemanticNames(pmx::Model& model, const std::map<std::string, std::string>& expressionShapes,
                          std::vector<std::string>* renames) {
    NameAllocator alloc;
    for (const auto& m : model.morphs) alloc.used.insert(m.name);
    size_t renamed = 0;
    for (auto& m : model.morphs) {
        std::string prefix;
        int number = -1;
        std::string stateWord;
        if (ParseExpressionName(m.name, prefix, number, stateWord)) {
            const int panel = PanelForPrefix(prefix);
            // the shape key of the pattern the morph plays (kept by AddExpressionMorphs); missing = fallback
            std::string key;
            if (const auto it = expressionShapes.find(m.name); it != expressionShapes.end()) key = it->second;
            const FaceStemParts kp = ParseFaceStem(key);
            const std::string base = BaseLabel(kp);
            std::string candidate;
            if (!base.empty()) candidate = WithState(std::string(CategoryWord(panel)) + " " + base, stateWord == "open" ? "op" : "cl");
            const std::string finalName = candidate.empty() ? alloc.unknownName(panel, "Morph") : alloc.claim(candidate);
            if (renames) renames->emplace_back(m.name + " -> " + finalName);
            m.nameEn = m.name;   // the source identifier (Brow 0 closed) survives as the English name
            m.name = finalName;
            ++renamed;
            continue;
        }
        // a raw blend shape morph: "<mesh path>.<shape key>" (cf_O_face.eye_face.f00_def_cl -> key f00_def_cl)
        const size_t dot = m.name.rfind('.');
        if (dot == std::string::npos || dot + 1 >= m.name.size()) continue;
        const std::string key = m.name.substr(dot + 1);
        const FaceStemParts kp = ParseFaceStem(key);
        // a raw shape morph carries a mesh prefix (f00_, mayu00_, ...) or a state marker (_op/_cl) or a known shape word; everything else
        // with a dot is somebody else's name ("Take off outfit0.bra"), left alone
        if (kp.prefix.empty() && kp.state.empty() && !kp.known) continue;
        const int panel = m.panel >= 1 && m.panel <= 4 ? m.panel : 4;
        const std::string base = BaseLabel(kp);
        std::string candidate;
        if (!base.empty()) candidate = WithState(std::string(CategoryWord(panel)) + " Shape " + base, kp.state);
        const std::string finalName = candidate.empty() ? alloc.unknownName(panel, "Shape") : alloc.claim(candidate);
        if (renames) renames->emplace_back(m.name + " -> " + finalName);
        m.nameEn = m.name;
        m.name = finalName;
        ++renamed;
    }
    return renamed;
}

size_t AddFaceAliases(pmx::Model& model) {
    auto find = [&](const std::string& n) { for (size_t i = 0; i < model.morphs.size(); ++i) if (model.morphs[i].name == n) return static_cast<int>(i); return -1; };
    const struct { const char* alias; const char* source; int panel; } kAliases[] = {
        {"Mouth Ah", "あ", 3},
        {"Mouth Ee", "い", 3},
        {"Mouth Oo", "う", 3},
        {"Mouth Mm", "ん", 3},
        {"Mouth Smile", "口角上げ", 3},
        {"Eye Happy", "笑い", 2},
    };
    size_t added = 0;
    for (const auto& a : kAliases) {
        if (find(a.alias) >= 0) continue;
        const int src = find(a.source);
        if (src < 0) continue;
        pmx::Morph m;
        m.name = a.alias;
        m.nameEn = a.alias;
        m.panel = a.panel;
        m.group.emplace_back(src, 1.0f);
        model.morphs.push_back(std::move(m));
        ++added;
    }
    return added;
}

// ---------------------------------------------------------------- extended facial effects ------------------------------------------

namespace {

Bytes encodePng(const assemble::SceneTexture& t) {
    Bytes out;
    auto cb = [](void* ctx, void* data, int size) {
        auto* b = static_cast<Bytes*>(ctx);
        b->insert(b->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    };
    if (!stbi_write_png_to_func(cb, &out, t.width, t.height, 4, t.rgba.data(), t.width * 4))
        throw FormatError("PNG encode failed for texture " + t.name);
    return out;
}

// 512x512 RGBA, one big heart on a transparent ground: the heart the "Heart Eyes" overlay samples. The lobes sit up (+y in texture v).
std::vector<uint8_t> HeartPixels(int size) {
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4, 0);
    const float scale = 1.72f;   // the heart fills about two thirds of the frame: a transparent ring so the overlay doesn't swallow the whole pupil
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float inner = 0.0f, outer = 0.0f;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    const float nx = ((x + (sx + 0.5f) / 3.0f) / size * 2.0f - 1.0f) * scale;
                    const float ny = (1.0f - (y + (sy + 0.5f) / 3.0f) / size * 2.0f) * scale;   // v up
                    const float dInner = std::pow(nx * nx + ny * ny - 1.0f, 3) - nx * nx * ny * ny * ny;
                    // the rim: the same implicit shape, expanded a tenth of its size, in white behind the body
                    const float ox = nx * 0.90f, oy = ny * 0.90f;
                    const float dOuter = std::pow(ox * ox + oy * oy - 1.0f, 3) - ox * ox * oy * oy * oy;
                    if (dInner <= 0.0f) inner += 1.0f / 9.0f;
                    if (dOuter <= 0.0f) outer += 1.0f / 9.0f;
                }
            const float nx = ((x + 0.5f) / size * 2.0f - 1.0f) * scale;
            const float ny = (1.0f - (y + 0.5f) / size * 2.0f) * scale;
            const float t = std::min(1.0f, std::max(0.0f, (ny + 1.05f) / 2.15f));
            float r, g, b;
            if (inner > 0.0f) {   // the body of the heart, over the rim
                r = 0.72f + (0.98f - 0.72f) * t; g = 0.07f + (0.24f - 0.07f) * t; b = 0.17f + (0.34f - 0.17f) * t;
            } else {              // the white rim around it
                r = 0.96f; g = 0.90f; b = 0.92f;
            }
            const float coverage = inner > 0.0f ? inner : outer;
            float a = coverage * 0.97f;
            // the glint on the left lobe
            const float gx = nx + 0.50f, gy = ny - 0.38f;
            const float glint = (gx * gx) / (0.26f * 0.26f) + (gy * gy) / (0.15f * 0.15f);
            if (glint < 1.0f && coverage > 0.35f) {
                const float s = (1.0f - glint) * coverage;
                r += (1.0f - r) * s * 0.85f; g += (1.0f - g) * s * 0.85f; b += (1.0f - b) * s * 0.85f;
            }
            uint8_t* px = &rgba[(static_cast<size_t>(y) * size + x) * 4];
            px[0] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, r * 255.0f)));
            px[1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, g * 255.0f)));
            px[2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, b * 255.0f)));
            px[3] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, a * 255.0f)));
        }
    }
    return rgba;
}

// 128x128 RGBA, one sweat droplet on a transparent ground: round bottom, pointed top, a hard specular glint.
std::vector<uint8_t> DropletPixels(int size) {
    std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4, 0);
    auto inside = [](float nx, float ny) {
        // a true water-drop silhouette: a round belly at the bottom, tangent lines up to a point at the top
        constexpr float c = -0.34f, r = 0.60f, tip = 0.92f;             // belly centre y, belly radius, tip y
        if (ny <= c) { const float dy = ny - c; return nx * nx + dy * dy <= r * r; }
        if (ny >= tip) return false;
        const float d = tip - c;                                        // centre-to-tip distance
        const float tanA = r / std::sqrt(d * d - r * r);                 // half-angle at the tip, as a slope
        const float half = (tip - ny) * tanA;
        return std::fabs(nx) <= half;
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float coverage = 0.0f;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float nx = ((x + (sx + 0.5f) / 4.0f) / size * 2.0f - 1.0f);
                    const float ny = (1.0f - (y + (sy + 0.5f) / 4.0f) / size * 2.0f);
                    if (inside(nx, ny)) coverage += 1.0f / 16.0f;
                }
            const float nx = ((x + 0.5f) / size * 2.0f - 1.0f);
            const float ny = (1.0f - (y + 0.5f) / size * 2.0f);
            const float t = std::min(1.0f, std::max(0.0f, (ny + 0.9f) / 1.85f));
            float r = 0.60f + (0.84f - 0.60f) * t, g = 0.74f + (0.92f - 0.74f) * t, b = 0.88f + (0.99f - 0.88f) * t;
            float a = coverage * 0.92f;
            // a denser rim at the belly's edge (the refraction edge): darker blue
            const float rr = std::sqrt(nx * nx + (ny + 0.34f) * (ny + 0.34f));
            const float rim = std::max(0.0f, std::min(1.0f, (rr - 0.42f) / 0.18f));
            r -= r * rim * 0.32f; g -= g * rim * 0.25f; b -= b * rim * 0.10f;
            // the specular glint, upper left of the belly
            const float gx = nx + 0.20f, gy = ny + 0.06f;
            const float glint = (gx * gx) / (0.15f * 0.15f) + (gy * gy) / (0.26f * 0.26f);
            if (glint < 1.0f) { const float s = (1.0f - glint); r += (1.0f - r) * s * 0.85f; g += (1.0f - g) * s * 0.85f; b += (1.0f - b) * s * 0.85f; }
            uint8_t* px = &rgba[(static_cast<size_t>(y) * size + x) * 4];
            px[0] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, r * 255.0f)));
            px[1] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, g * 255.0f)));
            px[2] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, b * 255.0f)));
            px[3] = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, a * 255.0f)));
        }
    }
    return rgba;
}

// Writes the two generated effect textures; `write` false for the LOD passes (the files already exist from the main export).
// Returns the two model texture indices (or -1 for the one that could not be made).
std::pair<int, int> WriteFxTextures(pmx::Model& model, const std::string& outDir, bool write) {
    if (write) fileio::CreateDirectories(fileio::Join(outDir, "textures"));
    auto add = [&](const char* file, const std::vector<uint8_t>& rgba, int w, int h, const char* name) {
        assemble::SceneTexture t;
        t.name = name; t.width = w; t.height = h; t.rgba = rgba;
        Bytes png = encodePng(t);
        if (write) fileio::WriteFile(fileio::Join(outDir, file), png.data(), png.size());
        model.textures.push_back(file);
        return static_cast<int>(model.textures.size()) - 1;
    };
    const int heart = add("textures/fx_hearteyes.png", HeartPixels(512), 512, 512, "fx_hearteyes");
    const int drop = add("textures/fx_sweat.png", DropletPixels(128), 128, 128, "fx_sweat");
    return {heart, drop};
}

// A unit-less UV sphere (stacks x slices) squashed along local z, placed/oriented by the caller; UV param tracks the texture's droplet.
struct DropletMesh {
    std::vector<Vec3> pos, nrm;
    std::vector<Vec2> uv;
    std::vector<uint32_t> indices;
};
DropletMesh MakeDroplet(int stacks, int slices) {
    DropletMesh d;
    for (int i = 0; i <= stacks; ++i) {
        const float phi = 3.14159265f * static_cast<float>(i) / static_cast<float>(stacks);
        for (int j = 0; j < slices; ++j) {
            const float theta = 2.0f * 3.14159265f * static_cast<float>(j) / static_cast<float>(slices);
            const float sp = std::sin(phi);
            const Vec3 p{sp * std::cos(theta), std::cos(phi), sp * std::sin(theta) * 0.42f};
            d.pos.push_back(p);
            // the ellipsoid normal: x, y, z scaled by 1/s^2 on the squashed axis
            d.nrm.push_back(normalize(Vec3{p.x, p.y, p.z / (0.42f * 0.42f)}));
            d.uv.push_back({static_cast<float>(j) / slices, 1.0f - static_cast<float>(i) / stacks});
        }
    }
    for (int i = 0; i < stacks; ++i)
        for (int j = 0; j < slices; ++j) {
            const int j1 = (j + 1) % slices;
            const uint32_t a = static_cast<uint32_t>(i * slices + j), b = static_cast<uint32_t>(i * slices + j1);
            const uint32_t c = static_cast<uint32_t>((i + 1) * slices + j), e = static_cast<uint32_t>((i + 1) * slices + j1);
            d.indices.push_back(a); d.indices.push_back(c); d.indices.push_back(b);
            d.indices.push_back(b); d.indices.push_back(c); d.indices.push_back(e);
        }
    return d;
}

} // namespace   (close the texture / droplet helpers above)

// -------------------------------------------------------- placement helpers ---------------------------------------------------

namespace {

Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 subV(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float vlen(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 dropletFrameT(const Vec3& n) {
    Vec3 ref{0.0f, 1.0f, 0.0f};
    if (std::fabs(n.y) > 0.9f) ref = {1.0f, 0.0f, 0.0f};
    return normalize(cross3(ref, n));
}

// The left / right iris and white meshes (the game always builds them as cf_Ohitomi_{L,R} and cf_Ohitomi_{L,R}02).
void FindEyeMeshes(const assemble::Scene& scene, int eyeWhite[2], int iris[2]) {
    eyeWhite[0] = eyeWhite[1] = iris[0] = iris[1] = -1;
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const std::string n = lower(scene.meshes[i].name);
        const bool l = n.find("ohitomi_l") != std::string::npos, r = n.find("ohitomi_r") != std::string::npos;
        if (!l && !r) continue;
        const size_t at = l ? n.find("ohitomi_l") : n.find("ohitomi_r");
        const std::string tail = n.substr(at);
        const bool isIris = tail.find("02") != std::string::npos;
        if (isIris) iris[l ? 0 : 1] = static_cast<int>(i);
        else eyeWhite[l ? 0 : 1] = static_cast<int>(i);
    }
}

// The first mesh that looks like the face skin (it carries the blend shapes the expressions play).
int FindFaceMesh(const assemble::Scene& scene) {
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const std::string n = lower(scene.meshes[i].name);
        if (n.find("face") == std::string::npos) continue;
        if (n.find("eyeline") != std::string::npos || n.find("mayu") != std::string::npos || n.find("hitomi") != std::string::npos ||
            n.find("tang") != std::string::npos || n.find("tooth") != std::string::npos || n.find("gag") != std::string::npos) continue;
        return static_cast<int>(i);
    }
    return -1;
}

struct Anchor { Vec3 pos, normal; float radius = 0.0f; };

// The front-most face-mesh vertex inside the given box; falls back to a point on the face plane when there is none.
Anchor FrontAnchor(const assemble::SceneMesh& face, Vec3 wantMin, Vec3 wantMax, Vec3 fallback, Vec3 fallbackNormal, float radius) {
    float bestZ = -1e30f;
    const assemble::SceneVertex* best = nullptr;
    for (const auto& v : face.vertices) {
        if (v.pos.x < wantMin.x || v.pos.x > wantMax.x || v.pos.y < wantMin.y || v.pos.y > wantMax.y || v.pos.z < wantMin.z || v.pos.z > wantMax.z) continue;
        if (v.pos.z > bestZ) { bestZ = v.pos.z; best = &v; }
    }
    Anchor a;
    if (best) {
        a.pos = best->pos;
        a.normal = vlen(best->normal) > 1e-6f ? normalize(best->normal) : normalize(fallbackNormal);
    } else {
        a.pos = fallback;
        a.normal = normalize(fallbackNormal);
    }
    a.radius = radius;
    return a;
}

} // namespace

FaceEffectsResult AddFaceEffects(const assemble::Scene& scene, pmx::Model& model, const std::vector<int>& meshBase,
                                 const std::string& outDir, float scale,
                                 const std::function<void(const std::string&)>& log) {
    FaceEffectsResult res;
    auto say = [&](const std::string& s) { if (log) log(s); };
    auto findBone = [&](const std::string& n) { for (size_t i = 0; i < scene.bones.size(); ++i) if (scene.bones[i].name == n) return static_cast<int>(i); return -1; };
    auto findBoneAny = [&](std::initializer_list<const char*> names) { for (const char* n : names) { const int b = findBone(n); if (b >= 0) return b; } return -1; };

    int eyeL = findBoneAny({"cf_J_hitomi_tx_L", "左目"});
    int eyeR = findBoneAny({"cf_J_hitomi_tx_R", "右目"});
    int head = findBoneAny({"cf_j_head", "頭"});
    Vec3 eyeLPos = eyeL >= 0 ? scene.bones[static_cast<size_t>(eyeL)].pos : Vec3{};
    Vec3 eyeRPos = eyeR >= 0 ? scene.bones[static_cast<size_t>(eyeR)].pos : Vec3{};
    const bool eyesOk = eyeL >= 0 && eyeR >= 0 && vlen(subV(eyeLPos, eyeRPos)) > 1e-6f;
    const Vec3 eyeMid = eyesOk ? (eyeLPos + eyeRPos) * 0.5f : Vec3{};
    const float eyeHalf = eyesOk ? vlen(subV(eyeLPos, eyeRPos)) * 0.5f : 0.0f;

    int eyeWhite[2];
    int iris[2];
    FindEyeMeshes(scene, eyeWhite, iris);
    (void)eyeWhite;
    int faceMesh = FindFaceMesh(scene);

    // The generated textures, once, shared by the overlay materials.
    const auto fxTex = WriteFxTextures(model, outDir, true);
    const int heartTex = fxTex.first, dropTex = fxTex.second;

    // ---------- Eye Roll (a UV morph, the Ahegao technique expressed as PMX UVs) ----------
    {
        std::vector<std::pair<int, Vec4>> eyeUv;
        for (int side = 0; side < 2; ++side) {
            const int mi = iris[side];
            if (mi < 0) continue;
            if (static_cast<size_t>(mi) >= meshBase.size() || meshBase[static_cast<size_t>(mi)] < 0) continue;
            const int base = meshBase[static_cast<size_t>(mi)];
            const size_t nv = scene.meshes[static_cast<size_t>(mi)].vertices.size();
            for (size_t vi = 0; vi < nv; ++vi)
                eyeUv.emplace_back(base + static_cast<int>(vi), Vec4{0.0f, -0.16f, 0.0f, 0.0f});
        }
        if (eyeUv.empty()) {
            say("face fx: no iris meshes found; Eye Roll skipped");
        } else {
            pmx::Morph m;
            m.name = "Eye Roll";
            m.nameEn = "Eye Roll";
            m.panel = 2;
            m.uv = std::move(eyeUv);
            res.eyeRoll = static_cast<int>(model.morphs.size());
            model.morphs.push_back(std::move(m));
        }
    }

    // ---------- Heart Eyes (an overlay of the iris caps sampling a heart texture; a material morph fades it in) ----------
    {
        int heartMatCount = 0;
        int heartVerts = 0;
        for (int side = 0; side < 2; ++side) {
            const int mi = iris[side];
            if (mi < 0) continue;
            if (static_cast<size_t>(mi) >= meshBase.size() || meshBase[static_cast<size_t>(mi)] < 0) continue;
            const auto& src = scene.meshes[static_cast<size_t>(mi)];
            float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
            for (const auto& v : src.vertices) {
                umin = std::min(umin, v.uv.x); umax = std::max(umax, v.uv.x);
                vmin = std::min(vmin, v.uv.y); vmax = std::max(vmax, v.uv.y);
            }
            const float uRange = std::max(1e-6f, umax - umin), vRange = std::max(1e-6f, vmax - vmin);
            const uint32_t base = static_cast<uint32_t>(model.vertices.size());
            for (const auto& v : src.vertices) {
                pmx::Vertex pv;
                const Vec3 n = vlen(v.normal) > 1e-6f ? normalize(v.normal) : Vec3{0, 0, 1};
                pv.position = v.pos * scale + n * (0.0013f * scale);   // a quarter millimetre off the iris, so the depth test never fights it
                pv.normal = n;
                const float un = (v.uv.x - umin) / uRange, vn = (v.uv.y - vmin) / vRange;
                pv.uv = {0.13f + 0.74f * un, 0.13f + 0.74f * vn};
                for (int k = 0; k < 4; ++k) { pv.bone[k] = v.bone[k]; pv.weight[k] = v.weight[k]; }
                pv.edgeScale = 1.0f;
                model.vertices.push_back(pv);
                ++heartVerts;
            }
            for (const auto& sm : src.submeshes)
                for (uint32_t idx : sm.indices) {
                    model.indices.push_back(base + idx);
                    heartMatCount += 1;
                }
        }
        if (heartVerts == 0 || heartMatCount == 0) {
            say("face fx: no iris meshes found; Heart Eyes skipped");
        } else {
            pmx::Material hm;
            hm.name = "face/fx_hearteyes#0";
            hm.nameEn = hm.name;
            hm.memo = "part=head; effect=Heart Eyes overlay";
            hm.diffuse = {1.0f, 1.0f, 1.0f, 0.0f};      // invisible until the morph fades the alpha in
            hm.specular = {0.30f, 0.30f, 0.30f};
            hm.specularPower = 15.0f;
            hm.ambient = {0.5f, 0.5f, 0.5f};
            hm.drawFlags = 0x01;                         // double-sided, no edge
            hm.texture = heartTex;
            hm.indexCount = static_cast<uint32_t>(heartMatCount);
            model.materials.push_back(hm);
            const int heartMat = static_cast<int>(model.materials.size()) - 1;
            pmx::Morph m;
            m.name = "Heart Eyes";
            m.nameEn = "Heart Eyes";
            m.panel = 2;
            pmx::MaterialOffset off;
            off.material = heartMat;
            off.add = true;
            off.diffuse = {0.0f, 0.0f, 0.0f, 1.0f};
            m.materials.push_back(off);
            res.heartEyes = static_cast<int>(model.morphs.size());
            model.morphs.push_back(std::move(m));
            res.overlayVertices += static_cast<size_t>(heartVerts);
        }
    }

    // ---------- Sweat (small drops) and Sweat Heavy (the base drops at the same alpha, plus the heavy set) ----------
    {
        if (head < 0) {
            say("face fx: no head bone found; Sweat skipped");
        } else if (!eyesOk) {
            say("face fx: eye bones missing or coincident; Sweat skipped");
        } else if (faceMesh < 0) {
            say("face fx: no face mesh found to anchor the droplets; Sweat skipped");
        } else {
            const auto& face = scene.meshes[static_cast<size_t>(faceMesh)];
            const float midX = eyeMid.x, midZ = eyeMid.z, eyeY = eyeMid.y;
            // eyeHalf is half the inter-pupillary distance (about 31 mm); the search boxes are sized from it, not from literal metres.
            Anchor forehead = FrontAnchor(face, {midX - 1.4f * eyeHalf, eyeY + 0.4f * eyeHalf, midZ - 0.3f}, {midX + 1.4f * eyeHalf, eyeY + 8.0f * eyeHalf, midZ + 3.0f},
                                          {midX, eyeY + 2.2f * eyeHalf, midZ + 0.72f * eyeHalf}, {0.0f, 0.32f, 0.95f}, 0.34f * eyeHalf);
            Anchor foreheadBig = FrontAnchor(face, {midX - 1.2f * eyeHalf, eyeY + 1.2f * eyeHalf, midZ - 0.3f}, {midX + 1.2f * eyeHalf, eyeY + 8.0f * eyeHalf, midZ + 3.0f},
                                              {midX, eyeY + 2.6f * eyeHalf, midZ + 0.68f * eyeHalf}, {0.0f, 0.42f, 0.91f}, 0.52f * eyeHalf);
            Anchor cheek[2], cheekLow[2];
            for (int s = 0; s < 2; ++s) {
                const float sgn = s == 0 ? 1.0f : -1.0f;
                // below and to the side of the eye, outside the iris's x-extent, inside the face
                const Vec3 in{midX + sgn * 1.70f * eyeHalf, eyeY - 2.6f * eyeHalf, midZ - 0.3f};
                const Vec3 out{midX + sgn * 2.75f * eyeHalf, eyeY + 0.8f * eyeHalf, midZ + 3.0f};
                cheek[s] = FrontAnchor(face, in.x < out.x ? in : out, in.x < out.x ? out : in,
                                       {midX + sgn * 2.15f * eyeHalf, eyeY - 0.90f * eyeHalf, midZ + 0.70f * eyeHalf}, {sgn * 0.55f, -0.05f, 0.84f}, 0.20f * eyeHalf);
                const Vec3 in2{midX + sgn * 1.60f * eyeHalf, eyeY - 4.0f * eyeHalf, midZ - 0.3f};
                const Vec3 out2{midX + sgn * 2.65f * eyeHalf, eyeY - 1.8f * eyeHalf, midZ + 3.0f};
                cheekLow[s] = FrontAnchor(face, in2.x < out2.x ? in2 : out2, in2.x < out2.x ? out2 : in2,
                                          {midX + sgn * 2.0f * eyeHalf, eyeY - 2.6f * eyeHalf, midZ + 0.66f * eyeHalf}, {sgn * 0.55f, -0.15f, 0.82f}, 0.24f * eyeHalf);
            }
            // the two sets: A small (forehead + the two cheeks), B heavy-only (the big forehead drop + two lower cheeks)
            std::vector<Anchor> setA = {forehead, cheek[0], cheek[1]};
            std::vector<Anchor> setB = {foreheadBig, cheekLow[0], cheekLow[1]};
            const DropletMesh tmpl = MakeDroplet(8, 14);
            auto emitSet = [&](const std::vector<Anchor>& drops) -> uint32_t {
                uint32_t indices = 0;
                for (const Anchor& d : drops) {
                    const Vec3 n = d.normal;
                    const Vec3 t = dropletFrameT(n);
                    const Vec3 b = cross3(n, t);
                    const uint32_t base = static_cast<uint32_t>(model.vertices.size());
                    for (size_t i = 0; i < tmpl.pos.size(); ++i) {
                        pmx::Vertex pv;
                        const Vec3 p = d.pos + t * (tmpl.pos[i].x * d.radius) + b * (tmpl.pos[i].y * d.radius) + n * ((tmpl.pos[i].z + 0.32f) * d.radius);
                        pv.position = p * scale;
                        pv.normal = normalize(t * tmpl.nrm[i].x + b * tmpl.nrm[i].y + n * tmpl.nrm[i].z);
                        pv.uv = tmpl.uv[i];
                        pv.bone[0] = head; pv.weight[0] = 1.0f;
                        model.vertices.push_back(pv);
                    }
                    for (uint32_t idx : tmpl.indices) { model.indices.push_back(base + idx); ++indices; }
                    res.overlayVertices += tmpl.pos.size();
                }
                return indices;
            };
            const uint32_t countA = emitSet(setA);
            const uint32_t countB = emitSet(setB);
            auto sweatMat = [&](const char* name, uint32_t indexCount) {
                pmx::Material m;
                m.name = name;
                m.nameEn = name;
                m.memo = "part=head; effect=Sweat overlay";
                m.diffuse = {1.0f, 1.0f, 1.0f, 0.0f};      // invisible until the morph fades the alpha in
                m.specular = {0.75f, 0.75f, 0.75f};
                m.specularPower = 14.0f;
                m.ambient = {0.5f, 0.5f, 0.5f};
                m.drawFlags = 0x01;                         // double-sided, no edge
                m.texture = dropTex;
                m.indexCount = indexCount;
                model.materials.push_back(m);
                return static_cast<int>(model.materials.size()) - 1;
            };
            const int matA = sweatMat("face/fx_sweat#0", countA);
            const int matB = sweatMat("face/fx_sweat_heavy#0", countB);
            pmx::Morph sweat;
            sweat.name = "Sweat";
            sweat.nameEn = "Sweat";
            sweat.panel = 4;
            pmx::MaterialOffset offA;
            offA.material = matA; offA.add = true; offA.diffuse = {0, 0, 0, 1};
            sweat.materials.push_back(offA);
            res.sweat = static_cast<int>(model.morphs.size());
            model.morphs.push_back(std::move(sweat));
            pmx::Morph heavy;
            heavy.name = "Sweat Heavy";
            heavy.nameEn = "Sweat Heavy";
            heavy.panel = 4;
            heavy.materials.push_back(offA);                // the heavy morph is visibly stronger: the base set as well
            pmx::MaterialOffset offB;
            offB.material = matB; offB.add = true; offB.diffuse = {0, 0, 0, 1};
            heavy.materials.push_back(offB);
            res.sweatHeavy = static_cast<int>(model.morphs.size());
            model.morphs.push_back(std::move(heavy));
        }
    }

    return res;
}

std::vector<pmx::DisplayFrame> FacialDisplayFrames(const pmx::Model& model, const std::vector<int>& alreadyListed, size_t budget) {
    // MikuMikuDance keeps the count of LISTED MORPHS in one byte for the whole file and fills its morph table by it: 256 or more corrupt its heap
    // (a crash a few seconds after the model opens). The budget is shared with the Exp frame; morphs Exp already lists are never listed twice.
    const struct { const char* name; int panel; } kFrames[] = {{"Eye", 2}, {"Brow", 1}, {"Mouth", 3}, {"Other", 4}};
    constexpr size_t kPanels = 4;
    std::vector<char> skip(model.morphs.size(), 0);
    for (int mi : alreadyListed) if (mi >= 0 && mi < static_cast<int>(skip.size())) skip[static_cast<size_t>(mi)] = 1;
    std::vector<int> candidates[kPanels];
    size_t total = 0;
    for (size_t f = 0; f < kPanels; ++f) {
        for (size_t i = 0; i < model.morphs.size(); ++i)
            if (model.morphs[i].panel == kFrames[f].panel && !skip[i]) candidates[f].push_back(static_cast<int>(i));
        total += candidates[f].size();
    }
    size_t assigned[kPanels] = {};
    if (total <= budget) {
        for (size_t f = 0; f < kPanels; ++f) assigned[f] = candidates[f].size();
    } else {   // proportional split, largest remainder (ties: Eye, Brow, Mouth, Other), so every panel keeps a fair share of the 255 slots
        size_t base = 0;
        double remainder[kPanels];
        for (size_t f = 0; f < kPanels; ++f) {
            const double exact = total == 0 ? 0.0 : static_cast<double>(budget) * static_cast<double>(candidates[f].size()) / static_cast<double>(total);
            assigned[f] = static_cast<size_t>(exact);
            remainder[f] = exact - static_cast<double>(assigned[f]);
            base += assigned[f];
        }
        size_t left = budget - base;
        size_t order[kPanels] = {0, 1, 2, 3};
        std::stable_sort(order, order + kPanels, [&](size_t a, size_t b) { return remainder[a] > remainder[b]; });
        for (size_t k = 0; k < left; ++k) ++assigned[order[k]];   // left < kPanels: each floor lost less than one slot, so there are fewer leftovers than panels
    }
    std::vector<pmx::DisplayFrame> out;
    for (size_t f = 0; f < kPanels; ++f) {
        if (assigned[f] == 0) continue;
        pmx::DisplayFrame fr;
        fr.name = kFrames[f].name;
        fr.nameEn = kFrames[f].name;
        fr.morphs.assign(candidates[f].begin(), candidates[f].begin() + static_cast<std::ptrdiff_t>(assigned[f]));
        out.push_back(std::move(fr));
    }
    return out;
}

} // namespace cardbridge::exporter
