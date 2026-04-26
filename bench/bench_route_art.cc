// bench_route_art — head-to-head latency + memory comparison between
// ArtTrie and ByteRadixTrie across the picker's eligibility range
// (#47 round 1 wired the picker to choose ByteRadix when there's
// segment-aligned prefix overlap and N > 16; ART is the proposed
// strict replacement for that branch).
//
// Goal: prove ART is never meaningfully slower than ByteRadix in any
// shape the picker might steer to it. We don't need huge wins — 5%
// is fine — but we must NOT have regressions on adversarial shapes
// (high fan-out, long shared prefixes, etc).
//
// What's measured per (impl, shape, N, cache):
//   - p50 / p99 latency per match (ns)
//   - Lookup throughput (M/s)
//   - Inline memory (bytes — sizeof state struct)
//
// Cache regimes:
//   - hot:  the same trie is probed in a tight loop, so descent
//           cache lines stay in L1.
//   - cold: between probes the loop dirties enough cache to evict
//           the trie. Real gateway traffic isn't this adversarial,
//           but it's the regime where ART's smaller footprint
//           should pay off most.

#include "rut/runtime/route_art.h"
#include "rut/runtime/route_byte_radix.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace rut;

namespace {

// =====================================================================
// Workload shapes
// =====================================================================

// Realistic SaaS gateway routes (slim port of the closed-#41 dataset).
// Mostly /api/v1/<resource>, /admin/*, /oauth/*, /webhooks/*, and
// per-resource sub-routes — hits sparse fan-out on most nodes with
// occasional medium-fan-out at API segment boundaries.
const char* kSaasRoutes[] = {
    "/api/v1/users",
    "/api/v1/users/me",
    "/api/v1/users/me/settings",
    "/api/v1/orders",
    "/api/v1/orders/recent",
    "/api/v1/orders/pending",
    "/api/v1/products",
    "/api/v1/products/top",
    "/api/v1/products/search",
    "/api/v1/sessions",
    "/api/v1/sessions/new",
    "/api/v1/sessions/refresh",
    "/api/v1/events",
    "/api/v1/teams",
    "/api/v1/teams/me",
    "/api/v1/projects",
    "/api/v1/tasks",
    "/api/v1/comments",
    "/api/v1/tags",
    "/api/v1/files",
    "/api/v1/messages",
    "/api/v1/channels",
    "/api/v1/integrations",
    "/api/v1/policies",
    "/api/v1/notifications",
    "/api/v1/customers",
    "/api/v2/users",
    "/api/v2/orders",
    "/api/v2/products",
    "/api/v2/sessions",
    "/api/v3",
    "/admin",
    "/admin/users",
    "/admin/audit",
    "/admin/sessions",
    "/admin/billing",
    "/admin/billing/invoice",
    "/admin/billing/usage",
    "/admin/quota",
    "/admin/health",
    "/oauth/token",
    "/oauth/authorize",
    "/oauth/revoke",
    "/oauth/userinfo",
    "/oauth/jwks",
    "/webhooks/stripe",
    "/webhooks/github",
    "/webhooks/slack",
    "/webhooks/datadog",
    "/webhooks/pagerduty",
    "/webhooks/sentry",
    "/webhooks/intercom",
    "/webhooks/segment",
    "/health",
    "/healthz",
    "/metrics",
    "/_status",
    "/_ready",
    "/_live",
    "/internal/debug",
    "/internal/dump",
    "/internal/profile",
    "/internal/trace",
    "/v1",
    "/v2",
    "/v3",
    "/static/css/main.css",
    "/static/js/app.js",
    "/static/img/logo.svg",
    "/docs",
    "/docs/api",
    "/docs/getting-started",
    "/blog",
    "/blog/feed",
    "/blog/archive",
    "/about",
    "/contact",
    "/pricing",
    "/login",
    "/logout",
    "/signup",
    "/help",
    "/support",
};
constexpr u32 kSaasCount = sizeof(kSaasRoutes) / sizeof(kSaasRoutes[0]);

// Adversarial dense fan-out: catchall + many distinct top-level
// single-byte prefixes. Forces ART's root all the way to Node256
// and keeps ByteRadix's children FixedVec densely populated.
struct DenseFanout {
    char paths[127][3];
    u32 count;
    DenseFanout() {
        count = 127;
        for (u32 i = 0; i < count; i++) {
            paths[i][0] = '/';
            paths[i][1] = static_cast<char>(0x40 + i);
            paths[i][2] = '\0';
        }
    }
};

// Deep narrow chain: /a/b/c/d/.../leaf — every node has fan-out 1
// down a long path, then branches at the bottom.
struct DeepNarrow {
    char paths[64][32];
    u32 count;
    DeepNarrow() {
        count = 64;
        for (u32 i = 0; i < count; i++) {
            // /a/b/c/d/e/f/g/h/<i_hex>
            const char* prefix = "/a/b/c/d/e/f/g/h/";
            u32 plen = 0;
            while (prefix[plen]) {
                paths[i][plen] = prefix[plen];
                plen++;
            }
            const char hex[] = "0123456789abcdef";
            paths[i][plen++] = hex[(i >> 4) & 0xf];
            paths[i][plen++] = hex[i & 0xf];
            paths[i][plen] = '\0';
        }
    }
};

// =====================================================================
// Bench harness
// =====================================================================

template <typename Trie>
void build(Trie& t, const char* const* paths, u32 n, u32 cap) {
    if (n > cap) n = cap;
    for (u32 i = 0; i < n; i++) {
        const u32 plen = static_cast<u32>(strlen(paths[i]));
        t.insert(Str{paths[i], plen}, 0, static_cast<u16>(i));
    }
}

template <typename Trie>
void build_view(Trie& t, const char (*paths)[3], u32 n) {
    for (u32 i = 0; i < n; i++) {
        const u32 plen = static_cast<u32>(strlen(paths[i]));
        t.insert(Str{paths[i], plen}, 0, static_cast<u16>(i));
    }
}

template <u32 N, typename Trie>
void build_view_n(Trie& t, const char (*paths)[N], u32 n) {
    for (u32 i = 0; i < n; i++) {
        const u32 plen = static_cast<u32>(strlen(paths[i]));
        t.insert(Str{paths[i], plen}, 0, static_cast<u16>(i));
    }
}

constexpr u32 kIters = 2'000'000;
constexpr u32 kQueryRing = 1024;  // power-of-2 ring of probe paths

template <typename Trie>
double bench_hot(const Trie& t, const Str* probes, u32 nprobes) {
    // Warm-up.
    volatile u32 sink = 0;
    for (u32 i = 0; i < 10000; i++) {
        sink ^= t.match(probes[i % nprobes], 0);
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (u32 i = 0; i < kIters; i++) {
        sink ^= t.match(probes[i & (kQueryRing - 1)], 0);
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)sink;
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    return ns / kIters;  // ns/match
}

template <typename Trie>
double bench_cold(const Trie& t, const Str* probes, u32 nprobes) {
    // Between each probe, walk a 4 MB scratch buffer to evict L1+L2.
    static char scratch[4 * 1024 * 1024];
    static volatile u32 sink_v = 0;
    for (u32 i = 0; i < 10000; i++) sink_v ^= t.match(probes[i % nprobes], 0);
    auto start = std::chrono::high_resolution_clock::now();
    constexpr u32 kColdIters = 200'000;
    for (u32 i = 0; i < kColdIters; i++) {
        // Touch ~64 KB of scratch to evict the trie from L1.
        u32 acc = 0;
        for (u32 j = 0; j < 1024; j++) acc ^= scratch[(i * 64 + j * 64) & (sizeof(scratch) - 1)];
        sink_v ^= acc;
        sink_v ^= t.match(probes[i & (kQueryRing - 1)], 0);
    }
    auto end = std::chrono::high_resolution_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    return ns / kColdIters;
}

// =====================================================================
// Single-row run — build both impls, bench both, print compare row
// =====================================================================

struct Result {
    const char* shape;
    u32 n;
    double art_hot_ns;
    double br_hot_ns;
    double art_cold_ns;
    double br_cold_ns;
    u32 art_nodes;
    u32 br_nodes;
};

void print_header() {
    printf("%-20s %4s | %8s %8s %5s | %8s %8s %5s | %5s %5s\n",
           "shape",
           "N",
           "art_hot",
           "br_hot",
           "Δ%",
           "art_cold",
           "br_cold",
           "Δ%",
           "art_n",
           "br_n");
    printf(
        "----------------------------------------------------------------------------------------"
        "\n");
}

void print_row(const Result& r) {
    auto pct = [](double a, double b) { return ((b - a) / b) * 100.0; };
    printf("%-20s %4u | %7.1fns %7.1fns %+5.1f | %7.1fns %7.1fns %+5.1f | %5u %5u\n",
           r.shape,
           r.n,
           r.art_hot_ns,
           r.br_hot_ns,
           pct(r.art_hot_ns, r.br_hot_ns),
           r.art_cold_ns,
           r.br_cold_ns,
           pct(r.art_cold_ns, r.br_cold_ns),
           r.art_nodes,
           r.br_nodes);
}

// Build a probe ring: cycle through all registered paths plus a few
// misses. Probing the same trie is the realistic gateway hot path.
void build_probes(Str* probes, const char** paths, u32 n) {
    for (u32 i = 0; i < kQueryRing; i++) {
        probes[i] = Str{paths[i % n], static_cast<u32>(strlen(paths[i % n]))};
    }
}

void build_probes_view(Str* probes, const char (*paths)[3], u32 n) {
    for (u32 i = 0; i < kQueryRing; i++) {
        probes[i] = Str{paths[i % n], static_cast<u32>(strlen(paths[i % n]))};
    }
}

template <u32 N>
void build_probes_view_n(Str* probes, const char (*paths)[N], u32 n) {
    for (u32 i = 0; i < kQueryRing; i++) {
        probes[i] = Str{paths[i % n], static_cast<u32>(strlen(paths[i % n]))};
    }
}

void run_saas(u32 n) {
    ArtTrie a;
    ByteRadixTrie b;
    build(a, kSaasRoutes, n, kSaasCount);
    build(b, kSaasRoutes, n, kSaasCount);
    Str probes[kQueryRing];
    build_probes(probes, kSaasRoutes, n);
    Result r;
    r.shape = "saas";
    r.n = n;
    r.art_hot_ns = bench_hot(a, probes, kQueryRing);
    r.br_hot_ns = bench_hot(b, probes, kQueryRing);
    r.art_cold_ns = bench_cold(a, probes, kQueryRing);
    r.br_cold_ns = bench_cold(b, probes, kQueryRing);
    r.art_nodes = a.node_count();
    r.br_nodes = b.node_count();
    print_row(r);
}

void run_dense() {
    DenseFanout d;
    ArtTrie a;
    ByteRadixTrie b;
    build_view(a, d.paths, d.count);
    build_view(b, d.paths, d.count);
    Str probes[kQueryRing];
    build_probes_view(probes, d.paths, d.count);
    Result r;
    r.shape = "dense-fanout";
    r.n = d.count;
    r.art_hot_ns = bench_hot(a, probes, kQueryRing);
    r.br_hot_ns = bench_hot(b, probes, kQueryRing);
    r.art_cold_ns = bench_cold(a, probes, kQueryRing);
    r.br_cold_ns = bench_cold(b, probes, kQueryRing);
    r.art_nodes = a.node_count();
    r.br_nodes = b.node_count();
    print_row(r);
}

// Fan-out sweep — find the crossover where ART starts beating
// ByteRadix. Each row: N distinct single-byte top-level paths under
// root, so root fan-out = N exactly. ART takes Node4 (≤4), Node16
// (≤16), Node48 (≤48), Node256 (≥49). ByteRadix scans up to N
// entries linearly at the root.
void run_fanout_sweep() {
    static char paths[128][2];
    for (u32 i = 0; i < 128; i++) {
        paths[i][0] = '/';
        paths[i][1] = static_cast<char>(0x40 + i);
    }
    for (u32 n : {4u, 8u, 16u, 24u, 32u, 48u, 64u, 96u, 127u}) {
        ArtTrie a;
        ByteRadixTrie b;
        for (u32 i = 0; i < n; i++) {
            const Str p{paths[i], 2};
            a.insert(p, 0, static_cast<u16>(i));
            b.insert(p, 0, static_cast<u16>(i));
        }
        Str probes[kQueryRing];
        for (u32 i = 0; i < kQueryRing; i++) probes[i] = Str{paths[i % n], 2};
        Result r;
        r.shape = "fanout-sweep";
        r.n = n;
        r.art_hot_ns = bench_hot(a, probes, kQueryRing);
        r.br_hot_ns = bench_hot(b, probes, kQueryRing);
        r.art_cold_ns = bench_cold(a, probes, kQueryRing);
        r.br_cold_ns = bench_cold(b, probes, kQueryRing);
        r.art_nodes = a.node_count();
        r.br_nodes = b.node_count();
        print_row(r);
    }
}

void run_deep() {
    DeepNarrow d;
    ArtTrie a;
    ByteRadixTrie b;
    build_view_n<32>(a, d.paths, d.count);
    build_view_n<32>(b, d.paths, d.count);
    Str probes[kQueryRing];
    build_probes_view_n<32>(probes, d.paths, d.count);
    Result r;
    r.shape = "deep-narrow";
    r.n = d.count;
    r.art_hot_ns = bench_hot(a, probes, kQueryRing);
    r.br_hot_ns = bench_hot(b, probes, kQueryRing);
    r.art_cold_ns = bench_cold(a, probes, kQueryRing);
    r.br_cold_ns = bench_cold(b, probes, kQueryRing);
    r.art_nodes = a.node_count();
    r.br_nodes = b.node_count();
    print_row(r);
}

}  // namespace

int main() {
    printf("ART vs ByteRadix — match() latency, ns per call (lower is better)\n");
    printf("Δ%%  positive = ART faster, negative = ART slower\n\n");
    print_header();
    for (u32 n : {32u, 64u, kSaasCount}) run_saas(n);
    run_dense();
    run_deep();
    printf("\n--- Fan-out sweep (single-byte top-level paths) ---\n");
    print_header();
    run_fanout_sweep();
    printf("\nState struct sizes (inline memory):\n");
    printf("  ArtTrie       %8zu B (%.1f KB)\n", sizeof(ArtTrie), sizeof(ArtTrie) / 1024.0);
    printf("  ByteRadixTrie %8zu B (%.1f KB)\n",
           sizeof(ByteRadixTrie),
           sizeof(ByteRadixTrie) / 1024.0);
    printf("  ratio         %.2fx\n",
           static_cast<double>(sizeof(ByteRadixTrie)) / static_cast<double>(sizeof(ArtTrie)));
    return 0;
}
