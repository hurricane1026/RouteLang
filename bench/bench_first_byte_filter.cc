// bench_first_byte_filter — empirically measure whether a 256-bit
// "registered first-byte" bitmap pre-check (Bloom-style fast reject)
// breaks even on realistic mixed-hit/miss traffic.
//
// Hypothesis (from the route_select.cc design discussion): adding a
// 1-bit lookup before dispatch->match() costs ~1-2 ns per call but
// saves ~10-30 ns per filtered miss. Break-even at ~4% filtered
// traffic. The question is what real workloads look like — what
// fraction of real traffic carries an unregistered first byte?
//
// What we measure
//   1. Per-call overhead of the bitmap check on 100% hit traffic
//      (worst case for the optimization — every call pays the cost,
//      none benefits).
//   2. Per-call savings on 100% miss-filterable traffic (bot scans
//      probing /.env, /wp-admin, etc — bytes never registered).
//   3. Break-even hit rate: at what hit fraction does the bitmap
//      pre-check become a net loss?
//
// Probe corpora
//   hits:    a SaaS-gateway route mix (same kSaasRoutes as
//            bench_route_art).
//   misses:  a curated set of real bot/scanner targets pulled from
//            published 2024 CDN abuse reports (Cloudflare Radar,
//            Imperva). Common patterns: WordPress probes (/wp-*),
//            secrets harvesting (/.env, /.git, /.aws/), legacy
//            admin paths (/phpmyadmin, /admin.php), CVE-specific
//            (/cgi-bin/, /actuator/env). First bytes are often
//            'w', '.', 'p', 'c', 'a' — overlap depends on what's
//            registered.

#include "rut/runtime/route_byte_radix.h"
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace rut;

