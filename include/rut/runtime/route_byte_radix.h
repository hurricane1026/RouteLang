#pragma once

// ByteRadix — byte-level edge-compressed radix trie.
//
// Each node holds an `edge` (a contiguous byte run from its parent).
// Branching happens when prefixes diverge. Insertion may split an
// existing edge when a new path shares some prefix with it; the lower
// half of the edge moves into a new node and the original node's edge
// shrinks to the shared prefix.
//
// Match semantics: longest-prefix-match in BYTES. A request for
// "/api/v1/users" hits a route registered at "/api/v1" if it exists,
// even when "/api" is also registered (longer wins). At each terminal
// visited during descent we record the candidate route_idx; descent
// stops at the first edge byte mismatch or path exhaustion, and we
// return the deepest candidate seen.
//
// vs SegmentTrie:
//   - Byte-aware, NOT segment-aware. "/api/v1" matches "/api/v1xyz"
//     because there's no segment-boundary check. The selector picks
//     this dispatch only for configs whose route paths don't depend
//     on segment boundaries (no `:param` segments, no overlapping
//     routes that need the segment-level distinction).
//   - Edge compression typically yields fewer nodes for the same
//     routes: "/api/v1/users" + "/api/v1/orders" share an
//     "/api/v1/" edge of 9 bytes, then branch on 'u' vs 'o'. A
//     segment trie would split into 4 segment nodes (api / v1 /
//     users|orders).
//
// Bench data (closed #41 branch, realistic SaaS gateway, hot cache):
//   N=128 routes:
//     byte_radix:    0.91 us / match — fastest variant tested
//     segment_trie:  1.76 us / match
//     linear_scan:   2.00 us / match  (baseline)
//   IPC 4.25 (highest in the table): the compressed edges fit more
//   routes in fewer cache lines, descent is straight-line work.
//
// Storage: ~256 nodes × ~70 bytes/node ≈ 18 KB inline.
//
// Build-time canonicalization: insert strips a leading '/' and any
// trailing '/'. The match path strips '?' / '#' suffix in addition,
// so request bytes after the query/fragment marker don't participate.
// Routes registered with '?' or '#' are rejected by
// RouteConfig::is_routable_path before they reach insert().
//
// First-insert-wins on duplicate (path, method): the per-method
// terminal slot is set only if currently kInvalidRoute. RouteConfig
// guarantees route_idx is monotonic with insertion order, so older
// terminals (smaller route_idx) shadow newer duplicates.

#include "rut/common/types.h"
#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_trie.h"  // for kMethodSlots, method_slot, TrieNode::kInvalidRoute

namespace rut {

struct ByteRadixNode {
    // Per-node fan-out cap. Realistic configs branch <8 children at most
    // points; 16 covers shared-byte-then-divergent-tail patterns common
    // at the root (/, h, a, w, ...).
    static constexpr u32 kMaxChildren = 16;

    // Edge label: the byte run leading INTO this node. Non-owning view
    // into RouteEntry::path; safe for the config's RCU lifetime.
    Str edge{};

    // Child node-pool indices, scanned linearly (find_child equivalent).
    // Order is insertion order so first-byte-match returns the first
    // registered child with that byte.
    FixedVec<u16, kMaxChildren> children;

    // Per-method route slot at this terminal. kInvalidRoute means "this
    // node is not a terminal for that method". Slot 0 is "any".
    u16 route_idx_by_method[kMethodSlots] = {TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute,
                                             TrieNode::kInvalidRoute};
};

class ByteRadixTrie {
public:
    // 256 nodes covers 128 routes worst-case (no prefix sharing →
    // 1 root + 128 leaves; even less with realistic byte-prefix
    // overlap). Leaves room for edge splits during insert.
    static constexpr u32 kMaxNodes = 256;

    ByteRadixTrie() { clear(); }

    // Wipe and re-seed with the empty root node.
    void clear();

    // Insert a (path, method, route_idx). Returns false if:
    //   - method byte isn't recognized (mirrors trie/hash strict-method
    //     contract — selector should pre-filter unsupported methods),
    //   - the trie is out of node-pool / per-node-children capacity at
    //     any step. Insert is atomic — any structural mutation is
    //     rolled back to the pre-insert state on failure, so a partial
    //     insert can never leave dangling nodes that future inserts
    //     would inherit.
    //
    // CONTRACT: route_idx is assumed monotonic with insertion order
    // (RouteConfig::add_* guarantees this via route_count++). First-
    // insert-wins on duplicate (path, method) — the terminal slot is
    // set only if currently empty.
    bool insert(Str path, u8 method_char, u16 route_idx);

    // Look up `path` — returns the longest-prefix terminal's route_idx
    // for the matching method slot, or kInvalidRoute on no match.
    // method_char == 0 ("any") looks at slot 0 directly; specific
    // methods prefer their own slot but fall back to slot 0 at each
    // candidate terminal.
    u16 match(Str path, u8 method_char) const;

    // Introspection (tests / bench).
    u32 node_count() const { return nodes.len; }

private:
    FixedVec<ByteRadixNode, kMaxNodes> nodes;

    // Pick a method slot at a terminal node, with the any-slot fallback.
    static u16 pick_terminal(const ByteRadixNode& n, u32 slot);

    // Find the child of `parent` whose edge starts with byte `b`.
    // Returns TrieNode::kInvalidNodeIdx on no match.
    u16 find_child_by_first_byte(u16 parent, u8 b) const;
};

}  // namespace rut
