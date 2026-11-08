#pragma once

// Adapter classification and selection for the GPU preview / skinning device.
//
// Pure scoring logic, no D3D dependency, so it can be unit-tested with
// synthetic adapter descriptions (no physical GPUs needed). GpuView fills an
// AdapterDesc from DXGI_ADAPTER_DESC1 and calls ScoreAdapter /
// SelectBestAdapter to pick a device.
//
// Design: suitability for CardBridge's workload (a compute-shader skin plus
// the game's DXBC forward pass), not a definitive integrated-vs-discrete
// verdict. DXGI's DedicatedVideoMemory / SharedSystemMemory fields are
// useful signals but do not universally separate integrated from discrete
// (a laptop discrete card and a desktop integrated card can report similar
// numbers, and some integrated parts report large dedicated reservations).
// The score therefore combines:
//   - software / virtual / remote exclusion (DXGI_ADAPTER_FLAG_* + vendor),
//   - dedicated video memory as a capability signal (more is better for the
//     skinning buffers, but integrated with a few hundred MB is still fine),
//   - the dedicated/(dedicated+shared) ratio as a weak discrete hint,
//   - vendor only as an equal tie-breaker (NVIDIA / AMD / Intel are equal;
//     Intel Arc discrete wins on its memory signal, not on a vendor bonus).
// When the signals are ambiguous the class stays Unknown: the score still
// orders the adapters, but no definitive integrated/discrete claim is made.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cardbridge::exporter {

// Matches DXGI_ADAPTER_FLAG_* values without pulling in dxgi.h.
constexpr uint32_t kAdapterFlagRemote = 0x1;
constexpr uint32_t kAdapterFlagSoftware = 0x2;
// Matches DXGI_ADAPTER_FLAG_FORCE_D3D10 (software-like fallback).
constexpr uint32_t kAdapterFlagForceD3D10 = 0x4;

struct AdapterDesc {
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    std::string description;  // Adapter1 Description, UTF-8
    uint64_t dedicatedVideoMemory = 0;    // DXGI_ADAPTER_DESC
    uint64_t dedicatedSystemMemory = 0;
    uint64_t sharedSystemMemory = 0;
    uint32_t flags = 0;  // DXGI_ADAPTER_DESC1 Flags
    int index = 0;        // enumeration order
};

enum class AdapterClass { Unknown, Hardware, DiscreteLikely, IntegratedLikely, Software, Virtual, Remote };

inline const char* AdapterClassName(AdapterClass c) {
    switch (c) {
        case AdapterClass::DiscreteLikely: return "discrete-likely";
        case AdapterClass::IntegratedLikely: return "integrated-likely";
        case AdapterClass::Hardware: return "hardware";
        case AdapterClass::Software: return "software";
        case AdapterClass::Virtual: return "virtual";
        case AdapterClass::Remote: return "remote";
        default: return "unknown";
    }
}

struct AdapterScore {
    int score = 0;
    AdapterClass cls = AdapterClass::Unknown;
    bool suitable = false;  // hardware adapter usable without an explicit fallback
    std::string reason;     // short human-readable justification for logs
};

// Vendor IDs. Only used as an equal tie-breaker / virtual check, never as a
// discrete-vs-integrated verdict.
constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;
constexpr uint32_t kVendorMicrosoft = 0x1414;  // Microsoft Basic Render / Hyper-V video

inline bool IsVendorKnownHardware(uint32_t v) { return v == kVendorNvidia || v == kVendorAmd || v == kVendorIntel; }

// Heuristic from the description when the flag is not set (some basic/software
// adapters do not set DXGI_ADAPTER_FLAG_SOFTWARE). Case-sensitive substrings of
// the UTF-8 description as DXGI reports it.
inline bool DescriptionLooksSoftware(const std::string& desc) {
    return desc.find("Microsoft Basic Render Driver") != std::string::npos ||
           desc.find("Basic Render Driver") != std::string::npos ||
           desc.find("WARP") != std::string::npos ||
           desc.find("Software") != std::string::npos;
}

