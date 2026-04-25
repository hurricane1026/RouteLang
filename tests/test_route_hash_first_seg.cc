// Tests for runtime/route_hash_first_seg.h: bucketed dispatch.
//
// Coverage:
//   - basic hit / miss in bucket
//   - byte-prefix matching inside bucket (preserves linear-scan semantics)
//   - method-slot routing (specific beats any, any-fallback)
//   - catchall ("/" route) reachable for any request via the empty-
//     segment bucket fallback
//   - per-bucket capacity enforcement
//   - first-match-wins within a bucket (insert order preserved)

#include "rut/runtime/route_hash_first_seg.h"
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

TEST(route_hash_first_seg, exact_match_single_route) {
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    CHECK_EQ(t.match(S("/health"), 0), 7u);
}

TEST(route_hash_first_seg, no_match_when_no_routes) {
    HashFirstSegmentTable t;
    CHECK_EQ(t.match(S("/anything"), 0), kRouteIdxInvalid);
}

TEST(route_hash_first_seg, byte_prefix_match_within_bucket) {
    // Routes /api/v1 and /api/v2 share first segment "api". A request
    // /api/v1/users hits /api/v1 by byte-prefix; /api/v3/x hits
    // neither.
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/api/v1"), 0, 1));
    REQUIRE(t.insert(S("/api/v2"), 0, 2));
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 1u);
    CHECK_EQ(t.match(S("/api/v2"), 0), 2u);
    CHECK_EQ(t.match(S("/api/v3/x"), 0), kRouteIdxInvalid);
}

// ============================================================================
// Catchall reachability (the critical thing the empty-segment bucket
// fallback gives us — without it, hashed dispatch would silently break
// configs that depend on a "/" route)
// ============================================================================

TEST(route_hash_first_seg, catchall_root_matches_any_request) {
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/"), 0, 99));
    REQUIRE(t.insert(S("/health"), 0, 7));
    // Specific route still wins on its own bucket.
    CHECK_EQ(t.match(S("/health"), 0), 7u);
    // Unmatched first-segment falls back to the catchall bucket.
    CHECK_EQ(t.match(S("/missing"), 0), 99u);
    CHECK_EQ(t.match(S("/random/deep/path"), 0), 99u);
    // The root path itself hits the catchall.
    CHECK_EQ(t.match(S("/"), 0), 99u);
}

TEST(route_hash_first_seg, catchall_only_config_serves_everything) {
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/"), 0, 42));
    CHECK_EQ(t.match(S("/api/v1/users"), 0), 42u);
    CHECK_EQ(t.match(S("/admin"), 0), 42u);
    CHECK_EQ(t.match(S("/"), 0), 42u);
}

TEST(route_hash_first_seg, no_catchall_fallback_when_request_is_root) {
    // When the request itself has an empty first segment, we don't
    // double-probe (would just rescan the same bucket). A request "/"
    // with no "/" route registered must miss cleanly.
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/api"), 0, 1));
    CHECK_EQ(t.match(S("/"), 0), kRouteIdxInvalid);
    CHECK_EQ(t.match(S(""), 0), kRouteIdxInvalid);
}

// ============================================================================
// Method dispatch
// ============================================================================

TEST(route_hash_first_seg, method_specific_beats_any_within_bucket) {
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/x"), 0, 10));
    REQUIRE(t.insert(S("/x"), 'G', 20));
    // Insert order: /x ANY first, /x GET second. The bucket's linear
    // scan sees the ANY entry first — so GET requests would hit ANY
    // before reaching the specific entry. To preserve "specific wins"
    // semantics in this dispatch the SELECTOR is responsible for
    // ordering: emit method-specific routes before their any-method
    // counterpart at the same path. Here we exercise the documented
    // bucket-scan contract directly: insert order = match order.
    //
    // (When the selector lands, its tests will pin the higher-level
    // "specific beats any" guarantee end-to-end. Within this dispatch,
    // first-match-wins is the contract.)
    CHECK_EQ(t.match(S("/x"), 'G'), 10u);  // ANY entry hits first
    CHECK_EQ(t.match(S("/x"), 'P'), 10u);  // ANY matches POST too
    CHECK_EQ(t.match(S("/x"), 0), 10u);
}

