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
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        // Trim CR and whitespace.
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        const char* val = line.c_str() + eq + 1;
        auto num = [&](const char* name, float& dst, bool angle) {
            if (key == name) {
                float v = 0;
                if (std::sscanf(val, "%f", &v) == 1) { dst = angle ? HeelAngleInRange(v) : v; any = true; }
            }
        };
        num("angleAnkle", c.ankleDeg, true);
        num("angleToes", c.toeDeg, true);
        num("angleLeg", c.legDeg, true);
        num("height", c.heightMeters, false);
        num("shoeScaleX", c.shoeScaleX, false);
        num("shoeScaleY", c.shoeScaleY, false);
        num("shoeScaleZ", c.shoeScaleZ, false);
        num("shoeAngle", c.shoeAngleDeg, true);
        num("shoeTranslateY", c.shoeTranslateY, false);
        num("shoeTranslateZ", c.shoeTranslateZ, false);
        num("shoeShearY", c.shoeShearY, true);
        num("shoeShearZ", c.shoeShearZ, true);
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

} // namespace cardbridge::assemble
