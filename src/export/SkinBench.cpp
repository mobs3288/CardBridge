#include "export/SkinBench.h"

#include "assemble/Pose.h"
#include "export/GpuView.h"
#include "shape/Shape.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace cardbridge::exporter {

namespace {

uint32_t g_rngState = 0;

float RngFloat(float lo, float hi) {
    g_rngState ^= g_rngState << 13;
    g_rngState ^= g_rngState >> 17;
    g_rngState ^= g_rngState << 5;
    const float t = static_cast<float>(g_rngState & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
    return lo + (hi - lo) * t;
}

double Median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const size_t i = static_cast<size_t>(p * static_cast<double>(v.size()));
    return v[std::min(i, v.size() - 1)];
}

assemble::Scene MakeScene(const SkinBenchWorkload& w, std::vector<Mat4>& skin) {
    g_rngState = 0x12345678u ^ static_cast<uint32_t>(w.vertices * 2654435761u) ^ static_cast<uint32_t>(w.bones * 40503u);
    assemble::Scene scene;
    scene.name = "skin-bench:" + w.name;
    for (size_t b = 0; b < w.bones; ++b) {
        assemble::SceneBone bone;
        bone.name = "bench_bone_" + std::to_string(b);
        bone.parent = b ? static_cast<int>(b - 1) : -1;
        bone.pos = {RngFloat(-1.0f, 1.0f), RngFloat(0.0f, 2.0f), RngFloat(-1.0f, 1.0f)};
        scene.bones.push_back(std::move(bone));
    }
    assemble::SceneMesh mesh;
    mesh.name = "bench_mesh";
    mesh.vertices.reserve(w.vertices);
    for (size_t v = 0; v < w.vertices; ++v) {
        assemble::SceneVertex sv;
        sv.pos = {RngFloat(-1.0f, 1.0f), RngFloat(-1.0f, 1.0f), RngFloat(-1.0f, 1.0f)};
        sv.normal = normalize(Vec3{RngFloat(-1.0f, 1.0f), RngFloat(-1.0f, 1.0f), RngFloat(-1.0f, 1.0f)});
        if (sv.normal.x == 0 && sv.normal.y == 0 && sv.normal.z == 0) sv.normal = {0, 1, 0};
        // 4 influences: random bones, Dirichlet-like weights (squared uniform, normalized).
        float raw[4];
        float sum = 0;
        for (int k = 0; k < 4; ++k) {
            sv.bone[k] = static_cast<int>(RngFloat(0.0f, 1.0f) * static_cast<float>(w.bones)) % static_cast<int>(w.bones);
            const float r = RngFloat(0.0f, 1.0f);
            raw[k] = r * r;
            sum += raw[k];
        }
        if (sum <= 0) { sv.bone[0] = 0; sv.weight[0] = 1; sv.weight[1] = sv.weight[2] = sv.weight[3] = 0; }
        else for (int k = 0; k < 4; ++k) sv.weight[k] = raw[k] / sum;
        mesh.vertices.push_back(sv);
    }
    // One triangle strip over the vertices (indices are not timed; they keep
    // the mesh drawable through the normal GpuView::Load path).
    assemble::SceneSubmesh sub;
    sub.material = 0;
    for (size_t v = 0; v + 2 < mesh.vertices.size(); ++v) { sub.indices.push_back(static_cast<uint32_t>(v)); sub.indices.push_back(static_cast<uint32_t>(v + 1)); sub.indices.push_back(static_cast<uint32_t>(v + 2)); }
    mesh.submeshes.push_back(std::move(sub));
    scene.meshes.push_back(std::move(mesh));
    assemble::SceneMaterial mat;
    mat.name = "bench_mat";
    scene.materials.push_back(std::move(mat));
    assemble::ScenePart part;
    part.id = "bench";
    part.meshes = {0};
    scene.parts.push_back(std::move(part));
    skin.clear();
    skin.reserve(w.bones);
    for (size_t b = 0; b < w.bones; ++b) {
        const Vec3 rot{RngFloat(-20.0f, 20.0f), RngFloat(-20.0f, 20.0f), RngFloat(-20.0f, 20.0f)};
        const Vec3 trans{RngFloat(-0.1f, 0.1f), RngFloat(-0.1f, 0.1f), RngFloat(-0.1f, 0.1f)};
        skin.push_back(Mat4::TRS(trans, shape::EulerToQuat(rot), Vec3{1, 1, 1}));
    }
    return scene;
}

}  // namespace

std::vector<SkinBenchWorkload> DefaultSkinWorkloads() {
    return {
        {"small", 10000, 64, true},
        {"medium", 50000, 128, true},
        {"large", 150000, 256, true},
        {"extreme", 500000, 400, true},
    };
}

void MakeSyntheticSkinWorkload(const SkinBenchWorkload& w, assemble::Scene& scene, std::vector<Mat4>& skin) {
    scene = MakeScene(w, skin);
}

void BenchCpuSkin(const assemble::Scene& scene, const std::vector<Mat4>& skin, int repeats,
                  double& medianMs, double& p95Ms, std::vector<Vec3>& outPositions) {
    const auto& verts = scene.meshes[0].vertices;
    outPositions.assign(verts.size(), Vec3{});
    auto run = [&](std::vector<Vec3>& out) {
        for (size_t v = 0; v < verts.size(); ++v) out[v] = assemble::SkinPoint(verts[v], skin);
    };
    // Warm up (shader compilation / cache effects excluded from steady state).
    for (int i = 0; i < 5; ++i) run(outPositions);
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        run(outPositions);
        const auto t1 = std::chrono::high_resolution_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    medianMs = Median(samples);
    p95Ms = Percentile(samples, 0.95);
}

