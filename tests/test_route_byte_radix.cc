// Tests for runtime/route_byte_radix.h: byte-level edge-compressed
// radix trie.
//
// Coverage:
//   - basic exact match + miss
//   - longest-prefix-match across edge splits
//   - edge splitting on partial match (the canonical "shared prefix
//     diverges later" pattern)
//   - method-slot routing + first-insert-wins on duplicates
//   - request-time '?' / '#' stripping
//   - leading / trailing '/' canonicalization at insert and match
//   - capacity rejection with atomic rollback
//   - byte-aware (non-segment-aware) semantics — distinct from trie

#include "rut/runtime/route_byte_radix.h"
#include "test.h"

using namespace rut;

namespace {

constexpr Str S(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return Str{s, n};
}

}  // namespace

// ============================================================================
// Basic match
// ============================================================================

TEST(route_byte_radix, exact_match_single_route) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    CHECK_EQ(t.match(S("/health"), 0), 7u);
}

TEST(route_byte_radix, no_match_when_no_routes) {
    ByteRadixTrie t;
    CHECK_EQ(t.match(S("/anything"), 0), TrieNode::kInvalidRoute);
}

TEST(route_byte_radix, longest_prefix_match_wins) {
    // Two registered routes: /api and /api/v1. A request for
    // /api/v1/users hits /api/v1 (longer prefix) regardless of the
    // insert order. Distinct from linear-scan first-match-wins —
    // the selector picks byte_radix only for configs that don't
    // depend on linear-scan precedence.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api"), 0, 0));
    REQUIRE(t.insert(S("/api/v1"), 0, 1));
    CHECK_EQ(t.match(S("/api"), 0), 0u);           // exact /api
    CHECK_EQ(t.match(S("/api/v1"), 0), 1u);        // exact /api/v1
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 1u);  // longer wins
    CHECK_EQ(t.match(S("/api/other"), 0), 0u);     // /api wins (only /api fits)
}

TEST(route_byte_radix, longest_prefix_independent_of_insert_order) {
    // Insert /api/v1 BEFORE /api — same outcome.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api/v1"), 0, 0));
    REQUIRE(t.insert(S("/api"), 0, 1));
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 0u);
    CHECK_EQ(t.match(S("/api/other"), 0), 1u);
}

// ============================================================================
// Edge splitting — the structurally interesting case
// ============================================================================

TEST(route_byte_radix, edge_split_on_partial_match) {
    // Insert /api/v1 first (one edge "api/v1" from root). Then insert
    // /api/v2 — the edge must split at "api/v" (5 bytes shared) into
    //   root → "api/v" → {"1": old terminal, "2": new terminal}
    // Both routes must remain matchable after the split.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api/v1"), 0, 0));
    REQUIRE(t.insert(S("/api/v2"), 0, 1));
    CHECK_EQ(t.match(S("/api/v1"), 0), 0u);
    CHECK_EQ(t.match(S("/api/v2"), 0), 1u);
    // Sibling that doesn't share the "api/v" stem must miss.
    CHECK_EQ(t.match(S("/api/x"), 0), TrieNode::kInvalidRoute);
}

TEST(route_byte_radix, multiple_splits_preserve_terminals) {
    // /api/v1/users inserted first creates one long edge.
    // /api/v1/orders splits at "api/v1/" → {users, orders}.
    // /api/v2 splits at "api/v" → {"1/" → {users, orders}, "2"}.
    // All four terminals (the three plus an /api shorter-prefix) must
    // resolve to their original idx.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api/v1/users"), 0, 0));
    REQUIRE(t.insert(S("/api/v1/orders"), 0, 1));
    REQUIRE(t.insert(S("/api/v2"), 0, 2));
    REQUIRE(t.insert(S("/api"), 0, 3));
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 0u);
    CHECK_EQ(t.match(S("/api/v1/orders"), 0), 1u);
    CHECK_EQ(t.match(S("/api/v2"), 0), 2u);
    CHECK_EQ(t.match(S("/api"), 0), 3u);
    // Longest-prefix-match: /api/v1/anything hits /api (the only
    // matching prefix of /api/v1/anything when v1's children diverge).
    CHECK_EQ(t.match(S("/api/v1/x"), 0), 3u);
    CHECK_EQ(t.match(S("/api/v3"), 0), 3u);
}

