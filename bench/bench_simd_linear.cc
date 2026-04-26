// bench_simd_linear — head-to-head latency comparison of all
// LinearScan variants (scalar / SSE2 / AVX2 / AVX-512 / NEON) across
// route counts 8..128.
//
// Goal: find empirical cutoffs for the picker's SIMD-LinearScan
// branch. The hardcoded defaults in route_select.cc (kSimdLsXxx
// Cutoff) were conservative guesses; this bench replaces them with
// data points. Output is a sweep table that picker maintainers
// re-run periodically and check whether the cutoffs need adjusting.
//
// Why parity testing AND benchmarking: parity (test_route_simd_
// linear.cc) proves correctness — every variant returns the same
// route_idx for the same input. This bench layers the cost
// comparison on top: SAME inputs, so any latency difference is
// purely from the dispatch impl.
//
// Reads: SSE2/AVX2/AVX-512 only run on x86_64 hosts that support
// them (CpuCaps gate). NEON only on aarch64 hosts. Each variant
// the host doesn't support prints "(unsupported)" so the table
// stays interpretable across architectures.

#include <chrono>
#include <cstdio>
#include <cstring>

#include "rut/runtime/cpu_caps.h"
#include "rut/runtime/route_table.h"

using namespace rut;

namespace {

// Realistic SaaS-like routes — short / medium / long mixed so every
// SIMD chunk-size branch (≤16, ≤32, ≤64, >64) gets exercised.
const char* kRoutes[] = {
    "/",
    "/v1",
    "/v2",
    "/health",
    "/metrics",
    "/api/users",
    "/api/orders",
    "/api/products",
    "/admin/dashboard",
    "/admin/users",
    "/admin/audit",
    "/oauth/token",
    "/oauth/authorize",
    "/webhooks/stripe",
    "/webhooks/github",
    "/api/v1/users/me",
    "/api/v1/users/me/profile",
    "/api/v1/users/me/profile/preferences",
    "/api/v1/orders/recent",
    "/api/v1/orders/pending",
    "/api/v1/products/catalog",
    "/api/v1/products/search",
    "/api/v1/sessions/refresh",
    "/api/v1/sessions/new",
    "/api/v1/admin/dashboard/widgets",
    "/api/v1/admin/dashboard/widgets/billing",
    "/api/v1/admin/dashboard/widgets/billing/customers",
    "/api/v1/admin/dashboard/widgets/billing/customers/12345",
    "/api/v1/admin/dashboard/widgets/billing/customers/67890",
    "/api/v1/admin/dashboard/widgets/billing/invoices/recent",
    "/api/v1/admin/dashboard/widgets/billing/invoices/pending",
    "/api/v1/admin/dashboard/widgets/billing/invoices/2024-Q4",
};
constexpr u32 kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

constexpr u32 kIters = 5'000'000;
constexpr u32 kRingSize = 1024;

double bench(const RouteConfig& cfg, const Str* probes, u32 nprobes) {
    volatile u32 sink = 0;
    for (u32 i = 0; i < 50000; i++) {
        const auto& p = probes[i & (nprobes - 1)];
        const RouteEntry* r = cfg.match(reinterpret_cast<const u8*>(p.ptr), p.len, 0);
        sink ^= reinterpret_cast<uintptr_t>(r);
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (u32 i = 0; i < kIters; i++) {
        const auto& p = probes[i & (kRingSize - 1)];
        const RouteEntry* r = cfg.match(reinterpret_cast<const u8*>(p.ptr), p.len, 0);
        sink ^= reinterpret_cast<uintptr_t>(r);
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(end - start).count() / kIters;
}

double bench_dispatch(const RouteDispatch* d, u32 n) {
    if (n > kRouteCount) n = kRouteCount;
    RouteConfig cfg;
    if (!cfg.set_dispatch(d)) return -1.0;
    for (u32 i = 0; i < n; i++) {
        if (!cfg.add_static(kRoutes[i], 0, 200)) return -1.0;
    }
    Str probes[kRingSize];
    for (u32 i = 0; i < kRingSize; i++) {
        const char* p = kRoutes[i % n];
        probes[i] = Str{p, static_cast<u32>(strlen(p))};
    }
    return bench(cfg, probes, kRingSize);
}

void print_row(u32 n,
               double scalar,
               double sse2,
               double avx2,
               double avx512,
               double neon,
               const CpuCaps& caps) {
    auto fmt = [](double v, bool supported) -> const char* {
        static char buf[64][32];
        static u32 idx = 0;
        idx = (idx + 1) % 64;
        if (!supported) return "    n/a";
        if (v < 0) return "   skip";
        std::snprintf(buf[idx], 32, "%6.2fns", v);
        return buf[idx];
    };
    printf("%4u | %s | %s | %s | %s | %s\n",
           n,
           fmt(scalar, true),
           fmt(sse2, caps.has_sse2),
           fmt(avx2, caps.has_avx2),
           fmt(avx512, caps.has_avx512f && caps.has_avx512bw),
           fmt(neon, caps.has_neon));
}

}  // namespace

int main() {
    const CpuCaps caps = CpuCaps::detect();
    printf("=== LinearScan variants — latency sweep (ns/match, lower is better) ===\n");
    printf("CPU caps: sse2=%d sse42=%d avx2=%d avx512f=%d avx512bw=%d neon=%d sve=%d\n\n",
           caps.has_sse2,
           caps.has_sse42,
           caps.has_avx2,
           caps.has_avx512f,
           caps.has_avx512bw,
           caps.has_neon,
           caps.has_sve);
    printf("   N |  scalar  |   sse2   |   avx2   |  avx512  |   neon\n");
    printf("-----+----------+----------+----------+----------+----------\n");
    for (u32 n : {8u, 16u, 24u, 32u, 48u, 64u, 96u, kRouteCount}) {
        const double scalar = bench_dispatch(&kLinearScanDispatch, n);
        const double sse2 =
            caps.has_sse2 ? bench_dispatch(&kSimdLsSse2Dispatch, n) : -1.0;
        const double avx2 =
            caps.has_avx2 ? bench_dispatch(&kSimdLsAvx2Dispatch, n) : -1.0;
        const double avx512 = (caps.has_avx512f && caps.has_avx512bw)
                                  ? bench_dispatch(&kSimdLsAvx512Dispatch, n)
                                  : -1.0;
        const double neon =
            caps.has_neon ? bench_dispatch(&kSimdLsNeonDispatch, n) : -1.0;
        print_row(n, scalar, sse2, avx2, avx512, neon, caps);
    }
    printf("\nGuidance for picker cutoffs in route_select.cc:\n");
    printf("  Find the largest N where each SIMD variant is still ≤ scalar\n");
    printf("  + ~5%% (noise band). That N is the variant's cutoff.\n");
    return 0;
}
