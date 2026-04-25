#pragma once

// route_select — choose the right RouteDispatch for a given route set.
//
// The interface seam in route_dispatch.h gives us a vtable; each impl
// (LinearScan, SegmentTrie, HashFullPath, HashFirstSegment, ByteRadix)
// has its own sweet spot. Letting every config pay the same dispatch
// leaves measurable performance on the table — bench data on the
// closed #41 branch showed up to ~7× variance across impls at N=128
// realistic, and the winner depends on the route set's shape.
//
// This file is the picker. Two pieces:
//
//   1. `RouteAnalysis` — a stateful builder that observes a config's
//      routes one at a time and accumulates the shape signals the
//      picker needs (count, prefix overlap, first-segment bucket
//      distribution, presence of `:param` segments). Accumulation is
//      the natural fit for callers like compile_to_config.h that
//      already iterate parsed routes once on the way to add_*.
//
//   2. `pick_dispatch(const RouteAnalysis&)` — looks at the
//      accumulated signals and returns one of the canonical
//      `RouteDispatch*` singletons declared in route_dispatch.h.
//
// Caller flow:
//   RouteAnalysis a;
//   for (each route) a.note_route(path, method);
//   RouteConfig cfg;
//   cfg.set_dispatch(pick_dispatch(a));
//   for (each route) cfg.add_*(path, method, ...);
//
// The two passes over the route list are intentional: the picker
// needs the full set before deciding, and add_* needs the dispatch
// chosen before the first call (RouteConfig refuses set_dispatch
// after route_count > 0). The redundant work is fixed-cost per
// config-build and has no impact on the runtime hot path.
//
// Heuristics implemented (each justified by bench data; see
// bench/bench_route_trie.cc on closed #41):
//
//   `:param` segments        → SegmentTrie (only impl that supports
//                              segment-bound parameter capture; once
//                              the .rut frontend grows :param syntax)
//   route count ≤ 16         → LinearScan (early-exit on hot prefixes
//                              + zero indirection wins at small N)
//   prefix-overlap routes    → ByteRadix (longest-prefix in bytes;
//                              hash variants give up prefix semantics)
//   diverse first segments
//     + bucket-fit           → HashFirstSegment
//   otherwise (exact-match)  → HashFullPath
//
// Out of scope here, deferred to follow-up PRs:
//   - `:param` detection (the .rut DSL doesn't yet emit them)
//   - Perfect-hash construction
//   - Run-time recalibration based on observed traffic shape
//
// Selector branches are covered 1:1 by tests in
// `tests/test_route_select.cc`.

#include "rut/common/types.h"
#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_hash_first_seg.h"  // for kBuckets / kPerBucket

namespace rut {

class RouteAnalysis {
public:
    // Cap mirrors RouteConfig::kMaxRoutes. Inputs past the cap return
    // false from note_route — the picker will still produce a
    // reasonable answer (it derives from accumulated state, not from
    // the truncated tail), but the count-based threshold can no
    // longer be relied on past kMaxPaths.
    static constexpr u32 kMaxPaths = 128;

    // Must mirror HashFirstSegmentTable's bucket count exactly so
    // the picker's first-segment-bucket-fit check matches what
    // hash_first_seg would actually do at insert time.
    static constexpr u32 kFirstSegBuckets = HashFirstSegmentTable::kBuckets;

    RouteAnalysis() {
        for (u32 i = 0; i < kFirstSegBuckets; i++) bucket_counts_[i] = 0;
    }

    // Record a (path, method) the caller plans to register. Returns
    // false if the analysis cap is exceeded — callers should still
    // proceed (analysis stays correct for the first kMaxPaths routes
    // and the picker's heuristics degrade gracefully).
    //
    // CONTRACT: `path` must outlive the RouteAnalysis instance — the
    // builder stores non-owning Str views for the prefix-overlap
    // check. Compile-to-config callers pass paths from RouteEntry-
    // shaped buffers that live for the build's duration; that's safe.
    bool note_route(Str path, u8 method);

    // Snapshots accessible to the picker. The accessors are deliberate
    // — pick_dispatch should never reach into the raw fields.

    u32 count() const { return n_; }

    // True iff any registered path is a strict prefix of another (in
    // bytes). Hash variants (HashFullPath / HashFirstSegment exact
    // mode) can't preserve linear-scan first-match-wins precedence
    // across overlapping prefixes, so this flag steers the picker
    // toward longest-prefix-capable impls (SegmentTrie, ByteRadix).
    bool has_prefix_overlap() const { return has_prefix_overlap_; }

    // True iff any path contains a `:param` segment marker. Currently
    // always false — the .rut frontend doesn't emit them yet — but
    // the picker hooks into it so the selector reads correctly when
    // that work lands.
    bool has_param_segments() const { return has_param_segments_; }

    // Largest count of routes sharing a first-segment hash bucket
    // (with kFirstSegBuckets buckets — same modulus hash_first_seg
    // would use). HashFirstSegment is admissible only when this
    // doesn't exceed its per-bucket cap.
    u32 max_first_seg_bucket() const;

    // Number of distinct first segments observed. Cheap proxy for
    // "is the first-segment hash actually doing partitioning work" —
    // when the count is tiny relative to the route count, hash_first_
    // seg's buckets aren't really distributing and the cost of the
    // hash isn't paid back.
    u32 distinct_first_segments() const;

private:
    Str paths_[kMaxPaths];
    u8 methods_[kMaxPaths];
    u32 n_ = 0;
    bool has_prefix_overlap_ = false;
    bool has_param_segments_ = false;
    u32 bucket_counts_[HashFirstSegmentTable::kBuckets];
    // distinct first segments are reconstructed on demand from paths_
    // — they're only consulted in pick_dispatch, not on every
    // note_route, so we don't pay an extra hash table here.

    static Str first_segment(Str p);
    static u64 fnv_first_seg(Str seg);
    static u32 first_seg_bucket(Str seg);
};

// Pick the canonical-singleton dispatch for the analyzed route set.
// Always returns one of the kFooDispatch globals declared in
// route_dispatch.h — never nullptr, never an unknown pointer (the
// canonical-singleton whitelist in RouteConfig::set_dispatch checks
// for this; the picker is the trusted source of those pointers).
const RouteDispatch* pick_dispatch(const RouteAnalysis& analysis);

}  // namespace rut
