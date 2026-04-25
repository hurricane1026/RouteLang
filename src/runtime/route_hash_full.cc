#include "rut/runtime/route_hash_full.h"

#include "rut/runtime/route_table.h"

namespace rut {

namespace {

// FNV-1a 64-bit basis + prime. Picked over a faster mixer because at
// our key length (8-128 bytes) the FNV inner loop is already cheap and
// FNV's bit dispersion is well-studied for the open-addressing case.
constexpr u64 kFnvBasis = 0xcbf29ce484222325ULL;
constexpr u64 kFnvPrime = 0x100000001b3ULL;

}  // namespace

u64 HashFullPathTable::key_hash(Str path, u8 method) {
    u64 h = kFnvBasis;
    for (u32 i = 0; i < path.len; i++) {
        h ^= static_cast<u8>(path.ptr[i]);
        h *= kFnvPrime;
    }
    // Fold method into a separate "byte position" so (path, 'G') and
    // (path, 0) hash to different buckets even if FNV would land them
    // adjacently — keeps the any-vs-specific fallback search predictable.
    h ^= static_cast<u64>(method);
    h *= kFnvPrime;
    return h;
}

bool HashFullPathTable::insert(Str path, u8 method, u16 route_idx) {
    if (n_ * 2 >= kCap) return false;  // load factor cap < 50%
    const u64 h = key_hash(path, method);
    const u32 start = bucket(h);
    u32 i = start;
    for (u32 step = 0; step < kCap; step++) {
        if (slots[i].route_idx == kRouteIdxInvalid) {
            slots[i].path = path;
            slots[i].method = method;
            slots[i].route_idx = route_idx;
            n_++;
            return true;
        }
        if (slots[i].method == method && slots[i].path.eq(path)) {
            // First insert wins — duplicates are idempotent. This
            // matches the trie's method-slot first-wins so the same
            // input config produces the same observable routing
            // decisions across dispatches.
            return true;
        }
        i = (i + 1) & (kCap - 1);
    }
    return false;  // unreachable while load-factor cap holds
}

u16 HashFullPathTable::match(Str path, u8 method) const {
    if (path.len == 0) return kRouteIdxInvalid;
    // Strip query / fragment from the request before hashing — the
    // raw request-target as parsed includes "?q=1" / "#frag" bytes,
    // and a registered route at /health would otherwise miss when
    // the request arrives as /health?check=1. Other dispatches do
    // this too: SegmentTrie strips before tokenizing, linear scan
    // matches by byte prefix so the trailing query bytes are
    // ignored automatically. Codex P2 on #44.
    u32 effective_len = 0;
    while (effective_len < path.len && path.ptr[effective_len] != '?' &&
           path.ptr[effective_len] != '#') {
        effective_len++;
    }
    if (effective_len == 0) return kRouteIdxInvalid;
    const Str key{path.ptr, effective_len};
    // Try the specific-method bucket first. If the request asked for
    // method 0 (any), this is the only lookup we do — no fallback
    // needed because a method-0 route would have hashed into this
    // same key.
    const u64 h_specific = key_hash(key, method);
    const u32 found_specific = probe(
        bucket(h_specific), [&](const Slot& s) { return s.method == method && s.path.eq(key); });
    if (found_specific != kCap) return slots[found_specific].route_idx;
    if (method == 0) return kRouteIdxInvalid;
    // Specific-method miss: fall back to the (path, any) slot, matching
    // the linear scan's "method 0 in route matches any request"
    // behavior.
    const u64 h_any = key_hash(key, 0);
    const u32 found_any =
        probe(bucket(h_any), [&](const Slot& s) { return s.method == 0 && s.path.eq(key); });
    if (found_any != kCap) return slots[found_any].route_idx;
    return kRouteIdxInvalid;
}

namespace {

u16 hash_full_path_match(const RouteConfig* cfg, Str path, u8 method) {
    return cfg->hash_full_state.match(path, method);
}

}  // namespace

const RouteDispatch kHashFullPathDispatch = {&hash_full_path_match};

}  // namespace rut
