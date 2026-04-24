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
// path would produce more segments than `out` can hold. Callers reject
// sentinel results and emit a clean build-time error.

u32 RouteTrie::tokenize_segments(Str path, FixedVec<Str, kMaxPathSegments>& out) {
    // Trim the query string / fragment: routing runs on the path component
    // only (RFC 3986), and the HTTP parser hands us the raw request-target
    // which may include `?foo=bar` or `#anchor`. Stripping here means
    // `/health?check=1` matches a route registered as `/health`.
    u32 end = path.len;
    for (u32 i = 0; i < end; i++) {
        if (path.ptr[i] == '?' || path.ptr[i] == '#') {
            end = i;
            break;
        }
    }
    u32 start = 0;
    for (u32 i = 0; i <= end; i++) {
        const bool at_sep = (i == end) || (path.ptr[i] == '/');
        if (!at_sep) continue;
        if (i > start) {
            if (!out.push(Str{path.ptr + start, i - start})) return kMaxPathSegments + 1;
        }
        start = i + 1;
    }
    return out.len;
}

// ---------------------------------------------------------------------------
// Scaffolding (done — do not touch for the user contribution)
// ---------------------------------------------------------------------------

void RouteTrie::clear() {
    nodes.len = 0;
    TrieNode root{};
    [[maybe_unused]] bool ok = nodes.push(root);
    // push cannot fail on a fresh FixedVec; nodes starts empty.
}

u16 RouteTrie::find_child(u16 parent, Str segment) const {
    if (segment.len == 0) return TrieNode::kInvalidRoute;
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
    return TrieNode::kInvalidRoute;
}

bool RouteTrie::insert(Str path, u8 method_char, u16 route_idx) {
    FixedVec<Str, kMaxPathSegments> segs{};
    const u32 n = tokenize_segments(path, segs);
    // Sentinel: a path with more segments than we can hold is rejected
    // at insert time. Silently truncating would create a route
    // registered at the wrong depth relative to what the user wrote.
    if (n > kMaxPathSegments) return false;

    u16 cur = 0;  // root
    for (u32 i = 0; i < n; i++) {
        u16 child = find_child(cur, segs[i]);
        if (child == TrieNode::kInvalidRoute) {
            // Pre-flight both capacity checks before mutating. Without
            // this, a successful nodes.push() followed by a failing
            // children.push() would leak a dangling node whose segment
            // view points into the route path buffer — on repeated
            // failures the node pool fills with ghosts until legitimate
            // inserts start failing.
            if (nodes.len >= kMaxNodes) return false;
            if (nodes[cur].children.full()) return false;
            TrieNode nn{};
            nn.segment = segs[i];
            if (!nodes.push(nn)) return false;
            child = static_cast<u16>(nodes.len - 1);
            if (!nodes[cur].children.push(child)) {
                // Pre-flight above rules this out, but guard in case a
                // future FixedVec invariant changes — roll the node
                // back so the pool stays consistent.
                nodes.len--;
                return false;
            }
        }
        cur = child;
    }
    // Record at the terminal. First-insert-wins on the same (path, method)
    // pair — preserves the existing add-order semantics for duplicates.
    const u32 slot = method_slot(method_char);
    if (nodes[cur].route_idx_by_method[slot] == TrieNode::kInvalidRoute) {
        nodes[cur].route_idx_by_method[slot] = route_idx;
    }
    return true;
}

u16 RouteTrie::match(Str path, u8 method_char) const {
    FixedVec<Str, kMaxPathSegments> segs{};
    // Ignore tokenize's return value on overflow: `segs` still holds
    // the first kMaxPathSegments, and we want to walk the trie as deep
    // as we have data for. Bailing out on overflow would let a request
    // with 17+ segments bypass a '/' catchall or a matching prefix
    // route — realistic routes never go that deep, but realistic
    // request URIs can (ConnectionBase::kMaxReqPathLen is 64 bytes,
    // enough for 32 two-byte segments).
    (void)tokenize_segments(path, segs);

    const u32 want_slot = method_slot(method_char);
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
        if (child == TrieNode::kInvalidRoute) break;
        cur = child;
        const u16 candidate = pick_terminal(nodes[cur], want_slot);
        if (candidate != TrieNode::kInvalidRoute) best = candidate;
    }
    return best;
}

}  // namespace rut
