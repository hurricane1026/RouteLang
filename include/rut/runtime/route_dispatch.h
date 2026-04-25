#pragma once

// RouteDispatch — vtable-style interface for route lookup.
//
// Why an interface?
//   The hot path of every request is RouteConfig::match(). Different
//   route-table shapes have very different ideal data structures (see
//   bench/bench_route_trie.cc for the data behind that claim): a small
//   flat config wins on a linear scan with early-exit; a wide exact-
//   match config wins on a hash table; a deep prefix-shared config wins
//   on a radix trie. Picking one structure for all configs leaves
//   measurable performance on the table and forces every workload onto
//   the same trade-off.
//
//   This file is the seam: a tiny vtable (one function pointer) that
//   lets each impl live behind a uniform call. The compiler-side
//   selector (a follow-up PR) inspects the parsed route set and picks
//   the appropriate impl. The runtime path is one indirect call per
//   match — branch predictor handles it cleanly because each
//   CompiledConfig stays bound to a single dispatch for its lifetime.
//
// Why a function pointer instead of virtual dispatch?
//   The codebase is `-fno-rtti`/no-stdlib by convention (see CLAUDE.md);
//   virtual functions still work technically but sit oddly with the
//   "compile-time backend selection via templates" pattern used
//   elsewhere (e.g. EpollBackend / IoUringBackend). Function pointers
//   are the smaller, more inspectable mechanism — `perf` reports the
//   concrete impl's symbol name instead of an indirect-call target.
//
// State storage:
//   For the linear-scan dispatch (this PR), the state is just the
//   existing RouteConfig::routes[] array — no extra storage needed. The
//   `match()` function takes a `const RouteConfig*` so impls can reach
//   into RouteConfig fields directly. Future impls (HashFullPath,
//   ByteRadix, …) will declare their own state structs and arrange for
//   RouteConfig to hold them; the interface stays the same.

#include "rut/common/types.h"

namespace rut {

struct RouteConfig;

// Sentinel for "no match"; route indices are otherwise [0, kMaxRoutes).
constexpr u16 kRouteIdxInvalid = 0xffffu;

struct RouteDispatch {
    // Look up the request-target `path` and resolve to a route index.
    //
    // Path bytes: `path` is the raw request-target as parsed (the bytes
    // between method and HTTP/version). It MAY contain a '?' query
    // string and/or '#' fragment — each impl is responsible for any
    // stripping its matching policy needs. The default linear scan
    // matches by byte prefix and so naturally handles "/api?q=1" via
    // a route at "/api" (the route's bytes match the leading bytes of
    // the request, regardless of what follows). The segment trie
    // strips '?' / '#' explicitly before tokenizing. New impls should
    // declare and uphold whichever policy is right for their shape.
    //
    // `method`: first byte of the HTTP method (G/P/D/H/O/C/T) or 0
    // for "any". 0 in a route entry matches any request method.
    //
    // Returns the route index in RouteConfig::routes[], or
    // kRouteIdxInvalid on no match. Callers (RouteConfig::match) turn
    // that into a `const RouteEntry*` for downstream code.
    u16 (*match)(const RouteConfig* cfg, Str path, u8 method);
};

// Linear byte-prefix scan over RouteConfig::routes[]. First-match-wins,
// which means earlier-inserted routes shadow later ones at overlapping
// prefixes — a behavior callers have relied on since the pre-trie days.
extern const RouteDispatch kLinearScanDispatch;

// Segment-aware radix trie (RouteConfig::trie). Longest-prefix match,
// segment-boundary aware, normalizes consecutive '/' and trailing '/'.
// Strips '?' / '#' from incoming requests before tokenizing. Reads
// route_idx_by_method[] at every terminal so a method-specific route
// beats an "any" route at the same path.
extern const RouteDispatch kSegmentTrieDispatch;

// Exact-match hash table on the entire path
// (RouteConfig::hash_full_state). Constant-time per match, but ONLY
// admissible when no route is a prefix of another and no `:param`
// segments are present — see route_hash_full.h. The selector
// (follow-up PR) is responsible for never picking this dispatch when
// those constraints don't hold.
extern const RouteDispatch kHashFullPathDispatch;

// First-segment-hashed bucket table with byte-prefix scan inside the
// bucket (RouteConfig::hash_first_seg_state). Preserves linear-scan
// semantics so prefix routes work, just with O(N/B) average lookup.
// Selector picks this for configs with diverse first segments and
// no first-segment bucket exceeding kPerBucket — see
// route_hash_first_seg.h.
extern const RouteDispatch kHashFirstSegmentDispatch;

// Byte-level edge-compressed radix trie (RouteConfig::byte_radix_state).
// Longest-prefix-match in BYTES, not segments — so the selector picks
// this only for configs without segment-boundary semantics (no
// `:param`, no overlapping segment-distinguished routes). See
// route_byte_radix.h for the contract and the bench data behind the
// choice.
extern const RouteDispatch kByteRadixDispatch;

}  // namespace rut