// ============================================================================
// Byte-aware (NOT segment-aware) semantics
// ============================================================================

TEST(route_byte_radix, byte_match_crosses_segment_boundaries) {
    // /api matches /apixyz because the trie is byte-aware. SegmentTrie
    // would NOT match /apixyz against /api (segment boundary would
    // have to align). This is the contract distinction the selector
    // uses to choose between dispatches.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api"), 0, 0));
    CHECK_EQ(t.match(S("/api"), 0), 0u);
    CHECK_EQ(t.match(S("/apixyz"), 0), 0u);
    CHECK_EQ(t.match(S("/apex"), 0), TrieNode::kInvalidRoute);  // diverges at 2nd byte
}

TEST(route_byte_radix, request_strips_query_and_fragment) {
    // Request paths arrive raw from the parser; '?' / '#' must not
    // participate in byte matching, otherwise /health?q=1 wouldn't hit
    // a registered /health route.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/health"), 0, 0));
    REQUIRE(t.insert(S("/api/users"), 0, 1));
    CHECK_EQ(t.match(S("/health?check=1"), 0), 0u);
    CHECK_EQ(t.match(S("/health?"), 0), 0u);
    CHECK_EQ(t.match(S("/health#frag"), 0), 0u);
    CHECK_EQ(t.match(S("/api/users?page=3"), 0), 1u);
}

TEST(route_byte_radix, trailing_slash_normalized) {
    // P2a-style: a trailing '/' on the route at insert OR on the
    // request at match is stripped to canonicalize to the same key.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/api"), 0, 0));
    CHECK_EQ(t.match(S("/api"), 0), 0u);
    CHECK_EQ(t.match(S("/api/"), 0), 0u);   // trailing slash on request
    REQUIRE(t.insert(S("/admin/"), 0, 1));  // trailing slash on route
    CHECK_EQ(t.match(S("/admin"), 0), 1u);
    CHECK_EQ(t.match(S("/admin/"), 0), 1u);
}

// ============================================================================
// Method dispatch
// ============================================================================

TEST(route_byte_radix, method_specific_beats_any_at_same_path) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/x"), 0, 0));    // any
    REQUIRE(t.insert(S("/x"), 'G', 1));  // GET-specific
    CHECK_EQ(t.match(S("/x"), 'G'), 1u);
    CHECK_EQ(t.match(S("/x"), 'P'), 0u);  // POST falls back to any
    CHECK_EQ(t.match(S("/x"), 0), 0u);
}

TEST(route_byte_radix, first_insert_wins_on_dup_method_slot) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/x"), 'G', 0));
    REQUIRE(t.insert(S("/x"), 'G', 1));  // duplicate (path, method)
    CHECK_EQ(t.match(S("/x"), 'G'), 0u);
}

TEST(route_byte_radix, rejects_unsupported_method_byte_at_insert) {
    ByteRadixTrie t;
    // 'g' (lowercase) is not a recognized method byte.
    CHECK(!t.insert(S("/x"), 'g', 0));
    CHECK(!t.insert(S("/x"), 'X', 0));
}

TEST(route_byte_radix, rejects_unsupported_method_byte_at_match) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/x"), 'G', 0));
    CHECK_EQ(t.match(S("/x"), 'g'), TrieNode::kInvalidRoute);
    CHECK_EQ(t.match(S("/x"), 'X'), TrieNode::kInvalidRoute);
}

// ============================================================================
// Capacity / atomic rollback
// ============================================================================

