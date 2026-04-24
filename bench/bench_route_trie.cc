// Benchmark: route-matching strategies.
//
// Four variants compared head-to-head so the design choice is backed by
// data rather than folklore:
//
//   1. linear_scan (pre-PR)      — the old byte-prefix scan across the flat
//                                  RouteEntry array. Baseline.
//   2. trie_firstbyte_inlined    — segment trie with parallel u8 first-byte
//                                  index (httprouter / matchit layout).
//                                  Defined inline in this TU.
//   3. trie_simple_inlined       — segment trie with plain linear child
//                                  scan + full Str::eq per sibling. Same
//                                  algorithm as the shipped RouteTrie but
//                                  defined inline in this TU, so the
//                                  comparison against (2) is apples-to-
//                                  apples (same inlining context).
//   4. trie_simple_shipped       — the production RouteTrie::match. Defined
//                                  in rut_runtime.a, so calls from here
//                                  cross a translation-unit boundary — the
//                                  number worth comparing against (1).
//
// Headline takeaways on i7-10700 (hot cache):
//   - Trie beats linear by ~2.6× at 128 routes, crossover around 32.
//   - TU-boundary overhead (3 vs 4) is ~7–10% — bigger than the first-byte
//     vs simple algorithmic difference (1–3%, within noise).
//   - We ship `trie_simple` because the first-byte index adds bytes and
//     code for a difference well under run-to-run variance on the target
//     segment-length distribution (3–6 byte segments, small fan-outs).
//
// Build:  ninja -C build bench_route_trie
// Run:    ./build/bench/bench_route_trie

#include "bench.h"
#include "rut/runtime/route_trie.h"

using namespace rut;

// ---------------------------------------------------------------------------
// Rejected alternative: segment trie with a parallel u8 first-byte index,
// httprouter / matchit style. Kept here solely to document why we didn't
// ship it — the benchmark should show this is slower than the simple
// RouteTrie at our segment-length distribution.
// ---------------------------------------------------------------------------

namespace indexed_alt {

struct Node {
    Str segment{};
    FixedVec<u8, 32> child_first_bytes;
    FixedVec<u16, 32> children;
    static constexpr u16 kInvalid = 0xffffu;
    u16 route_idx_by_method[kMethodSlots] = {
        kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
};

class Trie {
public:
    static constexpr u32 kMaxNodes = 512;
    static constexpr u32 kMaxSegments = 16;

    Trie() { clear(); }

    void clear() {
        nodes.len = 0;
        Node root{};
        (void)nodes.push(root);
    }

    bool insert(Str path, u8 method_char, u16 route_idx) {
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return false;
        u16 cur = 0;
        for (u32 i = 0; i < segs.len; i++) {
            u16 child = find(cur, segs[i]);
            if (child == Node::kInvalid) {
                if (nodes.len >= kMaxNodes) return false;
                Node nn;
                nn.segment = segs[i];
                if (!nodes.push(nn)) return false;
                child = static_cast<u16>(nodes.len - 1);
                if (!nodes[cur].child_first_bytes.push(static_cast<u8>(segs[i].ptr[0])))
                    return false;
                if (!nodes[cur].children.push(child)) return false;
            }
            cur = child;
        }
        const u32 s = method_slot(method_char);
        if (nodes[cur].route_idx_by_method[s] == Node::kInvalid)
            nodes[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method_char) const {
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return Node::kInvalid;
        const u32 s = method_slot(method_char);
        u16 cur = 0;
        u16 best = pick(nodes[0], s);
        for (u32 i = 0; i < segs.len; i++) {
            u16 child = find(cur, segs[i]);
            if (child == Node::kInvalid) break;
            cur = child;
            const u16 c = pick(nodes[cur], s);
            if (c != Node::kInvalid) best = c;
        }
        return best;
    }

private:
    FixedVec<Node, kMaxNodes> nodes;

    static u16 pick(const Node& n, u32 slot) {
        if (slot != 0 && n.route_idx_by_method[slot] != Node::kInvalid)
            return n.route_idx_by_method[slot];
        return n.route_idx_by_method[0];
    }

    u16 find(u16 parent, Str seg) const {
        if (seg.len == 0) return Node::kInvalid;
        const auto& p = nodes[parent];
        const u8 first = static_cast<u8>(seg.ptr[0]);
        for (u32 i = 0; i < p.child_first_bytes.len; i++) {
            if (p.child_first_bytes[i] != first) continue;
            const u16 ci = p.children[i];
            if (nodes[ci].segment.eq(seg)) return ci;
        }
        return Node::kInvalid;
    }

    static u32 tokenize(Str path, FixedVec<Str, kMaxSegments>& out) {
        u32 start = 0;
        for (u32 i = 0; i <= path.len; i++) {
            const bool at_sep = (i == path.len) || (path.ptr[i] == '/');
            if (!at_sep) continue;
            if (i > start) {
                if (!out.push(Str{path.ptr + start, i - start})) return kMaxSegments + 1;
            }
            start = i + 1;
        }
        return out.len;
    }
};

}  // namespace indexed_alt

// ---------------------------------------------------------------------------
// Bench-local simple trie — same algorithm as the shipped RouteTrie, but
// defined inline in this TU so it competes apples-to-apples with the
// bench-local first-byte variant. Needed because production RouteTrie's
// match() lives in rut_runtime.a (out-of-line call), which adds 10-15% of
// overhead that the inlined bench-local variant doesn't pay.
// ---------------------------------------------------------------------------

namespace simple_alt {

struct Node {
    Str segment{};
    FixedVec<u16, 32> children;
    static constexpr u16 kInvalid = 0xffffu;
    u16 route_idx_by_method[kMethodSlots] = {
        kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
};

class Trie {
public:
    static constexpr u32 kMaxNodes = 512;
    static constexpr u32 kMaxSegments = 16;

