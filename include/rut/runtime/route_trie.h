#pragma once

#include "rut/common/types.h"

namespace rut {

// RouteTrie — segment-aware radix router for RouteConfig.
//
// Replaces the O(n) linear scan in RouteConfig::match() with a trie lookup
// that gives O(segments × fanout) match time. At 128 routes the trie is
// ~2.6× faster than the linear scan in hot-cache microbenchmarks (see
// bench/bench_route_trie.cc). Crossover is around 32 routes; below that,
// linear scan wins because tokenize() adds fixed per-lookup overhead that
// the flat byte-compare doesn't pay.
//
// Child lookup is a plain linear scan over the children array with full
// segment comparison. The benchmark also evaluated the common
// httprouter / matchit "parallel u8 first-byte index" optimization: it is
// within 1–3% of this simple layout at our segment-length distribution
// (3–6 byte segments, small fan-outs), which is inside run-to-run noise
// and well under the ~7–10% overhead of the translation-unit boundary
// production calls cross anyway. We take the simpler design — fewer
// bytes per node, less code to maintain — and leave the first-byte index
// as a re-introducible optimization if workloads change (longer segments,
// larger fan-outs, or SIMD-ready eq).
//
// Semantics: segment-aware prefix match with longest-match-wins.
//   - Path is split on '/' into segments.
//   - Empty segments are dropped (so "/api//v1" == "/api/v1", "/api/" == "/api").
//   - Matching is case-sensitive (per RFC 3986).
//   - A route attached at the root ("/") acts as a catch-all for any request.
//   - If multiple routes share a path, the first-inserted wins (build-order
//     determinism for duplicate keys; the trie is built once at finalize()).
//
// Method dispatch: each terminal node holds a small per-method slot table so
// two routes with the same path but different HTTP methods both fit. Lookup
// prefers a method-specific slot and falls back to the "any" slot. (This is
// a semantic refinement over the old linear scan's first-match-wins across
// method boundaries; see commit message for details.)

// ---------------------------------------------------------------------------
// Method slot encoding
// ---------------------------------------------------------------------------
// The runtime today uses the first byte of the HTTP method as its enum
// ('G' = GET, 'P' = POST/PUT/PATCH, 'D' = DELETE, ...). That first-byte
// scheme has a known ambiguity for POST/PUT/PATCH that we preserve here to
// keep this PR scoped — a proper method enum is a separate change.
//
// method_slot() packs the handful of valid first-byte values into a dense
// [0..kMethodSlots) index for use as an array subscript. Slot 0 = "any".
static constexpr u32 kMethodSlots = 8;
inline u32 method_slot(u8 method_char) {
    switch (method_char) {
        case 0:
            return 0;  // any
        case 'G':
            return 1;  // GET
        case 'P':
            return 2;  // POST/PUT/PATCH (ambiguous — matches current behavior)
        case 'D':
            return 3;  // DELETE
        case 'H':
            return 4;  // HEAD
        case 'O':
            return 5;  // OPTIONS
        case 'C':
            return 6;  // CONNECT
        case 'T':
            return 7;  // TRACE
        default:
            return 0;  // unknown → treat as any
    }
}

// ---------------------------------------------------------------------------
// TrieNode
// ---------------------------------------------------------------------------
// Nodes live in a flat `RouteTrie::nodes` pool and reference each other via
// u16 indices (not pointers) so the whole trie is a single contiguous region
// safe to atomically swap under RCU.
//
// The root node (index 0) has an empty `segment`; every other node's
// `segment` is the non-empty text of the path segment leading into it.

struct TrieNode {
    // 32 covers realistic gateway fan-outs: even a root that holds every
    // top-level API segment ("/api", "/auth", "/admin", health/metrics/…)
    // plus a handful of static paths fits cleanly. Per-node memory cost
    // is 32 × (1B first-byte + 2B child idx) + FixedVec overhead ≈ 100B.
    static constexpr u32 kMaxChildren = 32;

    // Edge label: the path segment that leads INTO this node. Non-owning,
    // points into the original RouteEntry::path buffer on RouteConfig.
    Str segment{};

    // Child node-pool indices. find_child scans these linearly with a full
    // segment compare — see the comment at the top of this file for why
    // we don't layer a separate first-byte index on top.
    FixedVec<u16, kMaxChildren> children;

    // Per-method route index at this terminal. kInvalidRoute means "this
    // node is not terminal for that method". Slot 0 is "any"; other slots
    // are per-method (see method_slot()).
    static constexpr u16 kInvalidRoute = 0xffffu;
    u16 route_idx_by_method[kMethodSlots] = {kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute,
                                             kInvalidRoute};
};

// ---------------------------------------------------------------------------
// RouteTrie
// ---------------------------------------------------------------------------
// Owns the node pool. Build once via a sequence of insert() calls, then
// lookup via match(). No mutation after finalize — RCU-friendly.

class RouteTrie {
public:
    static constexpr u32 kMaxNodes = 512;        // ~128 routes × ~4 segs with sharing
    static constexpr u32 kMaxPathSegments = 16;  // longest realistic URL depth

    RouteTrie() { clear(); }

    // Wipe and re-seed with the root node.
    void clear();

    // Insert a route. `path` must be a RouteEntry::path view (persistent
    // across the trie's lifetime — we store non-owning segment views into it).
    // `method_char` is the first byte of the HTTP method (or 0 for any).
    // `route_idx` is the position in RouteConfig::routes.
    //
    // Returns false if the trie is out of node-pool capacity or a node is
    // out of child-slots — callers should treat either as a build-time
    // "route table too complex" failure and refuse the config.
    bool insert(Str path, u8 method_char, u16 route_idx);

    // Look up `path` and return the route index of the longest-matching
    // terminal whose method slot is compatible with `method_char`. Returns
    // TrieNode::kInvalidRoute if nothing matches.
    u16 match(Str path, u8 method_char) const;

    // Introspection helpers (for tests / bench).
    u32 node_count() const { return nodes.len; }

private:
    FixedVec<TrieNode, kMaxNodes> nodes;

    // Split `path` into segments according to the normalization policy
    // documented at the top of this file (P1a + P2a + P3a):
    //   - P1a: drop empty segments ("/api//v1" → ["api", "v1"], "/" → [])
    //   - P2a: trailing '/' drops to an empty final segment, which is
    //          then dropped — so "/api/" and "/api" both yield ["api"]
    //   - P3a: case-sensitive, preserve bytes verbatim (no tolower)
    //
    // Returns the number of segments pushed into `out`. Returns
    // kMaxPathSegments + 1 (i.e. a sentinel larger than cap) if the path
    // would produce more segments than fit — callers should reject such
    // paths at build time and emit a clear error.
    //
    // TODO(user contribution): implement this. ~8-12 lines. The three
    // policy decisions (P1a/P2a/P3a) collapse cleanly into "scan bytes,
    // split on '/', emit non-empty runs as Str views into the input."
    // Do NOT allocate — `out` is caller-provided storage, Str views
    // point into `path.ptr`.
    static u32 tokenize_segments(Str path, FixedVec<Str, kMaxPathSegments>& out);

    // Internal helper — scans `child_first_bytes` for a first-byte match
    // and then verifies full segment equality. Returns child index or
    // kInvalidRoute.
    u16 find_child(u16 parent, Str segment) const;
};

}  // namespace rut
