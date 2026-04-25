#include "rut/runtime/route_byte_radix.h"

#include "rut/runtime/route_table.h"

namespace rut {

namespace {

// Strip leading '/' and a trailing '/' run from `path`. insert() and
// match() share this so a route registered as "/api" and a request for
// "/api" tokenize to the same byte sequence ("api"); a trailing '/' on
// either side is treated as no-trailing-'/' the same way SegmentTrie's
// P2a normalization does.
Str strip_slashes(Str path) {
    u32 lo = 0;
    if (lo < path.len && path.ptr[lo] == '/') lo++;
    u32 hi = path.len;
    while (hi > lo && path.ptr[hi - 1] == '/') hi--;
    return Str{path.ptr + lo, hi - lo};
}

// Match-time-only: also stop at '?' / '#' so query / fragment bytes
// don't participate in byte matching. Mirrors what SegmentTrie::match
// does internally; insert() doesn't strip these because RouteConfig
// already rejected paths containing them.
Str canonicalize_request(Str path) {
    u32 lo = 0;
    if (lo < path.len && path.ptr[lo] == '/') lo++;
    u32 hi = lo;
    while (hi < path.len && path.ptr[hi] != '?' && path.ptr[hi] != '#') hi++;
    while (hi > lo && path.ptr[hi - 1] == '/') hi--;
    return Str{path.ptr + lo, hi - lo};
}

}  // namespace

void ByteRadixTrie::clear() {
    nodes.len = 0;
    ByteRadixNode root{};
    [[maybe_unused]] bool ok = nodes.push(root);
    // push cannot fail on a fresh FixedVec; nodes starts empty.
}

u16 ByteRadixTrie::pick_terminal(const ByteRadixNode& n, u32 slot) {
    if (slot != 0 && n.route_idx_by_method[slot] != TrieNode::kInvalidRoute) {
        return n.route_idx_by_method[slot];
    }
    return n.route_idx_by_method[0];
}

u16 ByteRadixTrie::find_child_by_first_byte(u16 parent, u8 b) const {
    const auto& p = nodes[parent];
    for (u32 i = 0; i < p.children.len; i++) {
        const u16 ci = p.children[i];
        if (nodes[ci].edge.len > 0 && static_cast<u8>(nodes[ci].edge.ptr[0]) == b) {
            return ci;
        }
    }
    return TrieNode::kInvalidNodeIdx;
}

bool ByteRadixTrie::insert(Str path, u8 method_char, u16 route_idx) {
    const u32 slot = method_slot(method_char);
    if (slot == kMethodSlotInvalid) return false;

    const Str p = strip_slashes(path);

    // Atomic insert: snapshot the whole node pool so a mid-insert
    // capacity failure rolls back to pre-insert state. Edge fields are
    // non-owning Str views (16 B each) and the FixedVec child arrays
    // are POD u16 buffers, so a memcpy-style snapshot is cheap. Worst
    // case ~256 × ~70 B ≈ 18 KB copied, dwarfed by the trie's overall
    // build cost on configs that even hit the pool cap.
    //
    // Per-step pre-flight isn't sufficient on its own: a same-step
    // edge split allocates one node and would leave the parent's
    // edge truncated even if the subsequent leaf-allocation failed.
    const u32 saved_len = nodes.len;
    ByteRadixNode saved_nodes[kMaxNodes];
    for (u32 i = 0; i < saved_len; i++) saved_nodes[i] = nodes[i];

    auto rollback = [&]() {
        for (u32 i = 0; i < saved_len; i++) nodes[i] = saved_nodes[i];
        nodes.len = saved_len;
    };

    u16 cur = 0;
    u32 i = 0;
    while (i < p.len) {
        const u8 b = static_cast<u8>(p.ptr[i]);
        const u16 child = find_child_by_first_byte(cur, b);
        if (child == TrieNode::kInvalidNodeIdx) {
            // No matching child — append a leaf with the rest of the
            // path as its edge.
            if (nodes.len >= kMaxNodes) {
                rollback();
                return false;
            }
            ByteRadixNode nn;
            nn.edge = Str{p.ptr + i, p.len - i};
            if (!nodes.push(nn)) {
                rollback();
                return false;
            }
            const u16 ni = static_cast<u16>(nodes.len - 1);
            if (!nodes[cur].children.push(ni)) {
                rollback();
                return false;
            }
            cur = ni;
            i = p.len;
            break;
        }
        // Match as many bytes of the existing edge as possible.
        const Str e = nodes[child].edge;
        u32 k = 0;
        while (k < e.len && i + k < p.len && e.ptr[k] == p.ptr[i + k]) k++;
        if (k == e.len) {
            // Full edge match — descend into child.
            cur = child;
            i += k;
            continue;
        }
        // Partial match — split the edge at byte k. The original child
        // now holds the shared prefix [0,k); a new node holds the old
        // tail [k,e.len) with all of the original child's terminal
        // slots and children moved to it.
        if (nodes.len >= kMaxNodes) {
            rollback();
            return false;
        }
        ByteRadixNode tail;
        tail.edge = Str{e.ptr + k, e.len - k};
        for (u32 m = 0; m < kMethodSlots; m++) {
            tail.route_idx_by_method[m] = nodes[child].route_idx_by_method[m];
        }
        tail.children = nodes[child].children;
        if (!nodes.push(tail)) {
            rollback();
            return false;
        }
        const u16 tail_idx = static_cast<u16>(nodes.len - 1);
        // Truncate child to shared prefix; clear its terminals/children
        // (they moved to `tail`); install `tail` as its sole child.
        nodes[child].edge = Str{e.ptr, k};
        for (u32 m = 0; m < kMethodSlots; m++) {
            nodes[child].route_idx_by_method[m] = TrieNode::kInvalidRoute;
        }
        nodes[child].children.len = 0;
        if (!nodes[child].children.push(tail_idx)) {
            rollback();
            return false;
        }
        cur = child;
        i += k;
        // Loop continues — the new path either continues into a fresh
        // sibling (next iteration's "no matching child" branch) or
        // ends right at the split point (loop exits, terminal slot set
        // on `cur`).
    }

    // First-insert-wins on duplicate (path, method).
    if (nodes[cur].route_idx_by_method[slot] == TrieNode::kInvalidRoute) {
        nodes[cur].route_idx_by_method[slot] = route_idx;
    }
    return true;
}

u16 ByteRadixTrie::match(Str path, u8 method_char) const {
    const u32 want_slot = method_slot(method_char);
    if (want_slot == kMethodSlotInvalid) return TrieNode::kInvalidRoute;

    // Reject non-origin-form request targets BEFORE seeding `best`
    // from the root terminal. Otherwise a configured "/" catchall
    // would silently match HTTP/1.1 asterisk-form ("*"), authority-
    // form ("host:port"), or empty targets — none of which are path-
    // routable and the linear-scan dispatch never returns a route for
    // them. RouteTrie::match applies the same guard (Codex P2 caught
    // it there originally, P1 reapplied here on #46 round 1).
    if (path.len == 0 || path.ptr[0] != '/') return TrieNode::kInvalidRoute;

    const Str p = canonicalize_request(path);
    u16 cur = 0;
    u16 best = pick_terminal(nodes[0], want_slot);
    u32 i = 0;
    while (i < p.len) {
        const u8 b = static_cast<u8>(p.ptr[i]);
        const u16 child = find_child_by_first_byte(cur, b);
        if (child == TrieNode::kInvalidNodeIdx) break;
        const Str e = nodes[child].edge;
        if (i + e.len > p.len) break;  // request shorter than remaining edge
        for (u32 k = 0; k < e.len; k++) {
            if (e.ptr[k] != p.ptr[i + k]) return best;
        }
        cur = child;
        i += e.len;
        const u16 candidate = pick_terminal(nodes[cur], want_slot);
        if (candidate != TrieNode::kInvalidRoute) best = candidate;
    }
    return best;
}

namespace {

u16 byte_radix_match(const RouteConfig* cfg, Str path, u8 method) {
    // Translate the impl's miss sentinel (TrieNode::kInvalidRoute,
    // shared with SegmentTrie) into the dispatch interface's miss
    // sentinel (kRouteIdxInvalid). Both are 0xffffu numerically today,
    // but the names mean different things — the impl's is a route-
    // table sentinel scoped to its own match() return; the
    // interface's is a vtable contract. Codex on #46 caught the
    // bare passthrough — segment_trie_match in route_dispatch.cc
    // already does the same translation.
    const u16 idx = cfg->byte_radix_state.match(path, method);
    return idx == TrieNode::kInvalidRoute ? kRouteIdxInvalid : idx;
}

}  // namespace

const RouteDispatch kByteRadixDispatch = {&byte_radix_match};

}  // namespace rut