namespace {

// Curated production-like SaaS routes (subset of bench_route_art's
// kSaasRoutes; first bytes after '/' span ~14 distinct values).
const char* kRoutes[] = {
    "/api/v1/users",
    "/api/v1/orders",
    "/api/v1/products",
    "/api/v1/sessions",
    "/api/v1/events",
    "/api/v1/teams",
    "/api/v1/projects",
    "/api/v1/messages",
    "/api/v1/channels",
    "/api/v2/users",
    "/api/v2/orders",
    "/admin",
    "/admin/users",
    "/admin/audit",
    "/admin/billing",
    "/oauth/token",
    "/oauth/authorize",
    "/oauth/jwks",
    "/webhooks/stripe",
    "/webhooks/github",
    "/webhooks/slack",
    "/health",
    "/healthz",
    "/metrics",
    "/_status",
    "/_ready",
    "/_live",
    "/internal/debug",
    "/internal/profile",
    "/v1",
    "/v2",
    "/static/css/main.css",
    "/static/js/app.js",
    "/docs",
    "/blog",
    "/about",
    "/contact",
    "/pricing",
    "/login",
    "/logout",
    "/signup",
    "/help",
    "/support",
};
constexpr u32 kRouteCount = sizeof(kRoutes) / sizeof(kRoutes[0]);

// Bot/scanner targets from real abuse logs. First bytes are diverse.
// What matters per-byte:
//   Filterable (NOT in registered first-byte set): paths whose first
//   byte after '/' isn't 'a','o','w','h','m','_','i','v','s','d','b','c','p','l'.
//   Above means: '.', 'p' (phpmyadmin), 'w' (wp-*), 'c' (cgi-bin),
//   'a' (admin.php) all overlap with registered. Truly filterable:
//   any byte not in [a,b,c,d,h,i,l,m,o,p,s,v,w,_].
const char* kBotMisses[] = {
    "/.env",                      // '.' — FILTERABLE
    "/.git/HEAD",                 // '.' — FILTERABLE
    "/.aws/credentials",          // '.' — FILTERABLE
    "/.ssh/id_rsa",               // '.' — FILTERABLE
    "/.well-known/security.txt",  // '.' — FILTERABLE
    "/.DS_Store",                 // '.' — FILTERABLE
    "/wp-admin/admin-ajax.php",   // 'w' — collides with /webhooks (NOT filterable)
    "/wp-login.php",              // 'w' — NOT filterable
    "/wp-config.php",             // 'w' — NOT filterable
    "/phpmyadmin/index.php",      // 'p' — collides with /pricing (NOT filterable)
    "/phpinfo.php",               // 'p' — NOT filterable
    "/admin.php",                 // 'a' — collides with /admin (NOT filterable)
    "/cgi-bin/test.cgi",          // 'c' — collides with /contact (NOT filterable)
    "/actuator/env",              // 'a' — NOT filterable
    "/server-status",             // 's' — NOT filterable
    "/robots.txt",                // 'r' — FILTERABLE
    "/sitemap.xml",               // 's' — NOT filterable
    "/favicon.ico",               // 'f' — FILTERABLE
    "/xmlrpc.php",                // 'x' — FILTERABLE
    "/ReadMe.md",                 // 'R' — FILTERABLE (uppercase)
    "/CHANGELOG",                 // 'C' — FILTERABLE (uppercase)
    "/Trash/",                    // 'T' — FILTERABLE
    "/jenkins/",                  // 'j' — FILTERABLE
    "/grafana",                   // 'g' — FILTERABLE
    "/kibana",                    // 'k' — FILTERABLE
    "/nginx_status",              // 'n' — FILTERABLE
    "/test.txt",                  // 't' — FILTERABLE
    "/uploads",                   // 'u' — FILTERABLE
    "/qa/",                       // 'q' — FILTERABLE
    "/zlib",                      // 'z' — FILTERABLE
    "/0",                         // '0' — FILTERABLE (digit)
    "/?id=1",                     // '?' — FILTERABLE
};
constexpr u32 kBotMissCount = sizeof(kBotMisses) / sizeof(kBotMisses[0]);

// Build a 256-bit registered-first-byte bitmap from the route list.
struct FirstByteBitmap {
    u64 bits[4] = {0, 0, 0, 0};
    void add(u8 b) { bits[b >> 6] |= (1ULL << (b & 63)); }
    bool contains(u8 b) const { return (bits[b >> 6] & (1ULL << (b & 63))) != 0; }
    u32 popcount() const {
        return static_cast<u32>(__builtin_popcountll(bits[0]) + __builtin_popcountll(bits[1]) +
                                __builtin_popcountll(bits[2]) + __builtin_popcountll(bits[3]));
    }
};

constexpr u32 kIters = 5'000'000;
constexpr u32 kRingSize = 4096;

// Bench: run kIters of match(), with `pre_filter` flagging whether
// to do the bitmap pre-check before each match.
double bench_with_filter(const ByteRadixTrie& t,
                         const FirstByteBitmap& bm,
                         const Str* probes,
                         u32 nprobes,
                         bool pre_filter) {
    volatile u32 sink = 0;
    // Warmup
    for (u32 i = 0; i < 50000; i++) {
        const Str& p = probes[i & (nprobes - 1)];
        if (pre_filter && p.len >= 2 && !bm.contains(static_cast<u8>(p.ptr[1]))) {
            sink ^= 0xffffu;
            continue;
        }
        sink ^= t.match(p, 0);
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (u32 i = 0; i < kIters; i++) {
        const Str& p = probes[i & (nprobes - 1)];
        if (pre_filter && p.len >= 2 && !bm.contains(static_cast<u8>(p.ptr[1]))) {
            sink ^= 0xffffu;  // simulate kRouteIdxInvalid return
            continue;
        }
        sink ^= t.match(p, 0);
    }
    auto end = std::chrono::high_resolution_clock::now();
    (void)sink;
    return std::chrono::duration<double, std::nano>(end - start).count() / kIters;
}

// Probe-ring builder: mix `hit_count` hits and `miss_count` misses.
// Returns total = hit_count + miss_count = kRingSize. Misses are
// drawn round-robin from kBotMisses (which has ~50% filterable).
void build_mix(Str* probes, u32 hit_pct) {
    const u32 n_hit = (kRingSize * hit_pct) / 100;
    const u32 n_miss = kRingSize - n_hit;
    u32 idx = 0;
    for (u32 i = 0; i < n_hit; i++, idx++) {
        const char* p = kRoutes[i % kRouteCount];
        probes[idx] = Str{p, static_cast<u32>(strlen(p))};
    }
    for (u32 i = 0; i < n_miss; i++, idx++) {
        const char* p = kBotMisses[i % kBotMissCount];
        probes[idx] = Str{p, static_cast<u32>(strlen(p))};
    }
}

// Compute filterable miss fraction given a bitmap and the bot list.
double filterable_miss_fraction(const FirstByteBitmap& bm) {
    u32 filterable = 0;
    for (u32 i = 0; i < kBotMissCount; i++) {
        const char* p = kBotMisses[i];
        const u32 plen = static_cast<u32>(strlen(p));
        if (plen < 2) continue;
        if (!bm.contains(static_cast<u8>(p[1]))) filterable++;
    }
    return static_cast<double>(filterable) / kBotMissCount;
}

}  // namespace

int main() {
    // Build trie + bitmap.
    ByteRadixTrie t;
    FirstByteBitmap bm;
    for (u32 i = 0; i < kRouteCount; i++) {
        const u32 plen = static_cast<u32>(strlen(kRoutes[i]));
        t.insert(Str{kRoutes[i], plen}, 0, static_cast<u16>(i));
        if (plen >= 2) bm.add(static_cast<u8>(kRoutes[i][1]));
    }

    printf("=== First-byte bitmap fast-reject — empirical break-even ===\n\n");
    printf("Registered first-byte set: %u distinct bytes (out of 256)\n", bm.popcount());
    printf("Bot-probe miss list:       %u entries\n", kBotMissCount);
    printf("Filterable bot fraction:   %.0f%% (rest collide with registered first bytes)\n\n",
           filterable_miss_fraction(bm) * 100.0);

    printf("%-12s | %-12s %-12s %-7s | %-12s\n",
           "hit_pct",
           "no_filter",
           "with_filter",
           "Δ ns",
           "filter_save");
    printf("---------------------------------------------------------------------\n");

    // Run sweep across hit ratios.
    for (u32 hit_pct : {0u, 25u, 50u, 75u, 90u, 95u, 99u, 100u}) {
        Str probes[kRingSize];
        build_mix(probes, hit_pct);
        const double t_no = bench_with_filter(t, bm, probes, kRingSize, false);
        const double t_yes = bench_with_filter(t, bm, probes, kRingSize, true);
        const double delta = t_yes - t_no;
        const char* verdict = (delta < 0) ? "WIN" : "loss";
        printf("%9u%%   | %9.2fns   %9.2fns   %+5.2f | %s\n", hit_pct, t_no, t_yes, delta, verdict);
    }

    // Compute break-even from "100% miss-filterable" vs "100% hit"
    // costs to validate the analytical model.
    Str hits_only[kRingSize];
    Str misses_only[kRingSize];
    build_mix(hits_only, 100);
    build_mix(misses_only, 0);
    const double hit_no = bench_with_filter(t, bm, hits_only, kRingSize, false);
    const double hit_yes = bench_with_filter(t, bm, hits_only, kRingSize, true);
    const double miss_no = bench_with_filter(t, bm, misses_only, kRingSize, false);
    const double miss_yes = bench_with_filter(t, bm, misses_only, kRingSize, true);

    const double overhead_per_hit = hit_yes - hit_no;
    const double save_per_miss_filtered = miss_no - miss_yes;
    // Break-even: overhead × hit_count = save × filtered_miss_count
    // where filtered_miss_count = miss_count × filterable_fraction.
    // Solve for break-even miss_pct:
    const double f = filterable_miss_fraction(bm);
    const double break_even_miss_pct =
        100.0 * overhead_per_hit / (overhead_per_hit + save_per_miss_filtered * f);

    printf("\n=== Per-call costs ===\n");
    printf("  overhead on a hit:          %+.2f ns/call\n", overhead_per_hit);
    printf("  saving on a filterable miss: %+.2f ns/call\n", save_per_miss_filtered);
    printf("  filterable miss fraction:    %.0f%%\n", f * 100.0);
    printf("\nBreak-even traffic miss_pct: %.1f%% — beyond this miss rate the\n",
           break_even_miss_pct);
    printf("filter is a net win, below it a net loss.\n");
    return 0;
}
