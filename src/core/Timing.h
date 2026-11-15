#pragma once

// Stage timing for the conversion pipeline. Off unless CARDBRIDGE_TIMING is set
// to a non-empty, non-"0" value. When it is, each scope below adds its elapsed
// wall time to a process-wide recorder, grouped by name (a name can appear on
// several threads; the recorder is locked). nAt process end (or when
// cbtime::Report() is called explicitly) the recorder prints a sorted
// breakdown to stderr. Set CARDBRIDGE_TIMING_JSON=<file.json> to write the same
// breakdown as JSON instead (names are code constants, so no escaping is
// needed beyond the two delimiters).
//
// This exists so the expensive stages are measured, not guessed:
//   CARDBRIDGE_TIMING=1 cardbridge convert card.png --library <lib> --out <dir>
// prints one line per stage and one total. Disabled, the cost is one cached
// getenv per scope entry.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace cardbridge {
namespace cbtime {

struct Agg {
    double ms = 0.0;
    long count = 0;
};

class Recorder {
public:
    static Recorder& Instance() {
        static Recorder r;
        return r;
    }
    bool Enabled() const {
        static const bool on = [] {
            const char* v = std::getenv("CARDBRIDGE_TIMING");
            return v && *v && std::strcmp(v, "0") != 0;
        }();
        return on;
    }
    void Add(const std::string& name, double ms) {
        std::lock_guard<std::mutex> lk(mu_);
        ++scopes_;
        Agg& e = agg_[name];
        e.ms += ms;
        e.count += 1;
    }
    // Drops every recorded stage (benchmarks want per-iteration numbers).
    void Reset() {
        std::lock_guard<std::mutex> lk(mu_);
        agg_.clear();
        scopes_ = 0;
    }
    // Prints (or writes as JSON) the breakdown. `total_ms` is the wall time the
    // caller measured for the whole command, when it has one (0 = omit it).
    void Report(double total_ms = 0.0) {
        if (!Enabled()) return;
        const char* jsonPath = std::getenv("CARDBRIDGE_TIMING_JSON");
        std::lock_guard<std::mutex> lk(mu_);
        // Sorted by descending time, so the hot stages come first.
        std::map<double, std::string> byTime;
        for (const auto& [name, a] : agg_) byTime.emplace(-a.ms, name);

        if (jsonPath && *jsonPath) {
            FILE* f = std::fopen(jsonPath, "wb");
            if (f) {
                std::fprintf(f, "{\n  \"total_ms\": %.3f,\n  \"stages\": [\n", total_ms);
                bool first = true;
                for (const auto& [neg, name] : byTime) {
                    const Agg& a = agg_[name];
                    if (!first) std::fprintf(f, ",\n");
                    first = false;
                    std::fprintf(f, "    {\"name\": \"%s\", \"ms\": %.3f, \"count\": %ld}", name.c_str(), a.ms, a.count);
                }
                std::fprintf(f, "\n  ]\n}\n");
                std::fclose(f);
                return;
            }
            std::fprintf(stderr, "cbtime: cannot write %s\n", jsonPath);
        }
        std::fprintf(stderr, "\n-- pipeline timing (CARDBRIDGE_TIMING) --\n");
        double known = 0.0;
        for (const auto& [neg, name] : byTime) {
            const Agg& a = agg_[name];
            known += a.ms;
            std::fprintf(stderr, "  %-34s %9.1f ms  x%ld\n", name.c_str(), a.ms, a.count);
        }
        std::fprintf(stderr, "  %-34s %9.1f ms\n", "(sum of stages)", known);
        if (total_ms > 0.0) std::fprintf(stderr, "  %-34s %9.1f ms\n", "(wall clock)", total_ms);
        std::fprintf(stderr, "----------------------------------------\n");
    }
private:
    std::mutex mu_;
    std::map<std::string, Agg> agg_;
    long scopes_ = 0;
};

inline void Report(double total_ms = 0.0) { Recorder::Instance().Report(total_ms); }
inline void Reset() { Recorder::Instance().Reset(); }

struct Scope {
    std::string name;
    std::chrono::steady_clock::time_point t0;
    bool on;
    bool done = false;
    explicit Scope(const char* n) : Scope(std::string(n)) {}
    explicit Scope(std::string n) : name(std::move(n)), on(Recorder::Instance().Enabled()) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    void Stop() {
        if (!on || done) return;
        done = true;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Recorder::Instance().Add(std::move(name), ms);
    }
    ~Scope() { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace cbtime
} // namespace cardbridge

// Unique-ish per-line scope name: paste CB_TIME_SCOPE("...") anywhere in a function.
#define CB_TIME_SCOPE(name) ::cardbridge::cbtime::Scope CB_TIME_SCOPE_##__LINE__(name)
