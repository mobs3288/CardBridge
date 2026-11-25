// Diagnostic only: dump the card's status/outfit clothes slots and accessory slots.
#include "card/CardFile.h"
#include "card/Abmx.h"
#include <cstdio>
#include <string>
using namespace cardbridge;

static void dumpValue(const Value& v, const char* key, int depth) {
    for (int i = 0; i < depth; ++i) std::fputc(' ', stderr);
    std::fprintf(stderr, "%s: ", key);
    if (v.isInt()) std::fprintf(stderr, "int %lld\n", (long long)v.asInt());
    else if (v.isFloat()) std::fprintf(stderr, "float %g\n", v.asFloat());
    else if (v.isStr()) std::fprintf(stderr, "str '%s'\n", v.asStr().c_str());
    else if (v.isBool()) std::fprintf(stderr, "bool %d\n", v.asBool());
    else if (v.isArray()) {
        std::fprintf(stderr, "array[%zu]\n", v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            char kb[32]; std::snprintf(kb, sizeof kb, "[%zu]", i);
            dumpValue(v.at(i), kb, depth + 1);
        }
    } else if (v.isMap()) {
        std::fprintf(stderr, "map[%zu]\n", v.size());
        for (const auto& kv : v.asMap()) {
            std::string k = kv.first.isStr() ? kv.first.asStr() : ("<" + std::to_string(kv.first.asInt()) + ">");
            dumpValue(kv.second, k.c_str(), depth + 1);
        }
    } else if (v.isBin()) std::fprintf(stderr, "bin[%zu]\n", v.asBin().size());
    else std::fprintf(stderr, "nil\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: probe <card>\n"); return 1; }
    card::Card c;
    try { c = card::LoadCard(argv[1]); } catch (const std::exception& e) { std::fprintf(stderr, "card: %s\n", e.what()); return 2; }
    std::fprintf(stderr, "family=%s outfits=%zu\n", card::FamilyName(c.family), c.outfits.size());
    std::fprintf(stderr, "--- status ---\n");
    {
        const Bytes& cs = c.status.at("clothesState").asBin();
        std::fprintf(stderr, "clothesState bytes:");
        for (uint8_t b : cs) std::fprintf(stderr, " %u", (unsigned)b);
        std::fprintf(stderr, "\n");
        const Bytes& so = c.status.at("siruLv").asBin();
        std::fprintf(stderr, "siruLv bytes:");
        for (uint8_t b : so) std::fprintf(stderr, " %u", (unsigned)b);
        std::fprintf(stderr, "\n");
    }
    dumpValue(c.status, "status", 0);
    {
        auto ab = card::LoadAbmx(c.extended);
        std::fprintf(stderr, "--- abmx bones=%zu ---\n", ab.bones.size());
        for (const auto& [name, mods] : ab.bones) {
            bool relevant = false;
            for (size_t i = 0; i < mods.size(); ++i) {
                const auto& m = mods[i];
                if (!m.empty() && (mods.size() == 1 || i == 0)) relevant = true;
            }
            if (!relevant) continue;
            std::fprintf(stderr, "abmx %-28s mods=%zu", name.c_str(), mods.size());
            for (size_t i = 0; i < mods.size() && i < 8; ++i) {
                const auto& m = mods[i];
                std::fprintf(stderr, " [%zu s=(%.3f,%.3f,%.3f) len=%.3f p=(%.3f,%.3f,%.3f)]", i, m.scale.x, m.scale.y, m.scale.z, m.length, m.position.x, m.position.y, m.position.z);
            }
            std::fprintf(stderr, "\n");
        }
    }
    std::fprintf(stderr, "--- rows ---\n");
    for (const auto& r : c.rows) {
        if (r.property.find("Clothes") != std::string::npos || r.property.find("clothes") != std::string::npos)
            std::fprintf(stderr, "row prop='%s' mod='%s' slot=%lld local=%lld cat=%d\n", r.property.c_str(), r.modId.c_str(), (long long)r.slot, (long long)r.localSlot, r.category);
    }
    for (size_t oi = 0; oi < c.outfits.size(); ++oi) {
        const auto& o = c.outfits[oi];
        std::fprintf(stderr, "--- outfit %zu ---\n", oi);
        const Value& parts = o.clothes.at("parts");
        std::fprintf(stderr, "clothes keys:\n");
        for (const auto& kv : o.clothes.asMap()) {
            if (kv.first.isStr()) std::fprintf(stderr, "  %s (%s)\n", kv.first.asStr().c_str(), kv.second.isArray() ? "array" : "other");
        }
        for (size_t i = 0; i < parts.size(); ++i) {
            const Value& p = parts.at(i);
            std::fprintf(stderr, "  part[%zu] id=%lld type=%lld", i, (long long)p.at("id").asInt(), (long long)p.at("type").asInt());
            const Value& ci = p.at("colorInfo");
            if (ci.isArray() && ci.size() > 0) {
                std::fprintf(stderr, " ci0.pat=%lld", (long long)ci.at(0).at("pattern").asInt());
            }
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr, "subPartsId:");
        for (const auto& e : o.clothes.at("subPartsId").asArray()) std::fprintf(stderr, " %lld", (long long)e.asInt());
        std::fprintf(stderr, "\n");
        const Value& acc = o.accessory.at("parts");
        std::fprintf(stderr, "  accessories: %zu\n", acc.size());
        for (size_t i = 0; i < acc.size(); ++i) {
            const Value& p = acc.at(i);
            if (p.at("id").asInt() == 0 && p.at("type").asInt(120) == 120) continue;
            std::fprintf(stderr, "  acc[%zu] type=%lld id=%lld parent='%s'\n", i, (long long)p.at("type").asInt(), (long long)p.at("id").asInt(), p.at("parentKey").asStr().c_str());
        }
    }
    return 0;
}
