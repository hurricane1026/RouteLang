// Tests for runtime/route_hash_full.h: exact-match hash dispatch.
//
// Covers the contract laid out at the top of the header:
//   - exact-string matching (no prefix walking)
//   - method-slot routing (specific beats any; any-fallback when
//     specific misses)
//   - capacity rejection at the load-factor cap
//   - duplicate-key idempotence (first insert wins)
//   - distinct hashing of (path, 0) vs (path, method) so they coexist
//   - probe correctness under collision (open-addressing wraparound)

#include "rut/runtime/route_hash_full.h"
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

TEST(route_hash_full, exact_match_single_route) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    CHECK_EQ(t.match(S("/health"), 0), 7u);
}

TEST(route_hash_full, no_match_returns_invalid) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    CHECK_EQ(t.match(S("/missing"), 0), kRouteIdxInvalid);
    CHECK_EQ(t.match(S(""), 0), kRouteIdxInvalid);  // empty is never a route
}

TEST(route_hash_full, no_prefix_walking) {
    // Crucial contract distinction from SegmentTrie: hash_full_path
    // treats /api as STRICTLY EXACT — a request for /api/v1/users
    // does NOT match a route registered at /api. The selector must
    // never pick this dispatch for configs that need prefix semantics.
    HashFullPathTable t;
    REQUIRE(t.insert(S("/api"), 0, 1));
    CHECK_EQ(t.match(S("/api"), 0), 1u);
    CHECK_EQ(t.match(S("/api/v1"), 0), kRouteIdxInvalid);
    CHECK_EQ(t.match(S("/api/v1/users"), 0), kRouteIdxInvalid);
    CHECK_EQ(t.match(S("/api/"), 0), kRouteIdxInvalid);  // trailing slash differs too
}

// ============================================================================
// Method dispatch
// ============================================================================

TEST(route_hash_full, method_specific_beats_any_via_distinct_slots) {
    // Two routes at the same path with different methods both fit; the
    // hash function folds method into the key so they don't collide.
    HashFullPathTable t;
    REQUIRE(t.insert(S("/x"), 0, 10));
    REQUIRE(t.insert(S("/x"), 'G', 20));
    CHECK_EQ(t.match(S("/x"), 'G'), 20u);
    CHECK_EQ(t.match(S("/x"), 0), 10u);    // direct hit on the any slot
    CHECK_EQ(t.match(S("/x"), 'P'), 10u);  // POST → any-slot fallback
    CHECK_EQ(t.match(S("/x"), 'D'), 10u);  // DELETE → any-slot fallback
}

TEST(route_hash_full, any_only_route_serves_every_method) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/wild"), 0, 99));
    CHECK_EQ(t.match(S("/wild"), 'G'), 99u);
    CHECK_EQ(t.match(S("/wild"), 'P'), 99u);
    CHECK_EQ(t.match(S("/wild"), 'D'), 99u);
    CHECK_EQ(t.match(S("/wild"), 0), 99u);
}

TEST(route_hash_full, specific_only_route_does_not_serve_other_methods) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/get-only"), 'G', 5));
    CHECK_EQ(t.match(S("/get-only"), 'G'), 5u);
    CHECK_EQ(t.match(S("/get-only"), 'P'), kRouteIdxInvalid);
    CHECK_EQ(t.match(S("/get-only"), 0), kRouteIdxInvalid);
}

TEST(route_hash_full, method_first_insert_wins_on_exact_dup) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/x"), 'G', 1));
    // Second insert at the same (path, method) is idempotent, not an
    // overwrite. Same shape as the trie's first-wins.
    REQUIRE(t.insert(S("/x"), 'G', 2));
    CHECK_EQ(t.match(S("/x"), 'G'), 1u);
}

// ============================================================================
// Capacity / build-time guards
// ============================================================================

TEST(route_hash_full, accepts_routes_up_to_load_factor_cap) {
    // The load-factor cap is < 50% (n_*2 >= kCap rejects), so kCap/2
    // = 256 inserts must all succeed at distinct paths.
    HashFullPathTable t;
    constexpr u32 kN = HashFullPathTable::kCap / 2;
    char paths[kN][16];
    for (u32 i = 0; i < kN; i++) {
        // Construct distinct paths "/r0000".."/r0255".
        paths[i][0] = '/';
        paths[i][1] = 'r';
        paths[i][2] = static_cast<char>('0' + (i / 100) % 10);
        paths[i][3] = static_cast<char>('0' + (i / 10) % 10);
        paths[i][4] = static_cast<char>('0' + i % 10);
        paths[i][5] = '\0';
        CHECK(t.insert(Str{paths[i], 5}, 0, static_cast<u16>(i)));
    }
    CHECK_EQ(t.size(), kN);
    // The next insert must trip the load-factor cap.
    CHECK(!t.insert(S("/overflow"), 0, 999));
}

TEST(route_hash_full, distinct_paths_dont_alias) {
    // Non-trivial probe correctness: insert N distinct routes, look
    // each one up, ensure we get the right index back. Catches any
    // off-by-one in the wraparound or method-fold logic.
    HashFullPathTable t;
    constexpr u32 kN = 64;
    char paths[kN][16];
    for (u32 i = 0; i < kN; i++) {
        paths[i][0] = '/';
        paths[i][1] = 'p';
        paths[i][2] = static_cast<char>('0' + (i / 10) % 10);
        paths[i][3] = static_cast<char>('0' + i % 10);
        paths[i][4] = '\0';
        REQUIRE(t.insert(Str{paths[i], 4}, 0, static_cast<u16>(i)));
    }
    for (u32 i = 0; i < kN; i++) {
        CHECK_EQ(t.match(Str{paths[i], 4}, 0), static_cast<u16>(i));
    }
}

// ============================================================================
// Adapter / dispatch interface integration
// ============================================================================

TEST(route_hash_full, clear_resets_state) {
    HashFullPathTable t;
    REQUIRE(t.insert(S("/a"), 0, 1));
    REQUIRE(t.insert(S("/b"), 0, 2));
    CHECK_EQ(t.size(), 2u);
    t.clear();
    CHECK_EQ(t.size(), 0u);
    CHECK_EQ(t.match(S("/a"), 0), kRouteIdxInvalid);
    // Reusing after clear() must work cleanly.
    REQUIRE(t.insert(S("/c"), 0, 3));
    CHECK_EQ(t.match(S("/c"), 0), 3u);
    CHECK_EQ(t.match(S("/a"), 0), kRouteIdxInvalid);
}

TEST(route_hash_full, match_strips_query_and_fragment) {
    // Codex P2 on #44: the runtime parser keeps the raw request-target
    // in ParsedRequest.path, so "/health?check=1" arrives at match()
    // verbatim. Without stripping, hash dispatch would miss /health
    // routes that the linear scan and segment trie both find. Strip
    // before hashing.
    HashFullPathTable t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    REQUIRE(t.insert(S("/api/users"), 0, 12));
    CHECK_EQ(t.match(S("/health?check=1"), 0), 7u);
    CHECK_EQ(t.match(S("/health?"), 0), 7u);
    CHECK_EQ(t.match(S("/health#frag"), 0), 7u);
    CHECK_EQ(t.match(S("/api/users?page=3&limit=10"), 0), 12u);
    // After the strip the leading byte is still '/', so a request
    // whose stripped path is empty (e.g., "?frag" alone, or "")
    // falls through cleanly without aliasing into a registered key.
    CHECK_EQ(t.match(S("?foo"), 0), kRouteIdxInvalid);
    CHECK_EQ(t.match(S("#frag"), 0), kRouteIdxInvalid);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
