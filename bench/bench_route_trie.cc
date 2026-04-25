// Benchmark: route-matching strategies — 11 variants head-to-head so the
// design discussion is anchored in measurement, not folklore.
//
// Production candidates (4) — these survived the round-1 comparison:
//   1. linear_scan (pre-PR)      — old byte-prefix scan over RouteEntry[].
//   2. trie_firstbyte_inlined    — segment trie + parallel u8 first-byte
//                                  index (httprouter / matchit layout).
//   3. trie_simple_inlined       — same algorithm as shipped RouteTrie but
//                                  defined inline in this TU (apples-to-
//                                  apples vs (2), which is also inlined).
//   4. trie_simple_shipped       — production RouteTrie::match crossing
//                                  the TU boundary into rut_runtime.a.
//
// Round-2 candidates (7) — explored after the perf-counter framework
// landed; some are real upgrades, some are documented as not paying off:
//   5. hash_full_path            — exact-match hash table on the whole path.
//                                  Loses prefix semantics; our generator
//                                  produces verbatim hits so it's a fair
//                                  upper-bound for "give up prefixes".
//   6. hash_first_segment        — bucket by first-segment hash, linear
//                                  within bucket. Cheap for top-level-
//                                  clustered configs (typical gateways).
//   7. perfect_hash              — search at build time for a hash seed
//                                  that produces no collisions. Fails to
//                                  converge at N=128 with our 16-bit seed
//                                  search + FNV-1a; would need CHD/BBHash
//                                  to scale.
//   8. byte_radix (compressed)   — byte-level edge-compressed radix trie.
//                                  Surprise winner — at 128 routes it's
//                                  the fastest variant tested.
//   9. simd_linear (AVX2)        — linear scan with vpcmpeqb-parallel
//                                  byte compare on a 32-byte prefix slot.
//                                  Loses to scalar linear at our route-
//                                  length distribution: the unconditional
//                                  32-byte work eats the early-exit win.
//  10. patricia (crit-bit)       — bit-level discriminating trie. Logn
//                                  descent but per-step bit-extract cost
//                                  drops IPC; mid-pack at 128 routes.
//  11. aho_corasick              — DFA over patterns (goto + per-state
//                                  transition list). Competitive but no
//                                  better than the simple trie at this N.
//
// Headline takeaways on i7-10700 (hot cache, 128 routes):
//   - byte_radix wins at 7.85× over linear_scan; segment trie 3.77×.
//     The compressed-edge layout fits more routes in fewer cache lines,
//     and the IPC ceiling (4.25) is the highest in the table.
//   - hash_full_path is 6.46× — the floor for exact-match dispatch when
//     prefix semantics are given up. Real gateways need prefixes, so it's
//     not a drop-in replacement, but defines what's possible.
//   - simd_linear is essentially a wash with linear_scan (1.01× at 128)
//     because routes don't get long enough to amortize the parallel work
//     against scalar early-exit.
//   - We continue to ship the segment trie. byte_radix beats it on this
//     synthetic load but its semantic match for prefix routing needs more
//     thought (interaction with method-specific terminals on intermediate
//     paths) before we'd flip the production path.
//
// Build:  ninja -C build bench_route_trie
// Run:    taskset -c 0 ./build/bench/bench_route_trie

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
        const u32 s = method_slot(method_char);
        if (s == kMethodSlotInvalid) return false;  // mirror production reject
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
        if (nodes[cur].route_idx_by_method[s] == Node::kInvalid)
            nodes[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method_char) const {
        const u32 s = method_slot(method_char);
        if (s == kMethodSlotInvalid) return Node::kInvalid;  // mirror production reject
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return Node::kInvalid;
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
        const u32 s = method_slot(method_char);
        if (s == kMethodSlotInvalid) return false;  // mirror production reject
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
        if (nodes[cur].route_idx_by_method[s] == Node::kInvalid)
            nodes[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method_char) const {
        const u32 s = method_slot(method_char);
        if (s == kMethodSlotInvalid) return Node::kInvalid;  // mirror production reject
        FixedVec<Str, kMaxSegments> segs;
        if (tokenize(path, segs) > kMaxSegments) return Node::kInvalid;
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

// ===========================================================================
// Additional algorithm variants — explore the design space beyond the
// trie/linear axis. None of these ship; they exist here so the design
// rationale is backed by data and each candidate gets a fair fight on the
// same generator + perf-counter harness.
// ===========================================================================
//
// Naming convention: variant name = data structure + interesting trait.
// All variants are bench-local (defined in this TU) so call-cost overhead
// stays out of the comparison.

// FNV-1a for short byte buffers — used by the hash variants and the
// perfect-hash search. 64-bit so rare collisions don't dominate at our N.
static inline u64 fnv1a(const char* p, u32 len, u64 seed = 0xcbf29ce484222325ULL) {
    u64 h = seed;
    for (u32 i = 0; i < len; i++) {
        h ^= static_cast<u8>(p[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

// ---------------------------------------------------------------------------
// 1. hash_full_path — exact-match hash table on the entire path.
// ---------------------------------------------------------------------------
// Defines the floor for "what's the fastest possible per-match if you give
// up prefix semantics?" Open-addressing, 2N power-of-2 capacity. Methods
// are encoded into the key by hashing (path bytes ++ method byte).
// Limitation: prefix matches like "GET /users/123" hitting a route at
// "/users" don't work — we'd need a multi-pass strip-and-retry chain on
// top. Our generator produces verbatim hits so this is fair as a lookup-
// floor measurement.

namespace hash_full {

class Table {
public:
    static constexpr u32 kCap = 512;  // ≥ 2 × kMaxRoutes(128) × 2 (worst-case)

    Table() { clear(); }
    void clear() {
        for (u32 i = 0; i < kCap; i++) slots[i].route_idx = kInvalid;
        n_ = 0;
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        if (n_ >= kCap / 2) return false;  // keep load factor below 0.5
        const u64 h = key_hash(path, method);
        u32 i = static_cast<u32>(h) & (kCap - 1);
        for (u32 probe = 0; probe < kCap; probe++) {
            auto& s = slots[i];
            if (s.route_idx == kInvalid) {
                s.path = path;
                s.method = method;
                s.route_idx = route_idx;
                n_++;
                return true;
            }
            if (s.method == method && s.path.eq(path)) return true;  // duplicate, ignore
            i = (i + 1) & (kCap - 1);
        }
        return false;
    }

    u16 match(Str path, u8 method) const {
        const u64 h = key_hash(path, method);
        u32 i = static_cast<u32>(h) & (kCap - 1);
        for (u32 probe = 0; probe < kCap; probe++) {
            const auto& s = slots[i];
            if (s.route_idx == kInvalid) {
                if (method != 0) {
                    // Fall back to "any" slot so methods 0 and 'G' return
                    // the same 'any' route a linear scan would have hit.
                    return any_match(path);
                }
                return kInvalid;
            }
            if (s.method == method && s.path.eq(path)) return s.route_idx;
            i = (i + 1) & (kCap - 1);
        }
        return kInvalid;
    }

private:
    static constexpr u16 kInvalid = 0xffffu;
    struct Slot {
        Str path{};
        u8 method = 0;
        u16 route_idx = kInvalid;
    };
    Slot slots[kCap];
    u32 n_ = 0;

    static u64 key_hash(Str path, u8 method) {
        u64 h = fnv1a(path.ptr, path.len);
        h ^= static_cast<u64>(method) * 0x100000001b3ULL;
        return h;
    }

    u16 any_match(Str path) const {
        const u64 h = key_hash(path, 0);
        u32 i = static_cast<u32>(h) & (kCap - 1);
        for (u32 probe = 0; probe < kCap; probe++) {
            const auto& s = slots[i];
            if (s.route_idx == kInvalid) return kInvalid;
            if (s.method == 0 && s.path.eq(path)) return s.route_idx;
            i = (i + 1) & (kCap - 1);
        }
        return kInvalid;
    }
};

}  // namespace hash_full

// ---------------------------------------------------------------------------
// 2. hash_first_segment — hash on the first path segment, linear within bucket.
// ---------------------------------------------------------------------------
// Real configs cluster heavily by top-level prefix ("/api/...", "/auth/...",
// "/health"). Hashing on the first segment gets us O(B) bucket lookup +
// O(N/B) within-bucket linear scan, where B is the bucket count. At our
// distribution (3-6 byte segments, ~20% of routes share a top-level), most
// buckets hold 1-3 routes.

namespace hash_seg {

class Table {
public:
    static constexpr u32 kBuckets = 64;
    static constexpr u32 kCap = 256;

    Table() { clear(); }
    void clear() {
        for (u32 b = 0; b < kBuckets; b++) bucket_lens[b] = 0;
        n_ = 0;
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        if (n_ >= kCap) return false;
        Str first = first_segment(path);
        const u32 b = static_cast<u32>(fnv1a(first.ptr, first.len)) & (kBuckets - 1);
        if (bucket_lens[b] >= kPerBucket) return false;
        auto& e = entries[b][bucket_lens[b]++];
        e.path = path;
        e.method = method;
        e.route_idx = route_idx;
        n_++;
        return true;
    }

    u16 match(Str path, u8 method) const {
        Str first = first_segment(path);
        const u32 b = static_cast<u32>(fnv1a(first.ptr, first.len)) & (kBuckets - 1);
        const u32 n = bucket_lens[b];
        for (u32 i = 0; i < n; i++) {
            const auto& e = entries[b][i];
            if (e.method != 0 && e.method != method) continue;
            if (path.len < e.path.len) continue;
            // Byte-prefix compare (matches linear_match's semantics).
            bool ok = true;
            for (u32 j = 0; j < e.path.len; j++) {
                if (path.ptr[j] != e.path.ptr[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) return e.route_idx;
        }
        return kInvalid;
    }

private:
    static constexpr u16 kInvalid = 0xffffu;
    static constexpr u32 kPerBucket = 16;
    struct Entry {
        Str path{};
        u8 method = 0;
        u16 route_idx = kInvalid;
    };
    Entry entries[kBuckets][kPerBucket];
    u32 bucket_lens[kBuckets];
    u32 n_ = 0;

    static Str first_segment(Str p) {
        u32 start = (p.len > 0 && p.ptr[0] == '/') ? 1 : 0;
        u32 end = start;
        while (end < p.len && p.ptr[end] != '/') end++;
        return Str{p.ptr + start, end - start};
    }
};

}  // namespace hash_seg

// ---------------------------------------------------------------------------
// 3. perfect_hash — search at build time for a hash seed that produces no
//                   collisions for the registered routes.
// ---------------------------------------------------------------------------
// Static config + RCU rebuild = build-time cost is free. At runtime, hash →
// bucket → single eq compare. Defines the absolute floor for static hashed
// dispatch (one hash + one compare per match). Build cost grows with N but
// at N=128 the seed search converges in microseconds in practice.

namespace perfect_hash {

class Table {
public:
    static constexpr u32 kCap = 256;  // power of 2, ≥ 2 × kMaxRoutes

    Table() { clear(); }
    void clear() {
        for (u32 i = 0; i < kCap; i++) slots[i].route_idx = kInvalid;
        n_ = 0;
        seed_ = 0;
    }

    // Finalize builds the perfect hash table once all routes are added.
    // Caller pattern: fill `pending_` via insert_pending(), then finalize().
    bool insert_pending(Str path, u8 method, u16 route_idx) {
        if (n_ >= kPending) return false;
        pending_[n_++] = {path, method, route_idx};
        return true;
    }

    bool finalize() {
        // Search for a seed where every route maps to a distinct bucket.
        for (u64 s = 1; s < (1ULL << 16); s++) {
            bool collision = false;
            for (u32 i = 0; i < n_; i++) {
                const u32 b = bucket_for(pending_[i].path, pending_[i].method, s);
                if (slots[b].route_idx != kInvalid) {
                    collision = true;
                    break;
                }
                slots[b].path = pending_[i].path;
                slots[b].method = pending_[i].method;
                slots[b].route_idx = pending_[i].route_idx;
            }
            if (!collision) {
                seed_ = s;
                return true;
            }
            for (u32 i = 0; i < kCap; i++) slots[i].route_idx = kInvalid;
        }
        return false;
    }

    u16 match(Str path, u8 method) const {
        const u32 b = bucket_for(path, method, seed_);
        const auto& s = slots[b];
        if (s.route_idx == kInvalid) return kInvalid;
        // Verify — the perfect hash guarantees no collisions among
        // REGISTERED routes, but a request that hashes into a populated
        // slot still needs an eq check to reject misses.
        if (s.method != 0 && s.method != method) return kInvalid;
        if (!s.path.eq(path)) return kInvalid;
        return s.route_idx;
    }

private:
    static constexpr u16 kInvalid = 0xffffu;
    static constexpr u32 kPending = 256;
    struct Slot {
        Str path{};
        u8 method = 0;
        u16 route_idx = kInvalid;
    };
    Slot slots[kCap];
    struct Pending {
        Str path{};
        u8 method = 0;
        u16 route_idx = kInvalid;
    };
    Pending pending_[kPending];
    u32 n_ = 0;
    u64 seed_ = 0;

    static u32 bucket_for(Str path, u8 method, u64 seed) {
        u64 h = fnv1a(path.ptr, path.len, seed);
        h ^= static_cast<u64>(method) * 0x100000001b3ULL;
        return static_cast<u32>(h) & (kCap - 1);
    }
};

}  // namespace perfect_hash

// ---------------------------------------------------------------------------
// 4. byte_radix — byte-level edge-compressed radix trie.
// ---------------------------------------------------------------------------
// At our segment-length distribution, segment-radix and byte-radix collapse
// to similar shapes (most segments are 3-6 bytes, and edge compression in a
// byte trie produces edges roughly the same length as a segment). The
// difference is at the BRANCH points: a segment trie can have a wide
// fan-out at one point ("/api" → {users, orders, products, ...}); a byte
// trie has narrower fan-out per node but more nodes. Including this here
// because the textbook "compressed radix" recommendation is byte-level —
// worth measuring rather than assuming segment-radix dominates.

namespace byte_radix {

struct Node {
    Str edge{};
    static constexpr u32 kMaxChildren = 16;
    FixedVec<u16, kMaxChildren> children;
    static constexpr u16 kInvalid = 0xffffu;
    u16 route_idx_by_method[kMethodSlots] = {
        kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
};

class Trie {
public:
    static constexpr u32 kMaxNodes = 1024;
    Trie() { clear(); }
    void clear() {
        nodes.len = 0;
        Node root{};
        (void)nodes.push(root);
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        const u32 s = method_slot(method);
        if (s == kMethodSlotInvalid) return false;
        // Strip leading '/' and trailing '/' so the trie keys on substantive
        // bytes only — matches the segment trie's normalization at byte level.
        u32 lo = 0, hi = path.len;
        if (lo < hi && path.ptr[lo] == '/') lo++;
        while (hi > lo && path.ptr[hi - 1] == '/') hi--;
        Str p{path.ptr + lo, hi - lo};
        u16 cur = 0;
        u32 i = 0;
        while (i < p.len) {
            // Find a child whose first byte matches.
            const auto& parent = nodes[cur];
            u16 child = Node::kInvalid;
            for (u32 c = 0; c < parent.children.len; c++) {
                const u16 ci = parent.children[c];
                if (nodes[ci].edge.len > 0 && nodes[ci].edge.ptr[0] == p.ptr[i]) {
                    child = ci;
                    break;
                }
            }
            if (child == Node::kInvalid) {
                if (nodes.len >= kMaxNodes) return false;
                Node nn;
                nn.edge = Str{p.ptr + i, p.len - i};
                if (!nodes.push(nn)) return false;
                const u16 ni = static_cast<u16>(nodes.len - 1);
                if (!nodes[cur].children.push(ni)) return false;
                cur = ni;
                i = p.len;
                break;
            }
            // Match as many bytes of the edge as possible.
            Str e = nodes[child].edge;
            u32 k = 0;
            while (k < e.len && i + k < p.len && e.ptr[k] == p.ptr[i + k]) k++;
            if (k == e.len) {
                cur = child;
                i += k;
                continue;
            }
            // Need to split the edge at byte k.
            if (nodes.len >= kMaxNodes) return false;
            Node split;
            split.edge = Str{e.ptr + k, e.len - k};
            // Move existing child's terminal slots + children into split.
            for (u32 m = 0; m < kMethodSlots; m++)
                split.route_idx_by_method[m] = nodes[child].route_idx_by_method[m];
            split.children = nodes[child].children;
            if (!nodes.push(split)) return false;
            const u16 split_idx = static_cast<u16>(nodes.len - 1);
            // Truncate child's edge to the common prefix.
            nodes[child].edge = Str{e.ptr, k};
            for (u32 m = 0; m < kMethodSlots; m++)
                nodes[child].route_idx_by_method[m] = Node::kInvalid;
            nodes[child].children.len = 0;
            if (!nodes[child].children.push(split_idx)) return false;
            cur = child;
            i += k;
        }
        if (nodes[cur].route_idx_by_method[s] == Node::kInvalid)
            nodes[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method) const {
        const u32 s = method_slot(method);
        if (s == kMethodSlotInvalid) return Node::kInvalid;
        u32 lo = 0, hi = path.len;
        if (lo < hi && path.ptr[lo] == '/') lo++;
        // Don't strip '?' / '#' — bench harness sends bare paths.
        while (hi > lo && path.ptr[hi - 1] == '/') hi--;
        Str p{path.ptr + lo, hi - lo};
        u16 cur = 0;
        u16 best = pick(nodes[0], s);
        u32 i = 0;
        while (i <= p.len) {
            if (i == p.len) break;
            const auto& parent = nodes[cur];
            u16 child = Node::kInvalid;
            for (u32 c = 0; c < parent.children.len; c++) {
                const u16 ci = parent.children[c];
                if (nodes[ci].edge.len > 0 && nodes[ci].edge.ptr[0] == p.ptr[i]) {
                    child = ci;
                    break;
                }
            }
            if (child == Node::kInvalid) break;
            Str e = nodes[child].edge;
            if (i + e.len > p.len) break;
            for (u32 k = 0; k < e.len; k++) {
                if (e.ptr[k] != p.ptr[i + k]) return best;
            }
            cur = child;
            i += e.len;
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
};

}  // namespace byte_radix

// ---------------------------------------------------------------------------
// 5. simd_linear — linear scan, but with AVX2 cmpeq across N routes in parallel.
// ---------------------------------------------------------------------------
// "What if linear_scan were just vectorized?" Pack each route's first 32
// bytes into an aligned slot, AVX2 vpcmpeqb against the request's first 32
// bytes, vpmovmskb to extract the match mask, and rank-by-popcount. Routes
// longer than 32 bytes still need a scalar tail compare on the candidate.

#include <immintrin.h>

namespace simd_linear {

class Table {
public:
    static constexpr u32 kCap = 256;
    static constexpr u32 kPrefixLen = 32;

    Table() { clear(); }
    void clear() {
        n_ = 0;
        for (u32 i = 0; i < kCap; i++) {
            for (u32 j = 0; j < kPrefixLen; j++) prefixes[i][j] = 0;
            full_paths[i] = Str{};
            full_lens[i] = 0;
            methods[i] = 0;
            route_idxs[i] = kInvalid;
        }
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        if (n_ >= kCap) return false;
        const u32 take = path.len < kPrefixLen ? path.len : kPrefixLen;
        for (u32 j = 0; j < take; j++) prefixes[n_][j] = static_cast<u8>(path.ptr[j]);
        // Pad shorter prefixes with a sentinel byte (0x80) that's outside
        // ASCII so it can't accidentally match a request byte.
        for (u32 j = take; j < kPrefixLen; j++) prefixes[n_][j] = 0x80;
        full_paths[n_] = path;
        full_lens[n_] = path.len;
        methods[n_] = method;
        route_idxs[n_] = route_idx;
        n_++;
        return true;
    }

    __attribute__((target("avx2"))) u16 match(Str path, u8 method) const {
        if (path.len < 1) return kInvalid;
        // Load request first 32 bytes (zero-padded for short paths).
        alignas(32) u8 req[kPrefixLen];
        const u32 take = path.len < kPrefixLen ? path.len : kPrefixLen;
        for (u32 j = 0; j < take; j++) req[j] = static_cast<u8>(path.ptr[j]);
        for (u32 j = take; j < kPrefixLen; j++) req[j] = 0;
        const __m256i v_req = _mm256_load_si256(reinterpret_cast<const __m256i*>(req));
        for (u32 i = 0; i < n_; i++) {
            if (methods[i] != 0 && methods[i] != method) continue;
            const __m256i v_route =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(prefixes[i]));
            const __m256i eq = _mm256_cmpeq_epi8(v_req, v_route);
            const u32 mask = static_cast<u32>(_mm256_movemask_epi8(eq));
            const u32 plen = full_lens[i];
            const u32 cmp_len = plen < kPrefixLen ? plen : kPrefixLen;
            const u32 needed = (1u << cmp_len) - 1;
            if ((mask & needed) != needed) continue;
            // Tail compare for routes longer than the SIMD prefix.
            if (plen > kPrefixLen) {
                if (path.len < plen) continue;
                bool ok = true;
                for (u32 j = kPrefixLen; j < plen; j++) {
                    if (path.ptr[j] != full_paths[i].ptr[j]) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;
            } else {
                if (path.len < plen) continue;
            }
            return route_idxs[i];
        }
        return kInvalid;
    }

private:
    static constexpr u16 kInvalid = 0xffffu;
    alignas(32) u8 prefixes[kCap][kPrefixLen];
    Str full_paths[kCap];
    u32 full_lens[kCap];
    u8 methods[kCap];
    u16 route_idxs[kCap];
    u32 n_ = 0;
};

}  // namespace simd_linear

// ---------------------------------------------------------------------------
// 6. patricia — bit-level crit-bit trie (Knuth / Sedgewick).
// ---------------------------------------------------------------------------
// Internal nodes branch on a single discriminating bit; keys are stored at
// leaves. Lookups walk one bit-test per level, then verify the candidate
// leaf with a full key compare. At our short keys (8-40 bytes = 64-320
// bits) and small N, the tree depth is log2(N) levels — 7 for N=128.

namespace patricia {

class Trie {
public:
    static constexpr u32 kMaxNodes = 256;  // ≥ 2 × kMaxRoutes for internal+leaf
    static constexpr u32 kMaxLeaves = 256;
    static constexpr u16 kInvalid = 0xffffu;

    Trie() { clear(); }
    void clear() {
        n_internal = 0;
        n_leaves = 0;
        root = kInvalid;
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        if (n_leaves >= kMaxLeaves) return false;
        const u16 leaf_id = static_cast<u16>(n_leaves);
        leaves[n_leaves++] = {path, method, route_idx};
        if (root == kInvalid) {
            root = leaf_idx_to_ref(leaf_id);
            return true;
        }
        // Walk to find the existing leaf with the longest crit-bit match.
        u16 cur = root;
        u32 trail[64];
        u32 depth = 0;
        while (is_internal(cur)) {
            trail[depth++] = ref_to_internal(cur);
            const auto& n = internals[ref_to_internal(cur)];
            cur = bit_at(path, n.bit) ? n.right : n.left;
            if (depth >= 64) return false;
        }
        const auto& other = leaves[ref_to_leaf(cur)];
        const u32 nb = first_diff_bit(path, other.path);
        if (nb == 0xffffffffu) return true;  // duplicate, ignore
        // Insert a new internal node at the right depth: walk down again
        // until we'd descend past `nb` or hit a deeper bit.
        if (n_internal >= kMaxNodes) return false;
        const u16 new_int = static_cast<u16>(n_internal++);
        internals[new_int].bit = nb;
        const bool right_is_new = bit_at(path, nb);
        internals[new_int].left = right_is_new ? cur : leaf_idx_to_ref(leaf_id);
        internals[new_int].right = right_is_new ? leaf_idx_to_ref(leaf_id) : cur;
        // Splice into trail.
        if (depth == 0) {
            root = internal_idx_to_ref(new_int);
        } else {
            // Find the parent at trail[d-1] whose child path went through bit < nb.
            u32 d = depth;
            while (d > 0 && internals[trail[d - 1]].bit >= nb) d--;
            if (d == 0) {
                // Re-link old root.
                if (root == cur || internals[ref_to_internal(root)].bit >= nb) {
                    internals[new_int].left = right_is_new ? root : leaf_idx_to_ref(leaf_id);
                    internals[new_int].right = right_is_new ? leaf_idx_to_ref(leaf_id) : root;
                    root = internal_idx_to_ref(new_int);
                }
            } else {
                auto& parent = internals[trail[d - 1]];
                u16 to_replace = bit_at(path, parent.bit) ? parent.right : parent.left;
                if (bit_at(path, parent.bit))
                    parent.right = internal_idx_to_ref(new_int);
                else
                    parent.left = internal_idx_to_ref(new_int);
                internals[new_int].left = right_is_new ? to_replace : leaf_idx_to_ref(leaf_id);
                internals[new_int].right = right_is_new ? leaf_idx_to_ref(leaf_id) : to_replace;
            }
        }
        return true;
    }

    u16 match(Str path, u8 method) const {
        if (root == kInvalid) return kInvalid;
        u16 cur = root;
        while (is_internal(cur)) {
            const auto& n = internals[ref_to_internal(cur)];
            cur = bit_at(path, n.bit) ? n.right : n.left;
        }
        const auto& l = leaves[ref_to_leaf(cur)];
        if (!l.path.eq(path)) return kInvalid;
        if (l.method != 0 && l.method != method) return kInvalid;
        return l.route_idx;
    }

private:
    struct Internal {
        u32 bit = 0;
        u16 left = kInvalid;
        u16 right = kInvalid;
    };
    struct Leaf {
        Str path{};
        u8 method = 0;
        u16 route_idx = kInvalid;
    };
    Internal internals[kMaxNodes];
    Leaf leaves[kMaxLeaves];
    u32 n_internal = 0;
    u32 n_leaves = 0;
    u16 root = kInvalid;

    // Top bit of the u16 reference distinguishes internal (0) from leaf (1).
    static u16 internal_idx_to_ref(u16 i) { return i; }
    static u16 leaf_idx_to_ref(u16 i) { return static_cast<u16>(i | 0x8000u); }
    static bool is_internal(u16 r) { return r != kInvalid && (r & 0x8000u) == 0; }
    static u16 ref_to_internal(u16 r) { return r; }
    static u16 ref_to_leaf(u16 r) { return static_cast<u16>(r & 0x7fffu); }

    static bool bit_at(Str p, u32 bit) {
        const u32 byte_idx = bit >> 3;
        if (byte_idx >= p.len) return false;
        return (static_cast<u8>(p.ptr[byte_idx]) >> (7 - (bit & 7))) & 1;
    }

    static u32 first_diff_bit(Str a, Str b) {
        const u32 lim = a.len < b.len ? a.len : b.len;
        for (u32 i = 0; i < lim; i++) {
            const u8 xa = static_cast<u8>(a.ptr[i]);
            const u8 xb = static_cast<u8>(b.ptr[i]);
            if (xa != xb) {
                const u8 x = xa ^ xb;
                // Highest differing bit.
                for (int bi = 7; bi >= 0; bi--) {
                    if (x & (1u << bi)) return i * 8 + (7 - bi);
                }
            }
        }
        if (a.len == b.len) return 0xffffffffu;
        return lim * 8;
    }
};

}  // namespace patricia

// ---------------------------------------------------------------------------
// 7. aho_corasick — multi-pattern DFA over the byte alphabet.
// ---------------------------------------------------------------------------
// Walk the request bytes through a goto + fail automaton, tracking the
// longest pattern that is a *prefix* of the input we've consumed so far.
// Aho-Corasick natively reports all substring matches; for routing we
// adapt it to prefix-match by only considering the path-prefix sequence
// from the start (no fail-link follows once we leave the root). That
// degrades to a plain trie walk in this benchmark — included anyway so
// the AC build/match code is on record for the design discussion.

namespace aho_corasick {

struct State {
    static constexpr u32 kMaxTrans = 16;
    FixedVec<u8, kMaxTrans> bytes;
    FixedVec<u16, kMaxTrans> targets;
    static constexpr u16 kInvalid = 0xffffu;
    u16 route_idx_by_method[kMethodSlots] = {
        kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
    u32 depth = 0;  // bytes from root, used for longest-match-wins
};

class Automaton {
public:
    static constexpr u32 kMaxStates = 4096;

    Automaton() { clear(); }
    void clear() {
        states_len = 0;
        State root{};
        states[states_len++] = root;
    }

    bool insert(Str path, u8 method, u16 route_idx) {
        const u32 s = method_slot(method);
        if (s == kMethodSlotInvalid) return false;
        u32 lo = 0, hi = path.len;
        if (lo < hi && path.ptr[lo] == '/') lo++;
        while (hi > lo && path.ptr[hi - 1] == '/') hi--;
        u16 cur = 0;
        for (u32 i = lo; i < hi; i++) {
            const u8 b = static_cast<u8>(path.ptr[i]);
            u16 next = find_trans(cur, b);
            if (next == State::kInvalid) {
                if (states_len >= kMaxStates) return false;
                State ns;
                ns.depth = states[cur].depth + 1;
                states[states_len] = ns;
                next = static_cast<u16>(states_len++);
                if (!states[cur].bytes.push(b)) return false;
                if (!states[cur].targets.push(next)) return false;
            }
            cur = next;
        }
        if (states[cur].route_idx_by_method[s] == State::kInvalid)
            states[cur].route_idx_by_method[s] = route_idx;
        return true;
    }

    u16 match(Str path, u8 method) const {
        const u32 s = method_slot(method);
        if (s == kMethodSlotInvalid) return State::kInvalid;
        u32 lo = 0, hi = path.len;
        if (lo < hi && path.ptr[lo] == '/') lo++;
        while (hi > lo && path.ptr[hi - 1] == '/') hi--;
        u16 cur = 0;
        u16 best = pick(states[0], s);
        for (u32 i = lo; i < hi; i++) {
            const u8 b = static_cast<u8>(path.ptr[i]);
            const u16 next = find_trans(cur, b);
            if (next == State::kInvalid) break;
            cur = next;
            const u16 c = pick(states[cur], s);
            if (c != State::kInvalid) best = c;
        }
        return best;
    }

private:
    State states[kMaxStates];
    u32 states_len = 0;

    static u16 pick(const State& st, u32 slot) {
        if (slot != 0 && st.route_idx_by_method[slot] != State::kInvalid)
            return st.route_idx_by_method[slot];
        return st.route_idx_by_method[0];
    }
    u16 find_trans(u16 from, u8 b) const {
        const auto& st = states[from];
        for (u32 i = 0; i < st.bytes.len; i++)
            if (st.bytes[i] == b) return st.targets[i];
        return State::kInvalid;
    }
};

}  // namespace aho_corasick

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
    hash_full::Table hash_full_t;
    hash_seg::Table hash_seg_t;
    perfect_hash::Table perfect_hash_t;
    byte_radix::Trie byte_radix_t;
    simd_linear::Table simd_t;
    patricia::Trie patricia_t;
    aho_corasick::Automaton ac_t;
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
        (void)hash_full_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        (void)hash_seg_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        (void)perfect_hash_t.insert_pending(path, g_routes[i].method, static_cast<u16>(i));
        (void)byte_radix_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        (void)simd_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        (void)patricia_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        (void)ac_t.insert(path, g_routes[i].method, static_cast<u16>(i));
        g_linear[i] = {
            g_routes[i].path, g_routes[i].path_len, g_routes[i].method, static_cast<u16>(i)};
    }
    const bool ph_ok = perfect_hash_t.finalize();
    if (!ph_ok) {
        bench::out(
            "  [INFO] perfect_hash failed to find seed in 64K tries — skipping that variant\n");
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
    // Enable hardware perf counters so the output reports cycles/iter,
    // IPC, and cache-miss% alongside wall time. The four approaches
    // run on identical inputs, so the perf numbers explain WHY one
    // wins or loses — not just that it does. No-op on hosts where
    // perf_event_paranoid > 2 or the PMU is unavailable.
    b.perf_counters(true);
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
    b.run("hash_full_path", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = hash_full_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("hash_first_segment", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = hash_seg_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    if (ph_ok) {
        b.run("perfect_hash", [&] {
            if (cold_cache) trash_cache();
            for (u32 i = 0; i < kNumRequests; i++) {
                auto r = perfect_hash_t.match(g_req_strs[i], g_req_methods[i]);
                bench::do_not_optimize(&r);
            }
        });
    }
    b.run("byte_radix (compressed)", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = byte_radix_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("simd_linear (AVX2)", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = simd_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("patricia (crit-bit)", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = patricia_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.run("aho_corasick", [&] {
        if (cold_cache) trash_cache();
        for (u32 i = 0; i < kNumRequests; i++) {
            auto r = ac_t.match(g_req_strs[i], g_req_methods[i]);
            bench::do_not_optimize(&r);
        }
    });
    b.compare();
    bench::out("\n");
}

int main() {
    bench::print_environment_warnings();
    const u32 sizes[] = {32, 64, 128};
    for (u32 si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        bench_scale(sizes[si], /*cold_cache=*/false);
    }
    for (u32 si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        bench_scale(sizes[si], /*cold_cache=*/true);
    }
    return 0;
}
