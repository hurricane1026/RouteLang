// Tests for runtime/route_trie.h: segment tokenization + trie insert/match.
//
// Coverage strategy: the trie has four moving parts — tokenize_segments
// (policy), find_child (linear scan with full segment compare), insert
// (build), match (longest-match + method fallback). Tokenize is
// covered by public observation: insert/match depend on it, so
// asserting correct lookup behavior across the canonical edge-case
// paths (multi-slash, trailing slash, case) exercises tokenize
// indirectly, while dedicated tests below pin the policy at the
// per-path level.

#include "rut/runtime/route_trie.h"
#include "test.h"

using namespace rut;

namespace {

constexpr Str S(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return Str{s, n};
}

// Insert a (path, method, idx) tuple; a bool-returning helper so callers can
// use REQUIRE in the test scope (the REQUIRE macro needs the `_tc` context
// variable that only exists inside TEST macros).
struct Insert {
    const char* path;
    u8 method;
    u16 idx;
};

bool build_ok(RouteTrie& t, const Insert* items, u32 n) {
    t.clear();
    for (u32 i = 0; i < n; i++) {
        if (!t.insert(S(items[i].path), items[i].method, items[i].idx)) return false;
    }
    return true;
}

}  // namespace

// ============================================================================
// Basic match
// ============================================================================

