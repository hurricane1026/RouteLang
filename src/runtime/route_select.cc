#include "rut/runtime/route_select.h"

#include "rut/runtime/route_byte_radix.h"
#include "rut/runtime/route_hash_first_seg.h"
#include "rut/runtime/route_hash_full.h"

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
    if (n_ >= kMaxPaths) return false;

    // Update prefix-overlap flag against everything seen so far.
    // O(N) per insert; bounded at kMaxPaths so total work is O(N²)
    // in the worst case, ~16K compares for the cap. Cheap relative
    // to the route-build cost the caller's already paying.
    if (!has_prefix_overlap_) {
        for (u32 i = 0; i < n_; i++) {
            if (is_strict_byte_prefix(path, paths_[i]) || is_strict_byte_prefix(paths_[i], path)) {
                has_prefix_overlap_ = true;
                break;
            }
        }
    }

    if (!has_param_segments_ && path_has_param_segment(path)) has_param_segments_ = true;

    // Update first-segment bucket counts. Stays in sync with what
    // HashFirstSegment would compute at insert time.
    bucket_counts_[first_seg_bucket(first_segment(path))]++;

    paths_[n_] = path;
    methods_[n_] = method;
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

    // Routes that overlap as byte prefixes need longest-prefix
    // matching; hash variants would lose precedence. ByteRadix beats
    // SegmentTrie on the bench whenever we can use it (no params),
    // so this branch picks ByteRadix for the prefix-overlap, no-param
    // case. SegmentTrie is reserved for the param-capture path
    // exclusively (above).
    if (a.has_prefix_overlap()) return &kByteRadixDispatch;

    // No prefix overlap, no params — exact-match hash variants are
    // admissible. Choose HashFirstSegment when (a) the per-bucket
    // cap holds and (b) first-segment hashing actually distributes
    // (≥ kMinDistinctFirstSegmentsForFirstSeg). Otherwise
    // HashFullPath, which is constant-time and unconditionally
    // applicable.
    if (a.max_first_seg_bucket() <= HashFirstSegmentTable::kPerBucket &&
        a.distinct_first_segments() >= kMinDistinctFirstSegmentsForFirstSeg) {
        return &kHashFirstSegmentDispatch;
    }
    return &kHashFullPathDispatch;
}

}  // namespace rut