TEST(route_hash_first_seg, method_specific_first_then_any) {
    // Reverse order: GET-specific first, then ANY. GET requests hit
    // the specific entry first; non-GET methods skip past it and
    // hit the ANY entry.
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/x"), 'G', 20));
    REQUIRE(t.insert(S("/x"), 0, 10));
    CHECK_EQ(t.match(S("/x"), 'G'), 20u);
    CHECK_EQ(t.match(S("/x"), 'P'), 10u);
}

// ============================================================================
// Capacity / build-time guards
// ============================================================================

TEST(route_hash_first_seg, rejects_when_per_bucket_exceeds_cap) {
    // All routes share first segment "x", forcing them into one
    // bucket. The first kPerBucket inserts succeed; the next one
    // must fail so the selector's wrong-pick stays loud.
    HashFirstSegmentTable t;
    constexpr u32 kN = HashFirstSegmentTable::kPerBucket;
    char paths[kN + 1][8];
    for (u32 i = 0; i <= kN; i++) {
        paths[i][0] = '/';
        paths[i][1] = 'x';
        paths[i][2] = '/';
        paths[i][3] = static_cast<char>('A' + i);
        paths[i][4] = '\0';
    }
    for (u32 i = 0; i < kN; i++) {
        CHECK(t.insert(Str{paths[i], 4}, 0, static_cast<u16>(i)));
    }
    CHECK(!t.insert(Str{paths[kN], 4}, 0, static_cast<u16>(kN)));
}

TEST(route_hash_first_seg, distinct_first_segments_distribute_well) {
    // 10 routes with 10 different first segments. Each lands in a
    // different bucket on average; scan_bucket sees only 1 entry per
    // hit, so this is the shape the dispatch is designed for.
    HashFirstSegmentTable t;
    const char* paths[] = {
        "/svc-a/x",
        "/svc-b/x",
        "/svc-c/x",
        "/admin/x",
        "/oauth/x",
        "/health",
        "/metrics",
        "/_status",
        "/_ready",
        "/api/v1/x",
    };
    for (u32 i = 0; i < 10; i++) {
        Str p = S(paths[i]);
        REQUIRE(t.insert(p, 0, static_cast<u16>(i)));
    }
    for (u32 i = 0; i < 10; i++) {
        Str p = S(paths[i]);
        CHECK_EQ(t.match(p, 0), static_cast<u16>(i));
    }
}

TEST(route_hash_first_seg, first_segment_stops_at_query_and_fragment) {
    // Codex P2 on #45: requests arrive with raw query / fragment bytes.
    // first_segment must stop at '?' / '#' so /health?check=1 buckets
    // the same as /health, otherwise registered routes would miss.
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/health"), 0, 7));
    REQUIRE(t.insert(S("/api/users"), 0, 12));
    CHECK_EQ(t.match(S("/health?check=1"), 0), 7u);
    CHECK_EQ(t.match(S("/health?"), 0), 7u);
    CHECK_EQ(t.match(S("/health#frag"), 0), 7u);
    CHECK_EQ(t.match(S("/api/users?page=3"), 0), 12u);
}

TEST(route_hash_first_seg, clear_resets_state) {
    HashFirstSegmentTable t;
    REQUIRE(t.insert(S("/a/x"), 0, 1));
    REQUIRE(t.insert(S("/b/x"), 0, 2));
    CHECK_EQ(t.size(), 2u);
    t.clear();
    CHECK_EQ(t.size(), 0u);
    CHECK_EQ(t.match(S("/a/x"), 0), kRouteIdxInvalid);
    REQUIRE(t.insert(S("/c/x"), 0, 3));
    CHECK_EQ(t.match(S("/c/x"), 0), 3u);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