TEST(route_trie, exact_match_single_route) {
    RouteTrie t;
    const Insert items[] = {{"/health", 0, 7}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/health"), 0), 7u);
}

TEST(route_trie, no_match_returns_sentinel_when_no_catchall) {
    RouteTrie t;
    const Insert items[] = {{"/health", 0, 7}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/missing"), 0), TrieNode::kInvalidRoute);
    CHECK_EQ(t.match(S("/"), 0), TrieNode::kInvalidRoute);
}

TEST(route_trie, catchall_root_matches_unrouted_paths) {
    RouteTrie t;
    const Insert items[] = {{"/health", 0, 7}, {"/", 0, 99}};
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/health"), 0), 7u);    // specific wins over catchall
    CHECK_EQ(t.match(S("/missing"), 0), 99u);  // falls through to catchall
    CHECK_EQ(t.match(S("/"), 0), 99u);         // root path hits catchall
}

TEST(route_trie, longest_match_wins_regardless_of_insert_order) {
    // Two orderings of the same routes must produce the same match — the
    // whole point of moving off the linear first-match-wins scan.
    const Insert forward[] = {{"/api", 0, 1}, {"/api/v1", 0, 2}, {"/api/v1/users", 0, 3}};
    const Insert reverse[] = {{"/api/v1/users", 0, 3}, {"/api/v1", 0, 2}, {"/api", 0, 1}};
    RouteTrie ta, tb;
    REQUIRE(build_ok(ta, forward, 3));
    REQUIRE(build_ok(tb, reverse, 3));
    const char* reqs[] = {
        "/api", "/api/", "/api/v2", "/api/v1", "/api/v1/users", "/api/v1/users/42"};
    for (const char* req : reqs) {
        const u16 a = ta.match(S(req), 0);
        const u16 b = tb.match(S(req), 0);
        CHECK_EQ(a, b);
    }
    // Spot-check expectations:
    CHECK_EQ(ta.match(S("/api"), 0), 1u);
    CHECK_EQ(ta.match(S("/api/"), 0), 1u);    // P2a: trailing slash ≡ no slash
    CHECK_EQ(ta.match(S("/api/v2"), 0), 1u);  // deeper sibling not in trie → fall back
    CHECK_EQ(ta.match(S("/api/v1"), 0), 2u);  // exact intermediate
    CHECK_EQ(ta.match(S("/api/v1/users"), 0), 3u);
    CHECK_EQ(ta.match(S("/api/v1/users/42"), 0), 3u);  // deeper request → longest prefix wins
}

// ============================================================================
// Normalization policies (P1a, P2a, P3a)
// ============================================================================

TEST(route_trie, P1a_consecutive_slashes_collapse) {
    RouteTrie t;
    const Insert items[] = {{"/api/v1", 0, 10}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/api//v1"), 0), 10u);                 // client double-slash → normalize
    CHECK_EQ(t.match(S("//api/v1"), 0), 10u);                 // leading double-slash
    CHECK_EQ(t.match(S("/api///v1"), 0), 10u);                // triple
    CHECK_EQ(t.match(S("///"), 0), TrieNode::kInvalidRoute);  // all-slash path has no segments
}

TEST(route_trie, P1a_insert_path_with_extra_slashes_normalizes) {
    // Inserting "/api//v1" should be equivalent to inserting "/api/v1".
    RouteTrie t;
    const Insert items[] = {{"/api//v1", 0, 10}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/api/v1"), 0), 10u);
    CHECK_EQ(t.match(S("/api//v1"), 0), 10u);
}

TEST(route_trie, P2a_trailing_slash_equivalent) {
    RouteTrie t;
    const Insert items[] = {{"/api", 0, 5}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/api"), 0), 5u);
    CHECK_EQ(t.match(S("/api/"), 0), 5u);
    CHECK_EQ(t.match(S("/api//"), 0), 5u);
}

TEST(route_trie, strips_query_string_before_matching) {
    // Request paths from the HTTP parser include the raw request-target
    // (path + query + fragment). Routing must run on the path component
    // only, or GET /health?check=1 won't match a route registered as
    // /health.
    RouteTrie t;
    const Insert items[] = {{"/health", 0, 1}, {"/api/users", 0, 2}};
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/health?check=1"), 0), 1u);
    CHECK_EQ(t.match(S("/health?"), 0), 1u);
    CHECK_EQ(t.match(S("/health#frag"), 0), 1u);
    CHECK_EQ(t.match(S("/api/users?page=3&limit=10"), 0), 2u);
    // Query on a miss still falls through cleanly (no match).
    CHECK_EQ(t.match(S("/unknown?x=1"), 0), TrieNode::kInvalidRoute);
}

TEST(route_trie, P3a_case_sensitive) {
    RouteTrie t;
    const Insert items[] = {{"/api", 0, 1}, {"/API", 0, 2}};
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/api"), 0), 1u);
    CHECK_EQ(t.match(S("/API"), 0), 2u);
    CHECK_EQ(t.match(S("/Api"), 0), TrieNode::kInvalidRoute);  // different case, different route
}

// ============================================================================
// Method dispatch
// ============================================================================

TEST(route_trie, method_specific_beats_any_slot) {
    // When a path has both a method-specific and an any-method route, the
    // specific slot wins for matching methods, any wins for others.
    RouteTrie t;
    const Insert items[] = {{"/x", 0, 10}, {"/x", 'G', 20}};
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/x"), 'G'), 20u);
    CHECK_EQ(t.match(S("/x"), 'P'), 10u);  // POST → any slot
    CHECK_EQ(t.match(S("/x"), 'D'), 10u);  // DELETE → any slot
}

TEST(route_trie, tokenize_preserves_question_and_hash_bytes) {
    // tokenize_segments no longer strips '?' or '#' — that's now the
    // caller's job. match() shortens above '?' / '#' in the incoming
    // request, but insert() takes paths at face value. RouteConfig's
    // add_* rejects route paths containing those bytes so production
    // code can't reach the weird state, but the trie itself treats
    // "/api" and "/api?x" as distinct keys.
    RouteTrie t;
    const Insert items[] = {{"/api", 0, 1}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/api?x=1"), 0), 1u);
    CHECK_EQ(t.match(S("/api#frag"), 0), 1u);
}

TEST(route_trie, rejects_unsupported_method_at_insert) {
    // Codex P2 on #41: the earlier method_slot() mapped unknown method
    // bytes to slot 0 (any), which would silently broaden a typoed
    // route into an all-methods route. Now unknown bytes reject at
    // insert() cleanly so callers see the failure.
    RouteTrie t;
    t.clear();
    // Lowercase 'g' (typo for GET), uppercase 'X', or any unsupported
    // first byte must fail.
    CHECK(!t.insert(S("/x"), 'g', 1));
    CHECK(!t.insert(S("/x"), 'X', 1));
    CHECK(!t.insert(S("/x"), 'Z', 1));
    // Known chars still work.
    CHECK(t.insert(S("/x"), 'G', 1));
    CHECK(t.insert(S("/y"), 0, 2));  // method=0 is "any", still valid
}

TEST(route_trie, rejects_unsupported_method_at_match) {
    // Symmetric: a request with an unsupported method byte can't
    // match anything, even routes at the same path.
    RouteTrie t;
    const Insert items[] = {{"/x", 0, 10}, {"/x", 'G', 20}};
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/x"), 'G'), 20u);
    CHECK_EQ(t.match(S("/x"), 'g'), TrieNode::kInvalidRoute);  // typo
    CHECK_EQ(t.match(S("/x"), 'X'), TrieNode::kInvalidRoute);  // unknown verb
}

TEST(route_trie, admits_routes_with_more_than_16_segments) {
    // Codex P2 on #41: kMaxPathSegments was 16, which rejected valid
    // route configs with 17+ segments even when the path fit within
    // RouteEntry::kMaxPathLen. Bumped to 64 to cover the worst-case
    // 128-byte path (= 64 two-byte segments). A 17-segment path must
    // now insert and match.
    RouteTrie t;
    const char* deep = "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q";  // 17 segments
    CHECK(t.insert(S(deep), 0, 99));
    CHECK_EQ(t.match(S(deep), 0), 99u);
}

TEST(route_trie, method_first_insert_wins_on_exact_dup) {
    RouteTrie t;
    const Insert items[] = {{"/x", 'G', 1}, {"/x", 'G', 2}};  // same method-slot, duplicate
    REQUIRE(build_ok(t, items, 2));
    CHECK_EQ(t.match(S("/x"), 'G'), 1u);  // first insert pins the slot
}

TEST(route_trie, method_post_put_patch_ambiguity_preserved) {
    // The current first-char method scheme collapses POST/PUT/PATCH → 'P'.
    // Preserve that here until a follow-up PR introduces a proper enum.
    RouteTrie t;
    const Insert items[] = {{"/x", 'P', 99}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S("/x"), 'P'), 99u);
    CHECK_EQ(t.match(S("/x"), 'G'), TrieNode::kInvalidRoute);
}

// ============================================================================
// Build-time guards
// ============================================================================

TEST(route_trie, empty_path_inserts_at_root) {
    RouteTrie t;
    const Insert items[] = {{"", 0, 42}};
    REQUIRE(build_ok(t, items, 1));
    CHECK_EQ(t.match(S(""), 0), 42u);
    CHECK_EQ(t.match(S("/"), 0), 42u);          // "/" normalizes to root
    CHECK_EQ(t.match(S("/anything"), 0), 42u);  // catchall
}

TEST(route_trie, node_count_shows_prefix_sharing) {
    RouteTrie t;
    const Insert items[] = {
        {"/api/v1/users", 0, 1}, {"/api/v1/orders", 0, 2}, {"/api/v2/users", 0, 3}};
    REQUIRE(build_ok(t, items, 3));
    // Root + {"api"} + {"v1", "v2"} + {"users", "orders", "users"} = 1 + 1 + 2 + 3 = 7.
    // If tokenize or insert accidentally didn't share the "api" prefix, we'd see 10.
    CHECK_EQ(t.node_count(), 7u);
}

TEST(route_trie, deep_path_still_falls_through_to_catchall_or_prefix) {
    // Regression guard (Codex P1): an earlier match() returned
    // kInvalidRoute the moment tokenize reported segment-count
    // overflow, letting requests with >16 segments bypass a '/'
    // catchall or a matching prefix route. ConnectionBase::
    // kMaxReqPathLen is 64 bytes — enough for 30+ two-byte segments
    // — so the overflow is reachable from a single valid request.
    // match() must walk as deep as the trie has data for and return
    // the longest-match terminal it saw.
    RouteTrie t;
    const Insert items[] = {{"/", 0, 42}, {"/api", 0, 7}};
    REQUIRE(build_ok(t, items, 2));
    // 17 single-char segments — exceeds kMaxPathSegments=16, but
    // starts with /api, so the /api terminal must still win.
    CHECK_EQ(t.match(S("/api/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p"), 0), 7u);
    // All-unknown deep path — catchall '/' must still fire instead
    // of a spurious no-match.
    CHECK_EQ(t.match(S("/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q"), 0), 42u);
}

TEST(route_trie, insert_atomic_on_child_cap_overflow) {
    // Regression guard (Copilot P1 + Codex P2): an earlier insert()
    // could push a new node into the pool and then fail on the
    // subsequent children.push(), leaving a dangling unreferenced
    // node. Repeated overflow attempts used to leak pool capacity
    // until legitimate inserts started failing. This test asserts
    // the pool size stays stable across 100 failed inserts after
    // saturating a parent's child cap.
    //
    // Important: TrieNode::segment is a non-owning Str view into the
    // path buffer the caller provided. Use a 2D array so every
    // inserted path keeps its own backing storage alive for the
    // lifetime of the trie — a single scratch buffer would alias
    // and all children would collapse onto the same segment.
    RouteTrie t;
    static constexpr u32 kN = TrieNode::kMaxChildren;
    // 3-digit decimal encoding (/000../127) — fits kMaxChildren up to
    // 999 and keeps every segment's bytes distinct so find_child can't
    // coincidentally match two of our fill paths onto the same child.
    char paths[kN][8] = {};
    for (u32 i = 0; i < kN; i++) {
        paths[i][0] = '/';
        paths[i][1] = static_cast<char>('0' + (i / 100) % 10);
        paths[i][2] = static_cast<char>('0' + (i / 10) % 10);
        paths[i][3] = static_cast<char>('0' + i % 10);
        REQUIRE(t.insert(Str{paths[i], 4}, 0, static_cast<u16>(i)));
    }
    const u32 saturated_count = t.node_count();
    // Every insert from here on must hit root's child cap. None
    // should grow node_count (what the old insert() used to leak).
    // Use /zXX segments that can't collide with any /NNN above.
    char overflow_paths[100][4] = {};
    for (u32 i = 0; i < 100; i++) {
        overflow_paths[i][0] = '/';
        overflow_paths[i][1] = 'z';
        overflow_paths[i][2] = static_cast<char>('a' + (i % 26));
        CHECK(!t.insert(Str{overflow_paths[i], 3}, 0, 0));
    }
    CHECK_EQ(t.node_count(), saturated_count);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
