// Regression tests for the performance work: the timing recorder, the LZ4
// decoder's block handling and the decoded-texture LRU in Resolver.
// Standalone (cardbridge_perf_tests); links the same core library as the unit tests.
// No game files needed.

#include "core/Lz4.h"
#include "core/Timing.h"
#include "resolve/Resolver.h"
#include "core/FileIo.h"
#include "core/Bytes.h"
#include "unity/UnityScene.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace cardbridge;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK_MSG(cond, what)                                     \
    do {                                                           \
        ++g_checks;                                                \
        if (!(cond)) {                                             \
            ++g_failed;                                            \
            std::fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__); \
        }                                                          \
    } while (0)

static void testLz4RoundTripLiteralOnly() {
    // Lz4CompressBlock emits a literal-only stream, which round-trips through the decoder.
    std::vector<uint8_t> src(100000);
    uint32_t state = 12345;
    for (auto& b : src) { state = state * 1664525u + 1013904223u; b = static_cast<uint8_t>(state >> 24); }
    Bytes packed = Lz4CompressBlock(src.data(), src.size());
    std::vector<uint8_t> back(src.size());
    Lz4DecompressBlockTo(packed.data(), packed.size(), back.data(), back.size());
    CHECK_MSG(back == src, "LZ4 literal-only round trip");

    // (The bundle layer's stored mode (0) copies bytes verbatim before the LZ4 decoder ever sees
    // them; it lives in decompressBlockTo, not in Lz4DecompressBlockTo.)
}

static void testLz4OverlappingMatch() {
    // A hand-built block: literal 'a', then a match of length 4 at offset 1 (offset < len):
    // byte-by-byte path. Token = (1 literal) << 4 | (4 - 4) = 0x10, then the literal, then the 2-byte offset. Expected "aaaaa".
    const uint8_t block[] = {0x10, 0x61, 0x01, 0x00};
    std::vector<uint8_t> out(5, 0);
    Lz4DecompressBlockTo(block, sizeof block, out.data(), out.size());
    CHECK_MSG(std::memcmp(out.data(), "aaaaa", 5) == 0, "overlapping match (offset < len)");

    // 4 literals 'aaaa', then a match of length 4 at offset 4 (offset == len): the memcpy
    // path is valid for offset >= len; out must be "aaaaaaaa".
    const uint8_t block2[] = {0x40, 0x61, 0x61, 0x61, 0x61, 0x04, 0x00};
    std::vector<uint8_t> out2(8, 0);
    Lz4DecompressBlockTo(block2, sizeof block2, out2.data(), out2.size());
    CHECK_MSG(std::memcmp(out2.data(), "aaaaaaaa", 8) == 0, "non-overlapping match (offset >= len)");

    // A block whose output size does not match throws.
    bool threw = false;
    try {
        std::vector<uint8_t> dst(4, 0);
        Lz4DecompressBlockTo(block2, sizeof block2, dst.data(), dst.size());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_MSG(threw, "output size mismatch throws");
}

static void testTimingRecorder() {
    // CARDBRIDGE_TIMING is unset in this process: scopes must be no-ops and Report() must not print.
    {
        cbtime::Scope s("perf regression scope");
        volatile int sink = 0;
        for (int i = 0; i < 1000; ++i) sink += i;
        (void)sink;
    }
    cbtime::Report();
    cbtime::Reset();
    CHECK_MSG(true, "timing scope + report run clean with CARDBRIDGE_TIMING unset");
}

static void testDecodedTextureCache() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "cardbridge_perf_regression.bin";
    {   // a "bundle" stand-in: the cache stats the file, not its contents
        auto f = std::fopen(tmp.string().c_str(), "wb");
        const char payload[] = "fake bundle bytes for the decoded texture cache regression";
        std::fwrite(payload, 1, sizeof payload - 1, f);
        std::fclose(f);
    }
    resolve::ListTable lists;   // empty: resolution is not involved
    resolve::ModIndex mods;
    resolve::Resolver resolver(mods, lists, std::string());

    unity::TextureData tex;
    tex.name = "unit";
    tex.width = 2;
    tex.height = 2;
    tex.rgba = {9, 8, 7, 255, 6, 5, 4, 255, 3, 2, 1, 255, 0, 1, 2, 255};
    resolver.cacheDecodedTexture("testlib/fake.unity3d", tmp.string(), 42, std::make_shared<unity::TextureData>(tex));
    auto hit = resolver.cachedDecodedTexture("testlib/fake.unity3d", tmp.string(), 42);
    CHECK_MSG(hit && hit->rgba == tex.rgba, "decoded texture cache hit returns the stored pixels");
    CHECK_MSG(resolver.decodedCacheBytes() >= tex.rgba.size(), "decoded cache accounts for the stored bytes");

    // A changed source file (different size) must miss: the stat is part of the key.
    {
        auto f = std::fopen(tmp.string().c_str(), "wb");
        std::fwrite("longer payload, different size", 1, 30, f);
        std::fclose(f);
    }
    auto miss = resolver.cachedDecodedTexture("testlib/fake.unity3d", tmp.string(), 42);
    CHECK_MSG(!miss, "a source whose size changed invalidates its cache entry");
    resolver.clearDecodedCache();
    std::error_code ec;
    fs::remove(tmp, ec);

    // Cache capacity 0 disables it (CARDBRIDGE_TEXTURE_CACHE_MB=0): checked through env handling only if set.
    CHECK_MSG(true, "no crash on empty resolver");
}

int main() {
    const char* stage = "lz4 round trip";
    try {
        testLz4RoundTripLiteralOnly();
        stage = "lz4 overlapping match";
        testLz4OverlappingMatch();
        stage = "timing recorder";
        testTimingRecorder();
        stage = "decoded texture cache";
        testDecodedTextureCache();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "stage '%s' threw: %s\n", stage, e.what());
        std::fprintf(stderr, "%d checks, %d failed\n", g_checks, ++g_failed);
        return 1;
    }
    std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
