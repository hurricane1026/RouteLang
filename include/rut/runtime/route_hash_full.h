#pragma once

// HashFullPath — exact-match hash table on the entire route path.
//
// Defines the floor for "what's the fastest possible per-match dispatch
// if you give up prefix semantics?" — at 128 routes on a realistic
// SaaS gateway corpus it's ~1.93× over the linear scan baseline (see
// the bench in feat/radix-trie-router for the data). Constant-time
// regardless of distribution: one FNV-1a + open-addressing probe.
//
// Limitations the selector must respect:
//   - NO PREFIX MATCHING. A route registered at "/api" doesn't match a
//     request like "/api/v1/users" — only an exact-string request like
//     "/api" hits. Configs that need longest-prefix semantics (anything
//     with `:param` segments, anything where one route is a prefix of
//     another) MUST stay on SegmentTrie.
//   - Method dispatch: 0 in a route matches any request method. A
//     specific method ('G', 'P', 'D', ...) only matches the same byte.
//     Two routes at the same path with different methods both fit, via
//     keying on (path, method). When a request method doesn't match the
//     specific slot we fall back to the (path, any) slot.
//   - kCap is fixed at 512 (= 4× kMaxRoutes for headroom against
//     long-running open-address probe chains; load factor stays < 25%
//     at the cap).
//
// Build cost: ~one FNV-1a per route on insert. Build is amortized
// across the lifetime of a CompiledConfig (RCU swap reuses the table)
// so insert speed is not on the hot path.

#include "rut/common/types.h"
#include "rut/runtime/route_dispatch.h"

namespace rut {

class HashFullPathTable {
public:
    static constexpr u32 kCap = 512;

    HashFullPathTable() { clear(); }

    void clear() {
        for (u32 i = 0; i < kCap; i++) {
            slots[i].path = Str{};
            slots[i].method = 0;
            slots[i].route_idx = kRouteIdxInvalid;
        }
        n_ = 0;
    }

    // Insert a (path, method, route_idx) triple. Returns false if the
    // table is too full (load factor would exceed 50%) — callers in
    // RouteConfig::add_* treat this as a hard route-rejection so the
    // hash dispatch stays consistent with the linear-scan default.
    //
    // Duplicate (path, method) keys: first insert wins; subsequent
    // calls return true without overwriting. Same shape as the trie's
    // method-slot first-wins behavior.
    //
    // path must outlive this table — the slot stores a non-owning
    // view. Callers in RouteConfig pass &routes[i].path which lives
    // for the config's RCU lifetime.
    bool insert(Str path, u8 method, u16 route_idx);

    // Look up `path` + `method`. Returns kRouteIdxInvalid on miss.
    // When the specific-method slot is empty but a same-path "any"
    // (method 0) slot exists, returns the any slot — matching the
    // first-match-wins fallback the linear scan provides.
    u16 match(Str path, u8 method) const;

    // Introspection for tests.
    u32 size() const { return n_; }

private:
    struct Slot {
        Str path;
        u8 method;
        u16 route_idx;
    };
    Slot slots[kCap];
    u32 n_;

    // FNV-1a + method byte folded in. method=0 ("any") and a specific
    // method byte hash to different buckets, so two routes at the same
    // path with different methods coexist without colliding on the
    // hash key — just on the slot table when probing converges.
    static u64 key_hash(Str path, u8 method);
    static u32 bucket(u64 h) { return static_cast<u32>(h) & (kCap - 1); }

    // Linear probe through `slots` from `start`, calling `pred` on
    // each occupied slot. Returns the first index where `pred`
    // returns true, or kCap on a sentinel-empty slot (`pred` not
    // called for empty slots), or kCap when the whole table has
    // been probed without a match (defensive — load factor cap
    // makes this unreachable in practice).
    template <typename Pred>
    u32 probe(u32 start, Pred pred) const {
        u32 i = start;
        for (u32 step = 0; step < kCap; step++) {
            if (slots[i].route_idx == kRouteIdxInvalid) return kCap;
            if (pred(slots[i])) return i;
            i = (i + 1) & (kCap - 1);
        }
        return kCap;
    }
};

// kHashFullPathDispatch is declared in route_dispatch.h alongside the
// other dispatch entries; see that header for the full vtable list.

}  // namespace rut