// Scores one adapter. Software and virtual adapters are unsuitable (negative
// score) unless the caller explicitly asks for a software fallback. All real
// hardware adapters are suitable; the score only orders them.
inline AdapterScore ScoreAdapter(const AdapterDesc& a) {
    const uint64_t dedicated = a.dedicatedVideoMemory;
    const uint64_t shared = a.sharedSystemMemory;
    const uint64_t sysDedicated = a.dedicatedSystemMemory;
    const uint64_t total = dedicated + sysDedicated + shared;
    const bool softwareFlag = (a.flags & kAdapterFlagSoftware) != 0;
    const bool forceD3D10 = (a.flags & kAdapterFlagForceD3D10) != 0;
    const bool remote = (a.flags & kAdapterFlagRemote) != 0;
    const bool looksSoftware = DescriptionLooksSoftware(a.description);
    const bool isVirtualVendor = (a.vendorId == kVendorMicrosoft);

    AdapterScore out;
    // Software / virtual: excluded from normal hardware selection.
    if (softwareFlag || forceD3D10 || looksSoftware || isVirtualVendor) {
        out.score = -1000000;
        out.cls = isVirtualVendor && !softwareFlag ? AdapterClass::Virtual : AdapterClass::Software;
        out.suitable = false;
        if (isVirtualVendor) out.reason = "virtual/basic adapter (Microsoft), excluded";
        else out.reason = "software adapter, excluded from hardware selection";
        return out;
    }
    if (remote) {
        // Remote (e.g. Remote Desktop) adapters are connected to a remote
        // framebuffer: usable only when nothing local exists.
        out.score = -50000;
        out.cls = AdapterClass::Remote;
        out.suitable = false;
        out.reason = "remote adapter, deprioritised below local hardware";
        return out;
    }

    // Local hardware: base so that even modest integrated graphics is
    // suitable for CardBridge (the skinning buffers for a character are
    // tens of MB, well within integrated parts). The rest only orders the
    // adapters; a low score never means "unusable".
    int score = 1000;
    AdapterClass cls = AdapterClass::Unknown;
    std::string reason = "local hardware adapter";

    // Dedicated video memory: capability signal for the skinning buffers.
    // Thresholds are coarse on purpose; exact numbers vary by driver.
    if (dedicated >= 8ULL * 1024 * 1024 * 1024) { score += 4000; reason = "large dedicated video memory (>=8GB)"; }
    else if (dedicated >= 4ULL * 1024 * 1024 * 1024) { score += 3200; reason = "large dedicated video memory (>=4GB)"; }
    else if (dedicated >= 2ULL * 1024 * 1024 * 1024) { score += 2500; reason = "dedicated video memory >=2GB"; }
    else if (dedicated >= 1ULL * 1024 * 1024 * 1024) { score += 1800; reason = "dedicated video memory >=1GB"; }
    else if (dedicated >= 512ULL * 1024 * 1024) { score += 1000; reason = "dedicated video memory >=512MB"; }
    else if (dedicated >= 256ULL * 1024 * 1024) { score += 500; reason = "dedicated video memory >=256MB"; }
    else if (dedicated >= 64ULL * 1024 * 1024) { score += 200; reason = "dedicated video memory >=64MB"; }
    else if (dedicated > 0) { score += 50; reason = "small dedicated video memory"; }

    // Dedicated share of the addressable memory: a weak discrete hint.
    // High share with GBs dedicated is a strong discrete signal; low share
    // with small dedicated is a strong integrated signal. The middle is
    // ambiguous and stays Unknown (we do not invent a verdict).
    if (dedicated > 0 && (dedicated + shared) > 0) {
        const double ratio = static_cast<double>(dedicated) / static_cast<double>(dedicated + shared);
        if (dedicated >= 1ULL * 1024 * 1024 * 1024 && ratio >= 0.75) {
            score += 600;
            cls = AdapterClass::DiscreteLikely;
            reason = "discrete-likely: >=1GB dedicated and >=75% dedicated share";
        } else if (dedicated <= 512ULL * 1024 * 1024 && ratio <= 0.30) {
            // No bonus and no penalty beyond the missing dedicated points:
            // integrated parts stay suitable, they just lose to discrete.
            cls = AdapterClass::IntegratedLikely;
            if (reason == "local hardware adapter") reason = "integrated-likely: small dedicated share of memory";
        }
    }

    // Total addressable memory: a small extra capability signal (capped, so
    // shared-memory-heavy integrated parts do not outscore a discrete card
    // on shared memory alone).
    if (total >= 16ULL * 1024 * 1024 * 1024) score += 400;
    else if (total >= 8ULL * 1024 * 1024 * 1024) score += 250;
    else if (total >= 4ULL * 1024 * 1024 * 1024) score += 100;

    // Known hardware vendor: an equal, tiny tie-breaker only. NVIDIA, AMD
    // and Intel get the same value, so Intel Arc discrete (large dedicated,
    // high share) scores like any other discrete card with the same memory,
    // and AMD integrated scores like Intel integrated with the same memory.
    if (IsVendorKnownHardware(a.vendorId)) score += 10;

    // Tiny total memory cannot hold even a small character's buffers.
    if (total > 0 && total < 256ULL * 1024 * 1024) {
        score -= 5000;
        reason = "less than 256MB total addressable memory";
    }

    if (cls == AdapterClass::Unknown && dedicated >= 1ULL * 1024 * 1024 * 1024) cls = AdapterClass::Hardware;
    if (cls == AdapterClass::Unknown && dedicated > 0) cls = AdapterClass::Hardware;

    out.score = score;
    out.cls = cls;
    out.suitable = score > 0;
    out.reason = reason;
    return out;
}

