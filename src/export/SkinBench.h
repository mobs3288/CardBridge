#pragma once

// Repeatable CPU vs GPU skinning benchmark.
//
// Synthetic workloads (documented vertex/bone counts and influence
// distributions) plus optional real-scene timing. GPU execution time comes
// from D3D11 timestamp/disjoint queries around the dispatch (see
// GpuView::LastGpuDispatchMs); CPU submission time alone is never reported
// as GPU time. Equivalence is checked numerically (CPU SkinPoint vs GPU
// readback) with a floating-point tolerance, never by "the dispatch ran".

#include "assemble/Scene.h"
#include "core/Math.h"

#include <string>
#include <vector>

namespace cardbridge::exporter {

struct SkinBenchWorkload {
    std::string name;
    size_t vertices = 0;
    size_t bones = 0;
    bool synthetic = true;
};

struct SkinBenchResult {
    SkinBenchWorkload workload;
    double cpuMs = -1;       // CPU SkinPoint loop (median), steady state without compilation
    double gpuSubmitMs = -1; // GpuView GPU SetSkin wall clock, submission only (median)
    double gpuExecMs = -1;   // GPU execution from timestamp queries (median), -1 when unavailable
    double e2eMs = -1;       // GPU end-to-end incl. flush + readback (preview-representative sync cost)
    double speedup = 1.0;    // cpuMs / gpuSubmitMs (>1 = GPU faster end-to-end for the pose edit)
    double maxError = -1;    // max |CPU - GPU| position error (model units), -1 when not checked
    bool equivalent = false;
    std::string note;
};

// Synthetic workloads used when no real character data is available.
// Vertex counts span small to extremely large (extensive modded geometry).
std::vector<SkinBenchWorkload> DefaultSkinWorkloads();

// Builds a synthetic scene + skin matrices (deterministic PRNG). Each vertex
// has 4 influences over random bones with normalized weights.
void MakeSyntheticSkinWorkload(const SkinBenchWorkload& w, assemble::Scene& scene, std::vector<Mat4>& skin);

// Times the CPU SkinPoint loop over the synthetic vertices (median / p95).
// Runs `repeats` steady-state iterations after warmup; outputs are written
// to `out` (positions) for the equivalence check.
void BenchCpuSkin(const assemble::Scene& scene, const std::vector<Mat4>& skin, int repeats,
                  double& medianMs, double& p95Ms, std::vector<Vec3>& outPositions);

// Runs the full comparison for one workload through GpuView (CPU mode vs GPU
// mode, same input data). `gpu` must already be Init()ed. Returns false when
// the GPU view is unavailable (software fallback); the caller then reports
// the CPU-only result with the GPU column marked unavailable.
bool BenchGpuVsCpu(const SkinBenchWorkload& w, int repeats, class GpuView& gpu, SkinBenchResult& out, std::string* why = nullptr);

// Runs every workload in DefaultSkinWorkloads() and prints the report table.
// Returns the number of workloads with a usable GPU result.
int RunSkinBenchmark(int repeats, std::string* summary = nullptr, const std::string& preference = "auto");

}  // namespace cardbridge::exporter
