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
    // Look up `path` (the request-target path, NOT including any '?'
    // query or '#' fragment — the caller is expected to have stripped
    // those if the impl doesn't strip internally; see linear_scan_match
    // for the byte-prefix semantics this impl uses).
    //
    // `method` is the first byte of the HTTP method (G/P/D/H/O/C/T) or
    // 0 for "any". 0 in a route entry matches any request method.
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

}  // namespace rut