TEST(route_byte_radix, atomic_insert_on_node_pool_exhaustion) {
    // Drive the pool to exactly kMaxNodes - 1 with a deterministic
    // sequence, then attempt an insert that demands 2 new nodes.
    // With only 1 slot left the second allocation must fail and the
    // rollback must restore the pre-insert pool exactly.
    //
    // The earlier shape used "if (!t.insert(...)) break;" plus a
    // best-effort deep insert and accepted EITHER success or failure;
    // Copilot on #46 round 3 flagged that the rollback path was
    // never actually exercised. The new shape is fully deterministic.
    ByteRadixTrie t;

    // Phase 1 — fill root to kMaxChildren with 2-byte-edge leaves.
    // Each insert: first byte unseen at root → add one leaf with a
    // 2-byte edge. +1 node per insert. After kMaxChildren inserts:
    // root + kMaxChildren leaves = 1 + kMaxChildren nodes.
    //
    // Each path needs its own backing buffer because the trie stores
    // non-owning Str views into the path bytes; reusing one buffer
    // across inserts would have every previously-inserted edge alias
    // the latest write and collapse all leaves into one (subtle, and
    // the in-place mutation of buffer contents would even change
    // later find_child_by_first_byte lookups).
    char p1_buf[ByteRadixNode::kMaxChildren][3];
    for (u32 i = 0; i < ByteRadixNode::kMaxChildren; i++) {
        p1_buf[i][0] = '/';
        p1_buf[i][1] = static_cast<char>(0x40 + i);
        p1_buf[i][2] = 'x';
        REQUIRE(t.insert(Str{p1_buf[i], 3}, 0, static_cast<u16>(i)));
    }
    REQUIRE_EQ(t.node_count(), 1u + ByteRadixNode::kMaxChildren);

    // Phase 2 — extend leaves with a 1-byte tail. Each descendant
    // insert: full edge match on the 2-byte parent edge, then add
    // one leaf for the trailing 'y'. +1 node per insert. Stop one
    // short of kMaxNodes so phase 3 has exactly 1 free slot.
    constexpr u32 phase2 =
        ByteRadixTrie::kMaxNodes - 2u - ByteRadixNode::kMaxChildren;  // 126 with 256/128
    static_assert(phase2 < ByteRadixNode::kMaxChildren,
                  "phase2 must consume at most kMaxChildren leaves");
    char p2_buf[phase2][4];
    for (u32 i = 0; i < phase2; i++) {
        p2_buf[i][0] = '/';
        p2_buf[i][1] = static_cast<char>(0x40 + i);
        p2_buf[i][2] = 'x';
        p2_buf[i][3] = 'y';
        REQUIRE(t.insert(Str{p2_buf[i], 4}, 0, static_cast<u16>(1000 + i)));
    }
    REQUIRE_EQ(t.node_count(), ByteRadixTrie::kMaxNodes - 1);

    // Phase 3 — an insert that splits a 2-byte parent edge AND adds
    // a sibling leaf. Needs 2 new nodes (split-tail + leaf); only 1
    // slot remains, so rollback must fire. We split the edge of the
    // 0th leaf ("/<0x40>x") with a fresh tail "z" via path "/<0x40>z".
    const char split_path[3] = {'/', static_cast<char>(0x40), 'z'};
    const u32 nodes_before = t.node_count();
    CHECK(!t.insert(Str{split_path, 3}, 0, 9999));
    CHECK_EQ(t.node_count(), nodes_before);  // rollback restored exactly

    // All phase-1 routes still match correctly — rollback didn't
    // corrupt their terminals or edges.
    for (u32 i = 0; i < ByteRadixNode::kMaxChildren; i++) {
        CHECK_EQ(t.match(Str{p1_buf[i], 3}, 0), static_cast<u16>(i));
    }
}