bool BenchGpuVsCpu(const SkinBenchWorkload& w, int repeats, GpuView& gpu, SkinBenchResult& out, std::string* why) {
    out = SkinBenchResult{};
    out.workload = w;
    std::vector<Mat4> skin;
    assemble::Scene scene = MakeScene(w, skin);
    // CPU baseline (isolated loop).
    double p95 = 0;
    {
        std::vector<Vec3> scratch;
        BenchCpuSkin(scene, skin, repeats, out.cpuMs, p95, scratch);
    }
    // GPU path through the real viewer (same input data, same shader).
    GpuOptions opt;
    opt.preference = "auto";
    opt.skin = "gpu";
    // Force a fresh Load so the GPU dual buffers exist.
    gpu.SetSkinMode("gpu");
    std::string loadWhy;
    if (!gpu.Load(scene, &loadWhy)) {
        if (why) *why = loadWhy;
        out.note = "GpuView::Load failed";
        return false;
    }
    // Warm up: compile once, run a few dispatches.
    for (int i = 0; i < 5; ++i) gpu.SetSkin(&skin);
    std::vector<double> submits, execs, e2es;
    submits.reserve(static_cast<size_t>(repeats));
    gpu.SetBenchQueries(true);
    for (int i = 0; i < repeats; ++i) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        gpu.SetSkin(&skin);
        const auto t1 = std::chrono::high_resolution_clock::now();
        submits.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        const double execMs = gpu.LastGpuDispatchMs();
        if (execMs >= 0) execs.push_back(execMs);
        // End-to-end including the flush + readback a syncing consumer pays.
        std::vector<std::array<float, 3>> pos;
        gpu.ReadbackPosed(pos);
        const auto t2 = std::chrono::high_resolution_clock::now();
        e2es.push_back(std::chrono::duration<double, std::milli>(t2 - t0).count());
    }
    gpu.SetBenchQueries(false);
    out.gpuSubmitMs = Median(submits);
    out.gpuExecMs = Median(execs);
    out.e2eMs = Median(e2es);
    if (out.cpuMs > 0 && out.gpuSubmitMs > 0) out.speedup = out.cpuMs / out.gpuSubmitMs;
    // Equivalence: CPU SkinPoint vs GPU readback.
    std::vector<Vec3> cpuPos;
    BenchCpuSkin(scene, skin, 1, p95, p95, cpuPos);
    std::vector<std::array<float, 3>> gpuPos;
    if (gpu.ReadbackPosed(gpuPos) && gpuPos.size() == cpuPos.size()) {
        double mx = 0;
        for (size_t v = 0; v < cpuPos.size(); ++v) {
            const double dx = cpuPos[v].x - gpuPos[v][0], dy = cpuPos[v].y - gpuPos[v][1], dz = cpuPos[v].z - gpuPos[v][2];
            mx = std::max(mx, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        out.maxError = mx;
        out.equivalent = mx < 1e-3;
    }
    return true;
}

int RunSkinBenchmark(int repeats, std::string* summary, const std::string& preference) {
    if (repeats <= 0) repeats = 30;
    GpuView gpu;
    std::string why;
    GpuOptions opt;
    opt.preference = preference;
    opt.skin = "gpu";
    if (!gpu.Init(opt, &why)) {
        std::fprintf(stderr, "skin bench: no GPU device (%s); CPU baselines only\n", why.c_str());
        for (const auto& w : DefaultSkinWorkloads()) {
            std::vector<Mat4> skin;
            assemble::Scene scene = MakeScene(w, skin);
            double med = 0, p95 = 0; std::vector<Vec3> pos;
            BenchCpuSkin(scene, skin, repeats, med, p95, pos);
            std::printf("workload %-8s verts %7zu bones %3zu  CPU %9.3f ms (p95 %9.3f)  GPU n/a (no device: %s)\n",
                        w.name.c_str(), w.vertices, w.bones, med, p95, why.c_str());
        }
        if (summary) *summary = "no GPU device";
        return 0;
    }
    const char* kindName = gpu.kind() == GpuKind::Hardware ? "Hardware" : gpu.kind() == GpuKind::Warp ? "Warp" : "Software";
    std::printf("skin bench: device %s (%s), %d steady-state repeats per workload (synthetic workloads, documented below)\n",
                gpu.adapter().c_str(), kindName, repeats);
    int usable = 0;
    for (const auto& w : DefaultSkinWorkloads()) {
        SkinBenchResult r;
        std::string benchWhy;
        if (!BenchGpuVsCpu(w, repeats, gpu, r, &benchWhy)) {
            std::printf("workload %-8s verts %7zu bones %3zu  GPU unavailable (%s)\n", w.name.c_str(), w.vertices, w.bones, benchWhy.c_str());
            continue;
        }
        ++usable;
        std::printf("workload %-8s verts %7zu bones %3zu  CPU %9.3f ms  GPU submit %8.3f ms  GPU exec %8.3f ms  e2e %8.3f ms  speedup %.2fx  maxerr %.2g (equiv %s)\n",
                    w.name.c_str(), w.vertices, w.bones, r.cpuMs, r.gpuSubmitMs, r.gpuExecMs, r.e2eMs, r.speedup, r.maxError, r.equivalent ? "yes" : "NO");
    }
    if (summary) *summary = gpu.adapter();
    return usable;
}

}  // namespace cardbridge::exporter
