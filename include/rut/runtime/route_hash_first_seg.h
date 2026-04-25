#pragma once

// HashFirstSegment — partition routes by first-segment hash, linear scan
// within bucket. Preserves the linear-scan default's byte-prefix
// matching semantics (so configs with prefix routes can use it), just
// with O(N/B) average-case lookup instead of O(N).
//
// Sweet spot:
//   - Configs with DIVERSE first segments (multi-service gateways:
//     /service-a, /service-b, /admin, /webhooks, /oauth, ...) — each
//     bucket holds 1-3 routes, lookup is dominated by the first-segment
//     hash + a tiny linear scan.
//   - Configs with prefix routes (where any path is a prefix of another)
//     and no `:param` segments — those need the byte-prefix walk that
//     this impl preserves; HashFullPath would be wrong, SegmentTrie is
//     overkill.
//
// Anti-sweet-spot (selector must avoid):
//   - Configs where >kPerBucket routes share the same first segment
//     (e.g., a monolithic SaaS API with 80+ routes all under /api/v1/).
//     Insert returns false past kPerBucket and the dispatch becomes
//     incomplete; the selector is responsible for measuring the
//     max-bucket-size from ParsedRoutes and rejecting this dispatch
//     when it would exceed kPerBucket.
//   - Configs with `:param` segments — the byte-prefix walk inside the
//     bucket can't capture params; need SegmentTrie.
//
// Storage: kBuckets × kPerBucket × sizeof(Entry) = 64 × 16 × 24B ≈ 24 KB.
//
// Catchall handling: a route at "/" tokenizes to first_segment="", which
// hashes to the same bucket every time (FNV(empty)). On a request whose
// own first-segment bucket misses, match() falls back to the empty-
// segment bucket — preserving the linear scan's "everything matches /"
// behavior.

#include "rut/common/types.h"
#include "rut/runtime/route_dispatch.h"

namespace rut {

class HashFirstSegmentTable {
public:
    static constexpr u32 kBuckets = 64;    // power of 2 for fast modulo
    static constexpr u32 kPerBucket = 16;  // selector's hard ceiling
    static constexpr u32 kCap = 256;       // total capacity (>= kMaxRoutes)

    HashFirstSegmentTable() { clear(); }

    void clear() {
        for (u32 b = 0; b < kBuckets; b++) bucket_lens[b] = 0;
        n_ = 0;
    }

    // Insert a (path, method, route_idx). Returns false on:
    //   - total capacity hit (n_ at kCap),
    //   - per-bucket capacity hit (the route's first-segment bucket
    //     already holds kPerBucket entries — the selector must never
    //     pick this dispatch for a config that triggers this; we
    //     return false defensively so a wrong selector choice is
    //     loud rather than silent).
    //
    // CONTRACT: route_idx must be assigned monotonically in insertion
    // order (route N gets idx N). RouteConfig::add_* guarantees this
    // via route_count++. match() relies on `smaller route_idx == older
    // insert` to preserve linear-scan's first-match-wins precedence
    // across the request's first-segment bucket and the catchall
    // bucket — without monotonicity, an older catchall would not
    // correctly shadow a newer specific route (Codex P1 on #45).
    bool insert(Str path, u8 method, u16 route_idx);

    // Look up `path` + `method`. Returns kRouteIdxInvalid on miss.
    // Sequence:
    //   1. hash first_segment(request_path) → bucket → linear scan
    //   2. if no hit and first_segment is non-empty, fall back to
    //      the empty-segment bucket (where catchalls at "/" sit)
    u16 match(Str path, u8 method) const;

    // Introspection.
    u32 size() const { return n_; }
    u32 bucket_size(u32 b) const { return b < kBuckets ? bucket_lens[b] : 0; }

private:
    struct Entry {
        Str path;
        u8 method;
        u16 route_idx;
    };
    Entry entries[kBuckets][kPerBucket];
    u32 bucket_lens[kBuckets];
    u32 n_;

    // First segment of `p`: skip leading '/', read until next '/' or
    // end. Returns an empty Str when `p` is "" or "/" or "/x" with
    // no second segment — those cases all hash to the catchall
    // bucket (first segment of the only routable byte sequence is
    // the empty prefix).
    static Str first_segment(Str p);

    static u32 bucket_for(Str seg);

    // Linear scan inside one bucket, returning the first matching
    // entry's route_idx or kRouteIdxInvalid if none match. Match
    // semantics: byte-prefix (path starts with entry.path) plus
    // method-slot (entry.method == 0 matches any).
    u16 scan_bucket(u32 b, Str path, u8 method) const;
};

}  // namespace rut
