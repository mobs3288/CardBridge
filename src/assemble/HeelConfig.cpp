#include "assemble/HeelConfig.h"

#include <cmath>
#include <cstdio>

namespace cardbridge::assemble {

float HeelAngleInRange(float deg) {
    if (deg > 60.001f) return -(360.0f - deg);
    return deg;
}

bool ParseStilettoHeel(const std::string& text, HeelConfig& out) {
    HeelConfig c;
    bool any = false;
    size_t at = 0;
    // UTF-8 BOM (File.ReadAllLines strips it in the original).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        at = 3;
    while (at < text.size()) {
        size_t n = text.find('\n', at);
        size_t r = text.find('\r', at);
        size_t end;
        if (n == std::string::npos && r == std::string::npos) end = text.size();
        else if (n == std::string::npos) end = r;
        else if (r == std::string::npos) end = n;
        else end = std::min(n, r);
        std::string line = text.substr(at, end - at);
        // Skip the line separator (\r\n counts once: a \n after a \r is the same break).
        at = end;
        while (at < text.size() && (text[at] == '\n' || text[at] == '\r')) ++at;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        // File.Trim() on both sides in the original; keys are otherwise exact/case-sensitive.
        size_t ks = 0;
        while (ks < key.size() && (key[ks] == ' ' || key[ks] == '\t' || key[ks] == '\r')) ++ks;
        size_t ke = key.size();
        while (ke > ks && (key[ke - 1] == ' ' || key[ke - 1] == '\t' || key[ke - 1] == '\r')) --ke;
        key = key.substr(ks, ke - ks);
        if (key.empty()) continue;
        const char* val = line.c_str() + eq + 1;
        auto num = [&](const char* name, float& dst, bool angle) {
            if (key == name) {
                float v = 0;
                if (std::sscanf(val, "%f", &v) == 1 && std::isfinite(v)) { dst = angle ? HeelAngleInRange(v) : v; any = true; }
            }
        };
        num("angleAnkle", c.ankleDeg, true);
        num("angleToes", c.toeDeg, true);
        num("angleLeg", c.legDeg, true);
        num("height", c.heightMeters, false);
        num("shoeScaleX", c.shoeScaleX, false);
        num("shoeScaleY", c.shoeScaleY, false);
        num("shoeScaleZ", c.shoeScaleZ, false);
        num("shoeAngle", c.shoeAngleDeg, false);   // Setup() keeps it raw (no GetAngleInRange)
        num("shoeTranslateY", c.shoeTranslateY, false);
        num("shoeTranslateZ", c.shoeTranslateZ, false);
        num("shoeShearY", c.shoeShearY, false);   // degrees for tan(); not range-folded in the original
        num("shoeShearZ", c.shoeShearZ, false);
    }
    if (!any) return false;
    // Setup(): a zero scale means 1.
    if (c.shoeScaleX == 0) c.shoeScaleX = 1;
    if (c.shoeScaleY == 0) c.shoeScaleY = 1;
    if (c.shoeScaleZ == 0) c.shoeScaleZ = 1;
    c.hasConfig = true;
    out = c;
    return true;
}

const std::map<std::string, HeelConfig>& KnownHeelTable() {
    // Transcribed from BepInEx/Stiletto/*.txt (angleAnkle, angleLeg, angleToes, height).
    // Every shipped file uses only the first three keys; angleToes is always absent (-> 0).
    struct Row { const char* name; float ankle, leg, height; };
    static const Row rows[] = {
        {"HM03", 0, 0, 0.0f},
        {"HM25", 15, 0, 0.028f},
        {"HM27", 15, 0, 0.022f},
        {"HM36", 0, 0, 0.0f},
        {"HS05", 0, 0, 0.0f},
        {"HS10", 0, 0, 0.0f},
        {"HS13", 0, 0, 0.0f},
        {"HS16", 8, 0, 0.015f},
        {"HS20", 0, 0, 0.0f},
        {"HS51", 0, 0, 0.0f},
        {"HScamp", 0, 0, 0.0f},
        {"HSloose", 0, 0, 0.0f},
        {"HSrobe", 0, 0, 0.0f},
        {"High Heeled Gal Sandals", 30, 16, 0.058f},
        {"Middle Heel Pumps", 0, 0, 0.018f},
        {"なし", 0, 0, 0.008f},
        {"アイドルパンプス", 0, 0, 0.0f},
        {"キュートナースシューズ", 0, 0, 0.0f},
        {"コルクサンダル", 0, 0, 0.0f},
        {"サイドリボンパンプス ", 0, 0, 0.0f},
        {"サンダル ", 10, 0, 0.008f},
        {"シェルサンダル", 0, 0, 0.0f},
        {"ダンスシューズ", 0, 0, 0.0f},
        {"チアブーツ", 0, 0, 0.0f},
        {"ハイカットスニーカー", 0, 0, 0.0f},
        {"ハロウィンシューズ", 0, 0, 0.0f},
        {"パンプス ", 0, 0, 0.0f},
        {"ビーチサンダル", 0, 0, 0.0f},
        {"フミナ", 0, 0, 0.0f},
        {"フラワーサンダル", 0, 0, 0.0f},
        {"ブロッサムサンダル", 0, 0, 0.0f},
        {"ベルトブーツ", 0, 0, 0.0f},
        {"マシュブーツ", 5, 0, 0.028f},
        {"メカブーツ", 0, 0, 0.0f},
        {"ラウンドパンプス", 0, 0, 0.0f},
        {"ランニングシューズ", 0, 0, 0.0f},
        {"リボンストラップサンダル", 10, 0, 0.008f},
        {"リボンパンプス ", 0, 0, 0.0f},
        {"リングブーツ", 0, 0, 0.0f},
        {"ローファー", 0, 0, 0.0f},
        {"上履き", 0, 0, 0.0f},
        {"将校のブーツ", 0, 0, 0.0f},
        {"小悪魔パンプス", 0, 0, 0.0f},
        {"水上靴_オーロラAL", 20, 0, 0.025f},
        {"草履", 0, 0, 0.0f},
        {"運動靴", 0, 0, 0.0f},
        {"騎士のブーツ", 0, 0, 0.0f},
        {"騎士のブーツ[マスク変更]", 0, 0, 0.0f},
        {"鬼神の脛当て", 0, 0, 0.0f},
        {"魔法のブーツ", 0, 0, 0.0f},
    };
    static std::map<std::string, HeelConfig> table;
    if (table.empty())
        for (const Row& r : rows) {
            HeelConfig c;
            c.ankleDeg = r.ankle; c.legDeg = r.leg; c.toeDeg = 0; c.heightMeters = r.height;
            c.hasConfig = true; c.source = std::string("known ") + r.name;
            table[r.name] = c;
        }
    return table;
}

bool KnownHeelConfig(const std::string& shoeName, HeelConfig& out) {
    const auto& t = KnownHeelTable();
    auto it = t.find(shoeName);
    if (it == t.end()) return false;
    out = it->second;
    return true;
}

bool ShoeWarpIsIdentity(const HeelConfig& c) {
    return c.shoeScaleX == 1 && c.shoeScaleY == 1 && c.shoeScaleZ == 1 && c.shoeAngleDeg == 0 &&
           c.shoeTranslateY == 0 && c.shoeTranslateZ == 0 && c.shoeShearY == 0 && c.shoeShearZ == 0;
}

Mat4 ShoeWarpMatrix(const HeelConfig& c) {
    // Stiletto UpdateShoeWarp, verbatim order: R1 * Shear * T * S * R2 in bind space.
    // Unity Euler(x,0,0) == CardBridge X-rotation (positive points +Z down), via TRS so
    // the sign matches settleHeels' rot() exactly.
    auto rotX = [](float deg) {
        float rad = deg * 3.14159265f / 180.0f;
        float h = rad * 0.5f;
        return Mat4::TRS({0, 0, 0}, Quat{std::sin(h), 0, 0, std::cos(h)}, {1, 1, 1});
    };
    Mat4 r1 = rotX(-(c.ankleDeg + c.legDeg));
    Mat4 shear;
    shear.m[6] = std::tan(c.shoeShearY * 3.14159265f / 180.0f);   // Y += tan(ShearY) * Z (m12)
    shear.m[9] = std::tan(c.shoeShearZ * 3.14159265f / 180.0f);   // Z += tan(ShearZ) * Y (m21)
    Mat4 trans; trans.m[3] = 0; trans.m[7] = c.shoeTranslateY; trans.m[11] = c.shoeTranslateZ;
    Mat4 scale;
    scale.m[0] = c.shoeScaleX; scale.m[5] = c.shoeScaleY; scale.m[10] = c.shoeScaleZ;
    Mat4 r2 = rotX(c.ankleDeg + c.legDeg + c.shoeAngleDeg);
    // Mat4 is row-major p'=M*p, so a*b applies b first. C# val5*identity*val6*val7*val8*val4
    // is the same composition order.
    return r1 * shear * trans * scale * r2;
}

} // namespace cardbridge::assemble