TEST(route_byte_radix, clear_resets_state) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/a"), 0, 0));
    REQUIRE(t.insert(S("/b"), 0, 1));
    const u32 nodes_after_inserts = t.node_count();
    CHECK(nodes_after_inserts > 1u);
    t.clear();
    CHECK_EQ(t.node_count(), 1u);  // root only
    CHECK_EQ(t.match(S("/a"), 0), TrieNode::kInvalidRoute);
    REQUIRE(t.insert(S("/c"), 0, 0));
    CHECK_EQ(t.match(S("/c"), 0), 0u);
    CHECK_EQ(t.match(S("/a"), 0), TrieNode::kInvalidRoute);
}

// ============================================================================
// Empty / root path
// ============================================================================

TEST(route_byte_radix, rejects_non_origin_form_request_targets) {
    // Codex P1 on #46: with a configured "/" catchall, match() seeded
    // `best` from the root terminal regardless of whether the request
    // was origin-form. Asterisk-form ("*"), authority-form ("host:port"),
    // and empty targets must NOT match any route — the linear-scan
    // dispatch never matches them either, and HTTP/1.1 doesn't define
    // path routing for them.
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/"), 0, 99));    // catchall
    REQUIRE(t.insert(S("/api"), 0, 7));  // specific
    // Origin-form still works.
    CHECK_EQ(t.match(S("/api"), 0), 7u);
    CHECK_EQ(t.match(S("/anything"), 0), 99u);
    // Non-origin-form: NO match, even though "/" is registered.
    CHECK_EQ(t.match(S("*"), 0), TrieNode::kInvalidRoute);
    CHECK_EQ(t.match(S("example.com:443"), 0), TrieNode::kInvalidRoute);
    CHECK_EQ(t.match(S(""), 0), TrieNode::kInvalidRoute);
}

TEST(route_byte_radix, accepts_kMaxRoutes_distinct_top_level_prefixes) {
    // Codex P1 on #46 round 2: per-node fan-out was capped at 16,
    // which made any config with 17+ distinct top-level prefixes
    // fail at insert time even though kMaxRoutes is 128. Bumped to
    // 128 to cover the worst case.
    //
    // Worst-case shape: 128 paths whose first byte after the leading
    // '/' is unique across the set, so they all become direct children
    // of root and force the children FixedVec to its cap.
    //
    // Earlier r3 version used "/aa", "/ab", ... for 128 paths but
    // collapsed all 128 to only 5 distinct first bytes — the root
    // never actually grew past ~5 children, so the test passed even
    // with the old kMaxChildren=16. Copilot on #46 round 3 caught
    // it. We now use a contiguous run of 128 unique non-special
    // bytes (0x40..0xbf, none of which are '/', '?', '#', or NUL).
    ByteRadixTrie t;
    char paths[ByteRadixNode::kMaxChildren][2];
    for (u32 i = 0; i < ByteRadixNode::kMaxChildren; i++) {
        paths[i][0] = '/';
        paths[i][1] = static_cast<char>(0x40 + i);
        REQUIRE(t.insert(Str{paths[i], 2}, 0, static_cast<u16>(i)));
    }
    // Root must hold exactly kMaxChildren leaves now — verify via
    // node_count (1 root + kMaxChildren leaves).
    CHECK_EQ(t.node_count(), 1u + ByteRadixNode::kMaxChildren);
    // Spot-check a few — the first, the middle, and the last.
    CHECK_EQ(t.match(Str{paths[0], 2}, 0), 0u);
    CHECK_EQ(t.match(Str{paths[64], 2}, 0), 64u);
    CHECK_EQ(t.match(Str{paths[ByteRadixNode::kMaxChildren - 1], 2}, 0),
             static_cast<u16>(ByteRadixNode::kMaxChildren - 1));
}

TEST(route_byte_radix, root_path_matches_root_terminal) {
    ByteRadixTrie t;
    REQUIRE(t.insert(S("/"), 0, 42));
    CHECK_EQ(t.match(S("/"), 0), 42u);
    // Root is a "shorter-than-anything" prefix — every request that
    // starts with '/' inherits the root terminal as a fallback.
    CHECK_EQ(t.match(S("/anything"), 0), 42u);
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 42u);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
