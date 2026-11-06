#pragma once

// Memory diagnostics for profiling the conversion pipeline. Off unless
// CARDBRIDGE_MEMPROBE is set in the environment, in which case LogMemory
// prints the process working set and private bytes at a named stage to
// stderr (e.g. "memprobe after assemble: workingset=... private=...").
// This distinguishes live allocations from allocator-reserved memory only
// coarsely (private bytes is what the process has committed), which is
// enough to see a stage retaining hundreds of megabytes.
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace cardbridge {

inline void LogMemory(const char* stage) {
    if (!std::getenv("CARDBRIDGE_MEMPROBE")) return;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
        // PagefileUsage is the process's private committed bytes.
        std::fprintf(stderr, "memprobe %-28s workingset=%.1f MB private=%.1f MB paged=%.1f MB\n", stage,
                     static_cast<double>(pmc.WorkingSetSize) / 1048576.0,
                     static_cast<double>(pmc.PagefileUsage) / 1048576.0,
                     static_cast<double>(pmc.PagefileUsage) / 1048576.0);
    }
#else
    std::fprintf(stderr, "memprobe %s: (unsupported on this platform)\n", stage);
#endif
}

} // namespace cardbridge