// Picks the best adapter index (into `adapters`) or -1. Software adapters are
// skipped unless `allowSoftware` is true and no suitable hardware exists
// (the caller's WARP/CPU fallback). Ties break on dedicated memory, then
// enumeration order, so results are deterministic.
inline int SelectBestAdapter(const std::vector<AdapterDesc>& adapters, bool allowSoftware = false) {
    int best = -1;
    int bestScore = 0;
    uint64_t bestDedicated = 0;
    for (int i = 0; i < static_cast<int>(adapters.size()); ++i) {
        const AdapterScore s = ScoreAdapter(adapters[static_cast<size_t>(i)]);
        const bool ok = s.suitable || (allowSoftware && (s.cls == AdapterClass::Software || s.cls == AdapterClass::Virtual));
        if (!ok) continue;
        const uint64_t dedicated = adapters[static_cast<size_t>(i)].dedicatedVideoMemory;
        if (best < 0 || s.score > bestScore || (s.score == bestScore && dedicated > bestDedicated)) {
            best = i; bestScore = s.score; bestDedicated = dedicated;
        }
    }
    return best;
}

// Human-readable one-line diagnostics for the selected adapter.
inline std::string DescribeAdapter(const AdapterDesc& a) {
    const AdapterScore s = ScoreAdapter(a);
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "%s (vendor 0x%04X device 0x%04X, dedicated %.0fMB shared %.0fMB flags 0x%X, score %d, %s: %s%s)",
                  a.description.c_str(), a.vendorId, a.deviceId,
                  static_cast<double>(a.dedicatedVideoMemory) / (1024.0 * 1024.0),
                  static_cast<double>(a.sharedSystemMemory) / (1024.0 * 1024.0),
                  a.flags, s.score, AdapterClassName(s.cls), s.reason.c_str(),
                  s.suitable ? "" : " [fallback only]");
    return std::string(buf);
}

}  // namespace cardbridge::exporter
