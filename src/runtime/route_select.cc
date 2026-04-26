// 2-way decision helpers for Phase 2 dispatch — see route_select.h
// for the public contract. The earlier multi-dispatch RouteAnalysis +
// pick_dispatch tree was retired in PR #50 round 5.

#include "rut/runtime/route_select.h"

namespace rut {

namespace {

// True iff `a` is a strict byte-prefix of `b` (a.len < b.len, and
// the first a.len bytes are equal).
bool is_strict_byte_prefix(Str a, Str b) {
    if (a.len >= b.len) return false;
    for (u32 i = 0; i < a.len; i++) {
        if (a.ptr[i] != b.ptr[i]) return false;
    }
    return true;
}

// Given that `a` is a strict byte-prefix of `b`, returns true iff
// the next byte in `b` (immediately past the shared prefix) is not
// '/'. That's the "segment-boundary-sensitive overlap" case —
// /api vs /apix, where byte-prefix and segment-prefix matching
// would diverge for in-between requests like /apij.
//
// Exception: the root "/" catchall (shorter.len == 1, shorter == "/")
// is itself a complete path segment, so any byte that follows it in a
// longer sibling path is always a new segment start — there is no
// mid-segment overlap possible, and no ambiguity with any sibling
// route. Exempting "/" avoids spuriously forcing SegmentTrie whenever
// a root catchall is registered alongside other routes.
bool boundary_sensitive_after_prefix(Str shorter, Str longer) {
    if (shorter.len == 1 && shorter.ptr[0] == '/') return false;
    return longer.ptr[shorter.len] != '/';
}

}  // namespace

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

bool has_boundary_sensitive_overlap(const Str* paths, u32 n) {
    for (u32 i = 0; i < n; i++) {
        for (u32 j = i + 1; j < n; j++) {
            const Str& a = paths[i];
            const Str& b = paths[j];
            Str shorter;
            Str longer;
            if (is_strict_byte_prefix(a, b)) {
                shorter = a;
                longer = b;
            } else if (is_strict_byte_prefix(b, a)) {
                shorter = b;
                longer = a;
            } else {
                continue;
            }
            if (boundary_sensitive_after_prefix(shorter, longer)) return true;
        }
    }
    return false;
}

bool needs_segment_aware(const Str* paths, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (path_has_param_segment(paths[i])) return true;
    }
    return has_boundary_sensitive_overlap(paths, n);
}

}  // namespace rut
