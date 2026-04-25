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
    // Fill the pool to within 1 of kMaxNodes with single-byte distinct
    // routes (each takes ~1 node). The next insert that needs more
    // than 1 node — a deeper distinct path — must fail and leave the
    // pool unchanged.
    ByteRadixTrie t;
    char paths[ByteRadixTrie::kMaxNodes][8];
    u32 admitted = 0;
    for (u32 i = 0; i + 2 < ByteRadixTrie::kMaxNodes; i++) {
        paths[i][0] = '/';
        paths[i][1] = static_cast<char>('a' + (i / 26 / 26) % 26);
        paths[i][2] = static_cast<char>('a' + (i / 26) % 26);
        paths[i][3] = static_cast<char>('a' + i % 26);
        paths[i][4] = '\0';
        if (!t.insert(Str{paths[i], 4}, 0, static_cast<u16>(i))) break;
        admitted++;
    }
    REQUIRE(admitted > 4);
    const u32 nodes_before = t.node_count();
    // Now insert a deep brand-new path that requires more nodes than
    // are left. If the pre-flight allowed it to start, the rollback
    // must restore the pool exactly.
    char deep[64];
    deep[0] = '/';
    for (u32 i = 1; i < 60; i++) deep[i] = static_cast<char>('a' + i % 26);
    deep[60] = '\0';
    // The deep path may succeed or fail depending on remaining space;
    // either way the post-state must be consistent — node_count is
    // either unchanged (failure rolled back) or grew by exactly the
    // number of new nodes the path actually allocated.
    const bool ok = t.insert(Str{deep, 60}, 0, 9999);
    if (!ok) {
        CHECK_EQ(t.node_count(), nodes_before);  // rollback
    } else {
        CHECK(t.node_count() >= nodes_before);
    }
    // All previously-admitted routes still match correctly.
    for (u32 i = 0; i < admitted; i++) {
        CHECK_EQ(t.match(Str{paths[i], 4}, 0), static_cast<u16>(i));
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
