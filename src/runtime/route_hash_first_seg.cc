#include "rut/runtime/route_hash_first_seg.h"

#include "rut/runtime/route_table.h"

namespace rut {

namespace {

constexpr u64 kFnvBasis = 0xcbf29ce484222325ULL;
constexpr u64 kFnvPrime = 0x100000001b3ULL;

u64 fnv1a(const char* p, u32 len) {
    u64 h = kFnvBasis;
    for (u32 i = 0; i < len; i++) {
        h ^= static_cast<u8>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

}  // namespace

Str HashFirstSegmentTable::first_segment(Str p) {
    u32 start = (p.len > 0 && p.ptr[0] == '/') ? 1 : 0;
    u32 end = start;
    // Stop at '/' AND at '?' / '#' — request-targets from the parser
    // arrive raw, so /health?check=1 must produce first_segment "health"
    // not "health?check=1". Without this, request and route would
    // bucket differently and a registered /health route would miss.
    // Codex P2 on #45.
    while (end < p.len && p.ptr[end] != '/' && p.ptr[end] != '?' && p.ptr[end] != '#') end++;
    return Str{p.ptr + start, end - start};
}

u32 HashFirstSegmentTable::bucket_for(Str seg) {
    return static_cast<u32>(fnv1a(seg.ptr, seg.len)) & (kBuckets - 1);
}

bool HashFirstSegmentTable::insert(Str path, u8 method, u16 route_idx) {
    if (n_ >= kCap) return false;
    const u32 b = bucket_for(first_segment(path));
    if (bucket_lens[b] >= kPerBucket) return false;
    auto& e = entries[b][bucket_lens[b]++];
    e.path = path;
    e.method = method;
    e.route_idx = route_idx;
    n_++;
    return true;
}

u16 HashFirstSegmentTable::scan_bucket(u32 b, Str path, u8 method) const {
    const u32 n = bucket_lens[b];
    for (u32 i = 0; i < n; i++) {
        const auto& e = entries[b][i];
        if (e.method != 0 && e.method != method) continue;
        if (path.len < e.path.len) continue;
        bool ok = true;
        for (u32 j = 0; j < e.path.len; j++) {
            if (path.ptr[j] != e.path.ptr[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return e.route_idx;
    }
    return kRouteIdxInvalid;
}

u16 HashFirstSegmentTable::match(Str path, u8 method) const {
    // First-match-wins across the GLOBAL insert order (Codex P1 on
    // #45). Each bucket scans in local insert order, so its first
    // match is the smallest route_idx that bucket holds. To preserve
    // linear-scan precedence — where a "/" catchall registered before
    // a specific route MUST shadow the specific one — we look at both
    // candidate buckets (request's first-segment bucket + the empty-
    // segment bucket where catchalls live) and return the smaller
    // route_idx of the two.
    //
    // Cost: at most two bucket scans per match. The catchall bucket
    // typically holds 0-1 entries (only "/" routes hash there) so
    // it's nearly free; we early-exit for the cases where there's
    // nothing meaningful to merge.
    const Str first = first_segment(path);
    const u32 primary_bucket = bucket_for(first);
    const u16 primary_hit = scan_bucket(primary_bucket, path, method);
    // If the request itself has empty first segment, primary_bucket
    // already IS the catchall bucket — we'd just rescan it.
    if (first.len == 0) return primary_hit;
    const u32 catchall_bucket = bucket_for(Str{nullptr, 0});
    if (catchall_bucket == primary_bucket) return primary_hit;
    const u16 catchall_hit = scan_bucket(catchall_bucket, path, method);
    if (primary_hit == kRouteIdxInvalid) return catchall_hit;
    if (catchall_hit == kRouteIdxInvalid) return primary_hit;
    return primary_hit < catchall_hit ? primary_hit : catchall_hit;
}

namespace {

u16 hash_first_seg_match(const RouteConfig* cfg, Str path, u8 method) {
    return cfg->hash_first_seg_state.match(path, method);
}

}  // namespace

const RouteDispatch kHashFirstSegmentDispatch = {&hash_first_seg_match};

}  // namespace rut