    Trie() { clear(); }

    void clear() {
        nodes.len = 0;
        Node root{};
        (void)nodes.push(root);
    }

    bool insert(Str path, u8 method_char, u16 route_idx) {
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return false;
        u16 cur = 0;
        for (u32 i = 0; i < segs.len; i++) {
            u16 child = find(cur, segs[i]);
            if (child == Node::kInvalid) {
                if (nodes.len >= kMaxNodes) return false;
                Node nn;
                nn.segment = segs[i];
                if (!nodes.push(nn)) return false;
                child = static_cast<u16>(nodes.len - 1);
                if (!nodes[cur].children.push(child)) return false;
            }
            cur = child;
        }
        const u32 s = method_slot(method_char);
        if (nodes[cur].route_idx_by_method[s] == Node::kInvalid)
            nodes[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method_char) const {
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return Node::kInvalid;
        const u32 s = method_slot(method_char);
        u16 cur = 0;
        u16 best = pick(nodes[0], s);
        for (u32 i = 0; i < segs.len; i++) {
            u16 child = find(cur, segs[i]);
            if (child == Node::kInvalid) break;
            cur = child;
            const u16 c = pick(nodes[cur], s);
            if (c != Node::kInvalid) best = c;
        }
        return best;
    }

private:
    FixedVec<Node, kMaxNodes> nodes;

    static u16 pick(const Node& n, u32 slot) {
        if (slot != 0 && n.route_idx_by_method[slot] != Node::kInvalid)
            return n.route_idx_by_method[slot];
        return n.route_idx_by_method[0];
    }

    u16 find(u16 parent, Str seg) const {
        const auto& p = nodes[parent];
        for (u32 i = 0; i < p.children.len; i++) {
            if (nodes[p.children[i]].segment.eq(seg)) return p.children[i];
        }
        return Node::kInvalid;
    }

    static u32 tokenize(Str path, FixedVec<Str, kMaxSegments>& out) {
        u32 start = 0;
        for (u32 i = 0; i <= path.len; i++) {
            const bool at_sep = (i == path.len) || (path.ptr[i] == '/');
            if (!at_sep) continue;
            if (i > start) {
                if (!out.push(Str{path.ptr + start, i - start})) return kMaxSegments + 1;
            }
            start = i + 1;
        }
        return out.len;
    }
};

}  // namespace simple_alt

// ---------------------------------------------------------------------------
// C: Pre-PR linear scan. Kept here as a standalone helper — the production
// RouteConfig no longer has this path, so we model it directly on the same
// route set.
// ---------------------------------------------------------------------------

struct LinearEntry {
    const char* path;
    u32 path_len;
    u8 method;
    u16 route_idx;
};

static u16 linear_match(const LinearEntry* entries, u32 n, Str path, u8 method_char) {
    for (u32 i = 0; i < n; i++) {
        const auto& e = entries[i];
        if (e.method != 0 && e.method != method_char) continue;
        if (path.len < e.path_len) continue;
        bool matched = true;
        for (u32 j = 0; j < e.path_len; j++) {
            if (path.ptr[j] != e.path[j]) {
                matched = false;
                break;
            }
        }
        if (matched) return e.route_idx;
    }
    return 0xffffu;
}

// ---------------------------------------------------------------------------
// Route set generator — scales up synthetic API-gateway loads.
// ---------------------------------------------------------------------------

struct RouteSpec {
    char path[128];
    u32 path_len;
    u8 method;
};

// Generate N routes with a realistic shape:
//   - every 7th is a shallow top-level like "/health_i"
//   - the rest are 3-4 deep like "/api/v{1|2|3}/res_i/sub_j"
// Mix of GET and POST so method-slot is exercised.
static void generate_routes(RouteSpec* out, u32 n) {
    const char* tlds[] = {"api", "service", "v1", "v2", "internal", "public", "admin"};
    const char* resources[] = {
        "users", "orders", "products", "search", "items", "sessions", "events", "metrics"};
    const char* subs[] = {"list", "detail", "summary", "export", "batch", "stats", "featured"};
    const u32 n_tlds = sizeof(tlds) / sizeof(tlds[0]);
    const u32 n_res = sizeof(resources) / sizeof(resources[0]);
    const u32 n_subs = sizeof(subs) / sizeof(subs[0]);

    auto write_str = [](char*& p, const char* s) {
        while (*s) *p++ = *s++;
    };
    auto write_u32 = [](char*& p, u32 v) {
        char buf[12];
        u32 bi = 0;
        if (v == 0) buf[bi++] = '0';
        while (v > 0) {
            buf[bi++] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        while (bi > 0) *p++ = buf[--bi];
    };

    for (u32 i = 0; i < n; i++) {
        char* p = out[i].path;
        if (i % 7 == 0) {
            *p++ = '/';
            write_str(p, "endpoint_");
            write_u32(p, i);
        } else {
            *p++ = '/';
            write_str(p, tlds[i % n_tlds]);
            *p++ = '/';
            write_str(p, resources[(i / 3) % n_res]);
            *p++ = '/';
            write_str(p, subs[(i / 5) % n_subs]);
            *p++ = '_';
            write_u32(p, i);
        }
        out[i].path_len = static_cast<u32>(p - out[i].path);
        *p = '\0';
        out[i].method = (i % 3 == 0) ? 'G' : (i % 3 == 1 ? 'P' : 0);
    }
}

// Build a request set — half are hits (sampling the route set), half are
// misses (randomized near-miss paths). Permuted to defeat branch prediction.
static constexpr u32 kNumRequests = 64;

static void build_requests(
    const RouteSpec* routes, u32 nroutes, char (*paths)[128], Str* strs, u8* methods) {
    // Simple LCG for deterministic-but-spread sampling.
    u64 seed = 0x9E3779B97F4A7C15ULL;
    auto next = [&]() -> u32 {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(seed >> 32);
    };

    for (u32 i = 0; i < kNumRequests; i++) {
        if (i % 2 == 0 && nroutes > 0) {
            // Hit: pick a random route and copy its path verbatim.
            const u32 r = next() % nroutes;
            u32 len = routes[r].path_len;
            for (u32 k = 0; k < len; k++) paths[i][k] = routes[r].path[k];
            strs[i] = Str{paths[i], len};
            methods[i] = routes[r].method;
        } else {
            // Miss: synthesize a path that won't hit anything.
            char* p = paths[i];
            *p++ = '/';
            *p++ = 'u';  // "unknown_<i>"
            *p++ = 'n';
            *p++ = 'k';
            *p++ = '_';
            u32 v = next();
            for (u32 d = 0; d < 6; d++) {
                *p++ = static_cast<char>('a' + (v % 26));
                v /= 26;
            }
            strs[i] = Str{paths[i], static_cast<u32>(p - paths[i])};
            methods[i] = 'G';
        }
    }
}

// Cache-trasher: occupies L1 + L2 before each match batch so the route
// data must be re-fetched from L3. Simulates "this core just did something
// else", which is what happens in a gateway serving thousands of
// connections (request parsing, buffer management, TLS state).
alignas(64) static u8 g_trash[4 * 1024 * 1024];  // 4 MB — larger than L2
static u64 g_trash_acc = 0;

static void trash_cache() {
    // Touch one byte per cacheline across the whole buffer. The write
    // prevents the compiler from eliminating the read.
    u64 acc = g_trash_acc;
    for (u32 i = 0; i < sizeof(g_trash); i += 64) {
        acc ^= g_trash[i];
        g_trash[i] = static_cast<u8>(acc);
    }
    g_trash_acc = acc;
}

// ---------------------------------------------------------------------------
// Main — sweep across route counts and cache states
// ---------------------------------------------------------------------------

static constexpr u32 kMaxRoutes = 128;  // RouteConfig::kMaxRoutes

static RouteSpec g_routes[kMaxRoutes];
static char g_req_paths[kNumRequests][128];
static Str g_req_strs[kNumRequests];
static u8 g_req_methods[kNumRequests];

static LinearEntry g_linear[kMaxRoutes];

static void bench_scale(u32 n_routes, bool cold_cache) {
    RouteTrie shipped;                    // production — out-of-line call through rut_runtime.a
    indexed_alt::Trie firstbyte_variant;  // alternative (inlined)
    simple_alt::Trie simple_inlined;      // same algo as shipped but inlined — isolates TU overhead
    generate_routes(g_routes, n_routes);

    for (u32 i = 0; i < n_routes; i++) {
        Str path{g_routes[i].path, g_routes[i].path_len};
        if (!shipped.insert(path, g_routes[i].method, static_cast<u16>(i))) {
            bench::out("  [SKIP] shipped trie full at route ");
            bench::out_u64(i);
            bench::out("\n\n");
            return;
        }
        if (!firstbyte_variant.insert(path, g_routes[i].method, static_cast<u16>(i))) {
            bench::out("  [SKIP] firstbyte variant full at route ");
            bench::out_u64(i);
            bench::out("\n\n");
            return;
        }
        if (!simple_inlined.insert(path, g_routes[i].method, static_cast<u16>(i))) {
            bench::out("  [SKIP] simple_inlined full at route ");
            bench::out_u64(i);
            bench::out("\n\n");
            return;
        }
        g_linear[i] = {
            g_routes[i].path, g_routes[i].path_len, g_routes[i].method, static_cast<u16>(i)};
    }
    build_requests(g_routes, n_routes, g_req_paths, g_req_strs, g_req_methods);

    bench::out("\n  Routes: ");
    bench::out_u64(n_routes);
    bench::out("    Requests/iter: ");
    bench::out_u64(kNumRequests);
    bench::out("    Trie nodes: ");
    bench::out_u64(shipped.node_count());
    bench::out(cold_cache ? "    [cold cache]\n" : "    [hot cache]\n");

    bench::Bench b;
    char title[96];
    {
        char* p = title;
        const char* prefix = "Route match @ ";
        while (*prefix) *p++ = *prefix++;
        u32 v = n_routes;
        char buf[8];
        u32 bi = 0;
        if (v == 0) buf[bi++] = '0';
        while (v > 0) {
            buf[bi++] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        while (bi > 0) *p++ = buf[--bi];
        const char* tail = cold_cache ? " routes (cold)" : " routes (hot)";
        while (*tail) *p++ = *tail++;
        *p = '\0';
    }
    b.title(title);
    // Fewer iterations when cold-cache trashing dominates; keep stability.
    b.min_iterations(cold_cache ? 50000 : 500000);
    b.warmup(cold_cache ? 500 : 10000);
    b.epochs(7);
    b.print_header();

    b.run("linear_scan (pre-PR)", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = linear_match(g_linear, n_routes, g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("trie_firstbyte_inlined", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = firstbyte_variant.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("trie_simple_inlined", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = simple_inlined.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("trie_simple_shipped (ooLine)", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = shipped.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.compare();
    bench::out("\n");
}

int main() {
    const u32 sizes[] = {32, 64, 128};
    for (u32 si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        bench_scale(sizes[si], /*cold_cache=*/false);
    }
    for (u32 si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        bench_scale(sizes[si], /*cold_cache=*/true);
    }
    return 0;
}
