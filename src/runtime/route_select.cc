#include "rut/runtime/route_select.h"

#include "rut/runtime/route_art.h"
#include "rut/runtime/route_byte_radix.h"
#include "rut/runtime/route_hash_first_seg.h"

namespace rut {

namespace {

constexpr u64 kFnvBasis = 0xcbf29ce484222325ULL;
constexpr u64 kFnvPrime = 0x100000001b3ULL;

// Threshold below which LinearScan's early-exit + zero-indirection
// dominates the indexed alternatives. Chosen at the bench data's
// crossover point on realistic SaaS configs (closed #41 bench): at
// N≤32 the trie/hash dispatchers are within 10% of linear or
// behind it, and the extra build-time cost + storage isn't paid back.
// We pick 16 to leave a margin — the bench is one machine's data
// point, and the linear-scan code path is always faster to reason
// about for tiny configs.
constexpr u32 kLinearScanCutoff = 16;

// Min distinct first segments to consider HashFirstSegment over
// HashFullPath. With ≤4 distinct first segments and N>16, most routes
// pile into a few buckets — the bucketing buys little over hashing
// the full path, while HashFullPath's per-match cost is identical and
// it sidesteps the per-bucket-cap selector check entirely.
constexpr u32 kMinDistinctFirstSegmentsForFirstSeg = 4;

// First-byte fan-out at which ART starts beating ByteRadix. Bench
// data (bench/bench_route_art.cc fan-out sweep) shows the crossover
// at exactly 17 — that's the threshold where ART's Node48 (byte-
// indexed O(1) lookup) replaces Node16 (16-key linear scan). Below
// 17, ByteRadix's homogeneous tight loop wins by 5-30%; from 17 up
// ART wins by 50-85%. The "low side" matters: routing typical SaaS
// to ART would cost ~25% latency in exchange for ~3× memory. Stick
// with ByteRadix below the threshold.
constexpr u32 kArtFanoutThreshold = 17;

// Detect `:param` style segments in a path. Format expected (when the
// frontend grows the syntax): a segment that begins with ':' marks
// that segment as a parameter capture. For now this scan never finds
// one in real traffic — RouteConfig::is_routable_path doesn't accept
// `:` differently from any other byte today, and the .rut DSL has no
// `:foo` syntax yet — but pinning the contract early means the
// picker's branch is in place when the syntax lands.
bool path_has_param_segment(Str path) {
    bool at_segment_start = true;
    for (u32 i = 0; i < path.len; i++) {
        const char c = path.ptr[i];
        if (c == '/') {
            at_segment_start = true;
            continue;
        }
        if (at_segment_start && c == ':') return true;
        at_segment_start = false;
    }
    return false;
}

// True iff `a` is a strict byte-prefix of `b` (a.len < b.len, and the
// first a.len bytes are equal).
bool is_strict_byte_prefix(Str a, Str b) {
    if (a.len >= b.len) return false;
    for (u32 i = 0; i < a.len; i++) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

// Given that `a` is a strict byte-prefix of `b`, returns true iff the
// next byte in `b` (the byte immediately past the shared prefix) is
// not '/'. That's the "segment-boundary-sensitive overlap" case —
// /api vs /apix, where byte-prefix and segment-prefix dispatch would
// give different answers for an intermediate request like /apij.
// Caller must establish the strict-prefix relationship; we don't
// reverify because the call sites already did.
bool boundary_sensitive_after_prefix(Str shorter, Str longer) {
    return longer.ptr[shorter.len] != '/';
}

// Largest route count that ByteRadix can absorb in the worst case
// without tripping its node-pool cap. Each insert can split an edge
// (1 new node) and add a leaf (1 more), so the strict upper bound is
// 1 + 2N nodes. With ByteRadixTrie::kMaxNodes = 256, that admits
// N ≤ 127. Realistic configs with prefix sharing fit far more, but
// the picker is the trusted source of dispatch correctness — we keep
// the conservative bound so a worst-case-shape config can never make
// it past pick_dispatch and into a partial-build failure during
// add_*. Codex P1 on #47 round 1 (the round-3 fan-out bump on #46
// fixed the 16-children failure mode but the total-node ceiling
// remains a separate constraint).
constexpr u32 kByteRadixSafeMaxRoutes = (ByteRadixTrie::kMaxNodes - 1) / 2;

}  // namespace

Str RouteAnalysis::first_segment(Str p) {
    u32 start = (p.len > 0 && p.ptr[0] == '/') ? 1 : 0;
    u32 end = start;
    while (end < p.len && p.ptr[end] != '/' && p.ptr[end] != '?' && p.ptr[end] != '#') end++;
    return Str{p.ptr + start, end - start};
}

u64 RouteAnalysis::fnv_first_seg(Str seg) {
    u64 h = kFnvBasis;
    for (u32 i = 0; i < seg.len; i++) {
        h ^= static_cast<u8>(seg.ptr[i]);
        h *= kFnvPrime;
    }
    return h;
}

u32 RouteAnalysis::first_seg_bucket(Str seg) {
    return static_cast<u32>(fnv_first_seg(seg)) & (kFirstSegBuckets - 1);
}

bool RouteAnalysis::note_route(Str path, u8 method) {
    // method is part of the (path, method) registration key callers
    // already track; keeping the parameter in the API surface lets a
    // future method-aware heuristic land without a signature churn.
    // No current selector signal is method-dependent, so we don't
    // store it. (Copilot on #47 round 1 flagged dead methods_ storage.)
    (void)method;
    if (n_ >= kMaxPaths) return false;

    // Update prefix-overlap flags against everything seen so far.
    // O(N) per insert; bounded at kMaxPaths so total work is O(N²)
    // in the worst case, ~16K compares for the cap. Cheap relative
    // to the route-build cost the caller's already paying.
    //
    // Two flags piggyback on the same scan:
    //   - has_prefix_overlap: any strict byte-prefix pair.
    //   - has_segment_boundary_sensitive_overlap: a strict-prefix pair
    //     whose continuation byte in the longer path is not '/'.
    //     Boundary-sensitive overlaps disqualify ByteRadix because
    //     its byte-prefix semantics would diverge from segment-aware
    //     routing for in-between requests (e.g. /api + /apix → /apij
    //     hits /api in byte mode, misses in segment mode).
    if (!has_prefix_overlap_ || !has_segment_boundary_sensitive_overlap_) {
        for (u32 i = 0; i < n_; i++) {
            const Str& other = paths_[i];
            Str shorter;
            Str longer;
            if (is_strict_byte_prefix(path, other)) {
                shorter = path;
                longer = other;
            } else if (is_strict_byte_prefix(other, path)) {
                shorter = other;
                longer = path;
            } else {
                continue;
            }
            has_prefix_overlap_ = true;
            if (!has_segment_boundary_sensitive_overlap_ &&
                boundary_sensitive_after_prefix(shorter, longer)) {
                has_segment_boundary_sensitive_overlap_ = true;
            }
            // Both flags set → no further information to gather.
            if (has_segment_boundary_sensitive_overlap_) break;
        }
    }

    if (!has_param_segments_ && path_has_param_segment(path)) has_param_segments_ = true;

    // Update first-segment bucket counts. Stays in sync with what
    // HashFirstSegment would compute at insert time.
    bucket_counts_[first_seg_bucket(first_segment(path))]++;

    // Update the first-byte set for distinct_first_bytes(). The
    // "first byte" is the byte immediately after a leading '/' if
    // present, otherwise the first byte of the path. Empty paths
    // contribute nothing.
    if (path.len > 0) {
        const u32 lo = (path.ptr[0] == '/') ? 1 : 0;
        if (lo < path.len) {
            const u8 b = static_cast<u8>(path.ptr[lo]);
            first_byte_seen_[b >> 6] |= (1ULL << (b & 63));
        }
    }

    paths_[n_] = path;
    n_++;
    return true;
}

u32 RouteAnalysis::max_first_seg_bucket() const {
    u32 max = 0;
    for (u32 i = 0; i < kFirstSegBuckets; i++) {
        if (bucket_counts_[i] > max) max = bucket_counts_[i];
    }
    return max;
}

u32 RouteAnalysis::distinct_first_bytes() const {
    return static_cast<u32>(
        __builtin_popcountll(first_byte_seen_[0]) + __builtin_popcountll(first_byte_seen_[1]) +
        __builtin_popcountll(first_byte_seen_[2]) + __builtin_popcountll(first_byte_seen_[3]));
}

u32 RouteAnalysis::distinct_first_segments() const {
    // O(N²) — for each path, check whether an earlier path has the
    // same first segment. At kMaxPaths=128 the worst-case is ~8K
    // compares, paid once at config-build time.
    u32 distinct = 0;
    for (u32 i = 0; i < n_; i++) {
        const Str s = first_segment(paths_[i]);
        bool seen = false;
        for (u32 j = 0; j < i; j++) {
            if (first_segment(paths_[j]).eq(s)) {
                seen = true;
                break;
            }
        }
        if (!seen) distinct++;
    }
    return distinct;
}

const RouteDispatch* pick_dispatch(const RouteAnalysis& a) {
    // Param segments require segment-bound parameter capture; only
    // SegmentTrie supports that.
    if (a.has_param_segments()) return &kSegmentTrieDispatch;

    // Tiny configs: linear scan beats the bookkeeping cost of any
    // indexed alternative.
    if (a.count() <= kLinearScanCutoff) return &kLinearScanDispatch;

    // Boundary-sensitive overlap (e.g. /api + /apix registered
    // together) disqualifies ByteRadix — its byte-prefix view would
    // mis-route intermediate requests. SegmentTrie is the only impl
    // that gives the segment-aware answer for these configs.
    // (Copilot on #47 round 1.)
    if (a.has_segment_boundary_sensitive_overlap()) return &kSegmentTrieDispatch;

    // Pure segment-aligned prefix overlap. Choose between ART and
    // ByteRadix by the byte-level fan-out at the trie root:
    //   - High fan-out (≥ kArtFanoutThreshold): ART's Node48/256
    //     byte-indexed lookup beats ByteRadix's linear scan over
    //     the children array. Bench shows +50-85% on dense top-
    //     level configs.
    //   - Low fan-out: ByteRadix's homogeneous tight loop wins —
    //     ART's polymorphic dispatch tax (one indirect branch per
    //     descent step) costs more than its node-type savings buy
    //     when most nodes are Node4. Bench: ByteRadix is ~5-30%
    //     faster on saas-like configs in this regime.
    //
    // count gate (kByteRadixSafeMaxRoutes): ByteRadix's node pool
    // (kMaxNodes = 256) has worst-case 1 + 2N capacity, so we route
    // configs above the safe bound to SegmentTrie rather than risk
    // an add_*-time build failure. ART has its own pool budget and
    // can absorb up to kMaxRoutes regardless of fan-out shape.
    if (a.has_prefix_overlap()) {
        if (a.distinct_first_bytes() >= kArtFanoutThreshold) return &kArtDispatch;
        if (a.count() <= kByteRadixSafeMaxRoutes) return &kByteRadixDispatch;
        return &kSegmentTrieDispatch;
    }

    // No prefix overlap, no params. HashFirstSegment is the right
    // pick when (a) the per-bucket cap holds and (b) first-segment
    // hashing actually partitions the route set
    // (≥ kMinDistinctFirstSegmentsForFirstSeg). Within a bucket
    // HashFirstSegment still does a byte-prefix scan, so requests
    // longer than the registered route (e.g., registered /api/users
    // matching request /api/users/42) work correctly.
    if (a.max_first_seg_bucket() <= HashFirstSegmentTable::kPerBucket &&
        a.distinct_first_segments() >= kMinDistinctFirstSegmentsForFirstSeg) {
        return &kHashFirstSegmentDispatch;
    }

    // Fall through to SegmentTrie, NOT HashFullPath. HashFullPath is
    // exact-match-only — it would silently turn a registered prefix
    // (/api/users) into a miss when the request arrives longer
    // (/api/users/42), breaking the linear-scan baseline contract
    // even though the picker thought it was a safe choice. SegmentTrie
    // is unconditionally correct (segment-prefix, longest-match wins)
    // and stays available for any shape we couldn't route to a faster
    // impl. Codex P1 on #47 round 1 caught the original
    // HashFullPath fallback.
    return &kSegmentTrieDispatch;
}

}  // namespace rut
