// Demo: exercise the nanobench-style additions (environment warnings,
// perf counters, MAD / err% reporting) so the commit ships a visible
// proof-of-life. Measures two trivially-different workloads — a warm
// memory scan vs. a random-access scan — and prints the perf counter
// numbers that explain their cycle-count difference.
//
// Build:  ninja -C build bench_bench_demo
// Run:    taskset -c 0 ./build/bench/bench_bench_demo

#include "bench.h"

using namespace rut;

static constexpr u32 kBufSize = 256 * 1024;  // 256 KB — fits in L2, not L1
alignas(64) static u8 g_buf[kBufSize];
alignas(64) static u32 g_indices[kBufSize / 64];

int main() {
    bench::print_environment_warnings();

    // Prep buffers. Sequential buf, shuffled cacheline-granular indices.
    for (u32 i = 0; i < kBufSize; i++) g_buf[i] = static_cast<u8>(i & 0xff);
    const u32 n_cls = kBufSize / 64;
    for (u32 i = 0; i < n_cls; i++) g_indices[i] = i * 64;
    // Fisher-Yates shuffle.
    u64 rnd = 0x9E3779B97F4A7C15ULL;
    for (u32 i = n_cls - 1; i > 0; i--) {
        rnd = rnd * 6364136223846793005ULL + 1442695040888963407ULL;
        const u32 j = static_cast<u32>(rnd >> 32) % (i + 1);
        u32 t = g_indices[i];
        g_indices[i] = g_indices[j];
        g_indices[j] = t;
    }

    bench::Bench b;
    b.title("bench infra sanity check");
    b.min_iterations(200000);
    b.warmup(5000);
    b.epochs(7);
    b.perf_counters(true);
    b.print_header();

    b.run("sequential_scan", [&] {
        u64 acc = 0;
        for (u32 i = 0; i < kBufSize; i += 64) acc += g_buf[i];
        bench::do_not_optimize(&acc);
    });

    b.run("random_cacheline_scan", [&] {
        u64 acc = 0;
        for (u32 i = 0; i < n_cls; i++) acc += g_buf[g_indices[i]];
        bench::do_not_optimize(&acc);
    });

    b.compare();
    bench::out("\n");
    return 0;
}
