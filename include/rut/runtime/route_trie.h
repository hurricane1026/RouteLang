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
//     determinism for duplicate keys; the trie is built incrementally via
//     add_* calls during RouteConfig construction, then treated as
//     read-only once the RouteConfig is published via RCU swap).
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
// method_slot() packs the handful of valid first-byte values into a
// dense [0..kMethodSlots) index for use as an array subscript. Slot 0
// = "any". Unsupported bytes (typos like 'g', future HTTP verbs, or
// garbage on the wire) return kMethodSlotInvalid so callers can
// reject them — an earlier revision mapped them to slot 0 ("any"),
// which silently widened a method-specific route into all-methods
// and/or conflated distinct method-specific routes. Codex flagged
// the silent coalescence (#41 P2); keep failure explicit.
static constexpr u32 kMethodSlots = 8;
static constexpr u32 kMethodSlotInvalid = 0xffffffffu;
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
            return kMethodSlotInvalid;
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
    // Sized to match RouteConfig::kMaxRoutes exactly. A config that
    // declares 128 routes all as distinct children of the same parent
    // (e.g. 128 top-level paths under root) must not be rejected on
    // topology alone — that would narrow the capacity contract the
    // pre-trie linear scan already honored (Codex P1 on #41). Most
    // inner nodes use very little of this (gateway routes rarely
    // have wide fan-out past root); the wasted-slots memory is
    // 128 × 2B − actual_children × 2B per node, tolerable at 512
    // nodes total.
    static constexpr u32 kMaxChildren = 128;

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
// Owns the node pool. Built incrementally via insert() calls (RouteConfig
// drives these from its add_* methods). Once the enclosing RouteConfig is
// published, treat the trie as read-only — RCU-friendly.

class RouteTrie {
public:
    // 128 routes × 4-segment distinct paths already need 513 nodes
    // (root + 128*4). Codex P1 on #41 flagged that 512 was too tight
    // to admit even that realistic shape. 2048 covers every
    // 128-route layout up to 16 segments each with no prefix
    // sharing — deeper worst cases (17+ segments, no sharing) are
    // still pathological and get rejected explicitly. Memory cost:
    // 2048 × ~290 B/node ≈ 600 KB per RouteConfig.
    static constexpr u32 kMaxNodes = 2048;
    // Sized so any legal request URI or registered route fits without
    // truncation. RouteEntry::kMaxPathLen is 128 bytes, which at a
    // minimum per-segment cost of 2 bytes ('/' + one content byte)
    // gives 64 segments worst case; ConnectionBase::kMaxReqPathLen is
    // 64 bytes (→ 32 segments). Pick the larger to cover route
    // admission. Codex flagged #41 P2 where a 16-cap rejected valid
    // 17-segment route configs.
    static constexpr u32 kMaxPathSegments = 64;

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

    // Split `path` into segments according to the normalization policy.
    //   - Strip any query string / fragment first ("?..." or "#..."),
    //     so only the path component participates in routing.
    //   - Drop empty segments ("/api//v1" → ["api", "v1"], "/" → []).
    //   - A trailing '/' produces an empty final segment that is then
    //     dropped — so "/api/" and "/api" both yield ["api"].
    //   - Match case-sensitively; preserve bytes verbatim (no tolower).
    //
    // Returns the segment count on success, or kMaxPathSegments + 1 as
    // a sentinel when the path would produce more segments than `out`
    // can hold. `insert()` rejects sentinel results so a build-time
    // config with too-deep paths fails cleanly; `match()` ignores the
    // sentinel and runs with the (truncated) segments so deep request
    // URIs still fall back to a catchall or prefix route.
    //
    // Does not allocate — `out` is caller-provided storage, emitted
    // Str views point into the path portion of `path.ptr`.
    static u32 tokenize_segments(Str path, FixedVec<Str, kMaxPathSegments>& out);

    // Linear-scan child lookup: walks `children` and compares the full
    // segment via Str::eq. Returns the child's node index, or
    // TrieNode::kInvalidRoute if no child matches. See the comment at
    // the top of this file for why we don't layer a u8 first-byte
    // index on top — at our segment-length distribution it's a net
    // cost, not a savings.
    u16 find_child(u16 parent, Str segment) const;
};

}  // namespace rut
