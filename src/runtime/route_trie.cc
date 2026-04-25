#include "rut/runtime/route_trie.h"

namespace rut {

// ---------------------------------------------------------------------------
// Path tokenization — encodes the normalization policies
// ---------------------------------------------------------------------------
//   P1a: drop empty segments (consecutive '/' collapse; leading '/' yields
//        no segment)
//   P2a: trailing '/' is equivalent to no trailing '/' — falls out of P1a
//        once the trailing empty run is skipped
//   P3a: case-sensitive — bytes are preserved verbatim, no tolower
//
// Returns the segment count, or kMaxPathSegments + 1 as a sentinel when the
// path would produce more segments than `out` can hold. The two callers
// treat the sentinel differently:
//   - insert() rejects sentinel results at build time (registering a
//     truncated path would land the route at the wrong depth relative
//     to what the user wrote).
//   - match() ignores the sentinel and routes using whatever segment
//     prefix `out` does hold, so a request that exceeds the cap still
//     hits the deepest applicable terminal — preserving catchall and
//     longest-prefix semantics rather than failing-closed on input
//     length alone (Codex P1 caught the earlier fail-closed match()).

u32 RouteTrie::tokenize_segments(Str path, FixedVec<Str, kMaxPathSegments>& out) {
    // Pure segment split — query/fragment stripping is the caller's
    // job. Routes arrive here via RouteConfig::add_* which already
    // rejected inputs containing '?' / '#', so insert() sees a clean
    // path. Incoming requests go through match(), which shortens the
    // Str above any '?' / '#' before calling tokenize. Keeping
    // tokenize pure means the insert-vs-match round-trip always
    // agrees on what a segment is: bytes between slashes.
    u32 start = 0;
    for (u32 i = 0; i <= path.len; i++) {
        const bool at_sep = (i == path.len) || (path.ptr[i] == '/');
        if (!at_sep) continue;
        if (i > start) {
            if (!out.push(Str{path.ptr + start, i - start})) return kMaxPathSegments + 1;
        }
        start = i + 1;
    }
    return out.len;
}

// ---------------------------------------------------------------------------
// Trie storage and lookup helpers
// ---------------------------------------------------------------------------

void RouteTrie::clear() {
    nodes.len = 0;
    TrieNode root{};
    [[maybe_unused]] bool ok = nodes.push(root);
    // push cannot fail on a fresh FixedVec; nodes starts empty.
}

u16 RouteTrie::find_child(u16 parent, Str segment) const {
    if (segment.len == 0) return TrieNode::kInvalidNodeIdx;
    const auto& p = nodes[parent];
    // Linear scan with full segment compare. Str::eq short-circuits on
    // length mismatch (usually the common case for heterogeneous
    // siblings) and then on first-byte mismatch if lengths coincide,
    // giving the same "first-byte fast path" as a separate u8 index —
    // but without the extra array and extra branch. Bench data confirms
    // this is faster than the httprouter-style parallel index for our
    // segment-length distribution.
    for (u32 i = 0; i < p.children.len; i++) {
        const u16 child_idx = p.children[i];
        if (nodes[child_idx].segment.eq(segment)) return child_idx;
    }
    return TrieNode::kInvalidNodeIdx;
}

bool RouteTrie::insert(Str path, u8 method_char, u16 route_idx) {
    // Reject unsupported method bytes up-front. An earlier revision
    // fell back to slot 0 ("any") for unknown chars, which would
    // silently broaden a route's method filter; fail fast instead so
    // callers notice (Codex P2 on #41).
    const u32 slot = method_slot(method_char);
    if (slot == kMethodSlotInvalid) return false;

    FixedVec<Str, kMaxPathSegments> segs{};
    const u32 n = tokenize_segments(path, segs);
    // Sentinel: a path with more segments than we can hold is rejected
    // at insert time. Silently truncating would create a route
    // registered at the wrong depth relative to what the user wrote.
    if (n > kMaxPathSegments) return false;

    // Snapshot state before any mutation so a mid-insert failure can
    // fully undo everything we've done so far. Per-iteration
    // pre-flights aren't sufficient on their own: a deep route that
    // creates k-1 nodes successfully and then fails at segment k was
    // still leaving k-1 ghost nodes (and the children-array pushes
    // that pointed at them) in place, consuming capacity until the
    // pool filled up and legitimate later routes got rejected. Codex
    // P1 on #41.
    const u32 saved_nodes_len = nodes.len;
    // Parents whose children list grew during this insert, one entry
    // per appended child. Rollback pops each parent's children list
    // in reverse order so they return to their pre-insert length.
    FixedVec<u16, kMaxPathSegments> pushed_parents{};

    auto rollback = [&]() {
        for (u32 r = pushed_parents.len; r > 0; r--) {
            nodes[pushed_parents[r - 1]].children.len--;
        }
        nodes.len = saved_nodes_len;
    };

    u16 cur = 0;  // root
    for (u32 i = 0; i < n; i++) {
        u16 child = find_child(cur, segs[i]);
        if (child == TrieNode::kInvalidNodeIdx) {
            // Capacity pre-flight — no mutation if either cap would
            // be exceeded. This avoids the usual "push succeeded,
            // dangling node left behind" leak on a same-iteration
            // failure.
            if (nodes.len >= kMaxNodes || nodes[cur].children.full()) {
                rollback();
                return false;
            }
            TrieNode nn{};
            nn.segment = segs[i];
            if (!nodes.push(nn)) {
                rollback();
                return false;
            }
            child = static_cast<u16>(nodes.len - 1);
            if (!nodes[cur].children.push(child)) {
                // Pre-flight above rules this out, but if a future
                // FixedVec invariant change makes it reachable, fall
                // into the full rollback so the pool stays clean.
                rollback();
                return false;
            }
            if (!pushed_parents.push(cur)) {
                // Unreachable: pushed_parents has the same cap as
                // segs (kMaxPathSegments) and we push at most one
                // entry per iteration. Roll back defensively anyway.
                rollback();
                return false;
            }
        }
        cur = child;
    }
    // Record at the terminal. First-insert-wins on the same (path, method)
    // pair — preserves the existing add-order semantics for duplicates.
    if (nodes[cur].route_idx_by_method[slot] == TrieNode::kInvalidRoute) {
        nodes[cur].route_idx_by_method[slot] = route_idx;
    }
    return true;
}

u16 RouteTrie::match(Str path, u8 method_char) const {
    // Unsupported method bytes can't match anything — bail before
    // touching the trie. Consistent with the insert-time rejection.
    const u32 want_slot = method_slot(method_char);
    if (want_slot == kMethodSlotInvalid) return TrieNode::kInvalidRoute;

    // Shorten the request path above any '?' (query) or '#' (fragment)
    // byte so routing uses only the path component (RFC 3986). We do
    // this at the call site rather than inside tokenize_segments so
    // insert() stays strict about the bytes it's tokenizing — a route
    // registered as "/health" never tokenizes to the same key as one
    // accidentally registered as "/health?x=1".
    u32 end = path.len;
    for (u32 i = 0; i < path.len; i++) {
        if (path.ptr[i] == '?' || path.ptr[i] == '#') {
            end = i;
            break;
        }
    }
    path.len = end;

    // Require origin-form request target (begins with '/') before
    // applying any route. Non-origin-form targets — OPTIONS `*`
    // (asterisk-form), CONNECT `host:port` (authority-form), and
    // absolute-form URLs — shouldn't route through path-based
    // matching at all; the pre-trie byte-prefix matcher rejected
    // them implicitly because pattern "/" failed to match their
    // first byte. The trie's root-terminal seed was bypassing that
    // and sending '*' / 'example:443' into a configured `/` catchall
    // (Codex P2 on #41).
    if (path.len == 0 || path.ptr[0] != '/') return TrieNode::kInvalidRoute;

    FixedVec<Str, kMaxPathSegments> segs{};
    // Ignore tokenize's return value on overflow: `segs` still holds
    // the first kMaxPathSegments, and we want to walk the trie as deep
    // as we have data for. Bailing out on overflow would let a request
    // that's deeper than cap bypass a '/' catchall or a matching
    // prefix route — insert() already rejects too-deep route configs,
    // so the trie never contains a terminal we'd miss.
    (void)tokenize_segments(path, segs);
    u16 cur = 0;
    // Track the deepest terminal we've seen that's compatible with the
    // requested method. Initialize from the root so a route inserted at
    // "/" acts as a catch-all even when the request has deeper segments
    // not in the trie.
    auto pick_terminal = [](const TrieNode& node, u32 slot) -> u16 {
        // Prefer a method-specific slot; fall back to slot 0 ("any").
        if (slot != 0 && node.route_idx_by_method[slot] != TrieNode::kInvalidRoute) {
            return node.route_idx_by_method[slot];
        }
        return node.route_idx_by_method[0];
    };
    u16 best = pick_terminal(nodes[0], want_slot);

    for (u32 i = 0; i < segs.len; i++) {
        const u16 child = find_child(cur, segs[i]);
        if (child == TrieNode::kInvalidNodeIdx) break;
        cur = child;
        const u16 candidate = pick_terminal(nodes[cur], want_slot);
        if (candidate != TrieNode::kInvalidRoute) best = candidate;
    }
    return best;
}

}  // namespace rut
