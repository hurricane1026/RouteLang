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
    while (end < p.len && p.ptr[end] != '/') end++;
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
    const Str first = first_segment(path);
    const u32 primary_bucket = bucket_for(first);
    const u16 primary_hit = scan_bucket(primary_bucket, path, method);
    if (primary_hit != kRouteIdxInvalid) return primary_hit;
    // Catchall fallback — routes registered at "/" hash into the
    // empty-segment bucket. If the request's own first-segment
    // bucket missed, check the catchall bucket. Skip when the
    // request itself has an empty first segment (we'd be probing
    // the same bucket twice).
    if (first.len == 0) return kRouteIdxInvalid;
    const u32 catchall_bucket = bucket_for(Str{nullptr, 0});
    if (catchall_bucket == primary_bucket) return kRouteIdxInvalid;
    return scan_bucket(catchall_bucket, path, method);
}

namespace {

u16 hash_first_seg_match(const RouteConfig* cfg, Str path, u8 method) {
    return cfg->hash_first_seg_state.match(path, method);
}

}  // namespace

const RouteDispatch kHashFirstSegmentDispatch = {&hash_first_seg_match};

}  // namespace rut
