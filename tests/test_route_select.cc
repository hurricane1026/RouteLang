// Tests for runtime/route_select.h: dispatch picker.
//
// Each branch of pick_dispatch() has a dedicated test that constructs
// a RouteAnalysis matching that branch's preconditions and asserts
// the picker returns the corresponding canonical singleton. Plus a
// few RouteAnalysis-builder tests for the signal accumulators.

#include "rut/runtime/route_select.h"
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
// RouteAnalysis builder
// ============================================================================

TEST(route_select, analysis_starts_empty) {
    RouteAnalysis a;
    CHECK_EQ(a.count(), 0u);
    CHECK(!a.has_prefix_overlap());
    CHECK(!a.has_param_segments());
    CHECK_EQ(a.max_first_seg_bucket(), 0u);
    CHECK_EQ(a.distinct_first_segments(), 0u);
}

TEST(route_select, note_route_increments_count) {
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/a"), 0));
    REQUIRE(a.note_route(S("/b"), 0));
    CHECK_EQ(a.count(), 2u);
}

TEST(route_select, prefix_overlap_detected_either_direction) {
    // /api inserted first, /api/v1 second — a is prefix of b.
    RouteAnalysis a1;
    REQUIRE(a1.note_route(S("/api"), 0));
    REQUIRE(a1.note_route(S("/api/v1"), 0));
    CHECK(a1.has_prefix_overlap());

    // Reverse insert order — same flag must fire (b's prefix is a).
    RouteAnalysis a2;
    REQUIRE(a2.note_route(S("/api/v1"), 0));
    REQUIRE(a2.note_route(S("/api"), 0));
    CHECK(a2.has_prefix_overlap());
}

TEST(route_select, no_prefix_overlap_for_disjoint_paths) {
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/users"), 0));
    REQUIRE(a.note_route(S("/api/orders"), 0));  // siblings, no overlap
    REQUIRE(a.note_route(S("/admin"), 0));       // different first segment
    REQUIRE(a.note_route(S("/health"), 0));
    CHECK(!a.has_prefix_overlap());
}

TEST(route_select, equal_length_distinct_paths_are_not_overlap) {
    // Same length but different bytes — neither is a prefix of the
    // other. Strict-prefix means a.len < b.len.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/abc"), 0));
    REQUIRE(a.note_route(S("/abd"), 0));
    CHECK(!a.has_prefix_overlap());
}

TEST(route_select, identical_paths_are_not_strict_prefix_overlap) {
    // Two registrations of the same path differ by method only.
    // Neither is a STRICT prefix of the other (strict requires
    // a.len < b.len), so the overlap flag stays false.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/x"), 'G'));
    REQUIRE(a.note_route(S("/x"), 'P'));
    CHECK(!a.has_prefix_overlap());
}

TEST(route_select, param_segments_detected) {
    // Forward-looking: the .rut DSL doesn't emit `:foo` yet, but the
    // picker hooks into has_param_segments so the contract is in
    // place when that lands.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/:id"), 0));
    CHECK(a.has_param_segments());
}

TEST(route_select, param_segments_only_at_segment_start) {
    // ':' inside a segment (e.g., a literal byte that happens to be
    // a colon) doesn't count as a param marker. Format: ':' must be
    // the first byte of its segment.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/foo:bar"), 0));  // colon mid-segment
    CHECK(!a.has_param_segments());
}

TEST(route_select, distinct_first_segments_counted) {
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/v1"), 0));
    REQUIRE(a.note_route(S("/api/v2"), 0));  // shares first seg "api"
    REQUIRE(a.note_route(S("/admin/users"), 0));
    REQUIRE(a.note_route(S("/health"), 0));
    REQUIRE(a.note_route(S("/metrics"), 0));
    CHECK_EQ(a.distinct_first_segments(), 4u);  // api, admin, health, metrics
}

TEST(route_select, max_first_seg_bucket_tracks_clustering) {
    // 5 routes all under /api/* → all hash to the "api" bucket.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/v1/users"), 0));
    REQUIRE(a.note_route(S("/api/v1/orders"), 0));
    REQUIRE(a.note_route(S("/api/v2/users"), 0));
    REQUIRE(a.note_route(S("/api/v2/orders"), 0));
    REQUIRE(a.note_route(S("/api/health"), 0));
    CHECK_EQ(a.max_first_seg_bucket(), 5u);
}

// ============================================================================
// pick_dispatch — one test per selector branch
// ============================================================================

TEST(route_select, picks_segment_trie_when_param_segments_present) {
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/:id"), 0));
    REQUIRE(a.note_route(S("/users/:user_id/posts"), 0));
    CHECK_EQ(pick_dispatch(a), &kSegmentTrieDispatch);
}

TEST(route_select, picks_linear_scan_for_tiny_configs) {
    // ≤16 routes, no params, irrespective of overlap shape.
    RouteAnalysis a;
    for (u32 i = 0; i < 8; i++) {
        char buf[8];
        buf[0] = '/';
        buf[1] = static_cast<char>('a' + i);
        const Str p{buf, 2};
        // note_route stores a non-owning view; we must keep the
        // backing storage alive across the analysis lifetime. For
        // this test we don't outlive the loop scope, so use static
        // strings instead.
        (void)p;
    }
    // Static-storage paths so the views remain valid.
    REQUIRE(a.note_route(S("/a"), 0));
    REQUIRE(a.note_route(S("/b"), 0));
    REQUIRE(a.note_route(S("/c"), 0));
    REQUIRE(a.note_route(S("/d"), 0));
    REQUIRE(a.note_route(S("/e"), 0));
    REQUIRE(a.note_route(S("/f"), 0));
    REQUIRE(a.note_route(S("/g"), 0));
    REQUIRE(a.note_route(S("/h"), 0));
    CHECK_EQ(pick_dispatch(a), &kLinearScanDispatch);
}

TEST(route_select, picks_byte_radix_when_prefix_overlap_no_params) {
    // /api shadows /api/v1 — needs longest-prefix-match. ByteRadix
    // is preferred over SegmentTrie when no params are present
    // (ByteRadix wins on bench).
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api"), 0));
    REQUIRE(a.note_route(S("/api/v1"), 0));
    REQUIRE(a.note_route(S("/api/v1/users"), 0));
    REQUIRE(a.note_route(S("/api/v2"), 0));
    REQUIRE(a.note_route(S("/admin"), 0));
    REQUIRE(a.note_route(S("/admin/users"), 0));
    // 17 routes total to clear the LinearScan cutoff.
    REQUIRE(a.note_route(S("/r1"), 0));
    REQUIRE(a.note_route(S("/r2"), 0));
    REQUIRE(a.note_route(S("/r3"), 0));
    REQUIRE(a.note_route(S("/r4"), 0));
    REQUIRE(a.note_route(S("/r5"), 0));
    REQUIRE(a.note_route(S("/r6"), 0));
    REQUIRE(a.note_route(S("/r7"), 0));
    REQUIRE(a.note_route(S("/r8"), 0));
    REQUIRE(a.note_route(S("/r9"), 0));
    REQUIRE(a.note_route(S("/r10"), 0));
    REQUIRE(a.note_route(S("/r11"), 0));
    CHECK(a.has_prefix_overlap());
    CHECK_EQ(pick_dispatch(a), &kByteRadixDispatch);
}

TEST(route_select, picks_hash_first_segment_when_diverse_segments_no_overlap) {
    // ≥17 routes, no prefix overlap, ≥4 distinct first segments,
    // bucket-fit holds. Should pick HashFirstSegment over HashFullPath.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api/users"), 0));
    REQUIRE(a.note_route(S("/api/orders"), 0));
    REQUIRE(a.note_route(S("/api/products"), 0));
    REQUIRE(a.note_route(S("/admin/users"), 0));
    REQUIRE(a.note_route(S("/admin/audit"), 0));
    REQUIRE(a.note_route(S("/admin/sessions"), 0));
    REQUIRE(a.note_route(S("/oauth/token"), 0));
    REQUIRE(a.note_route(S("/oauth/authorize"), 0));
    REQUIRE(a.note_route(S("/webhooks/stripe"), 0));
    REQUIRE(a.note_route(S("/webhooks/github"), 0));
    REQUIRE(a.note_route(S("/health"), 0));
    REQUIRE(a.note_route(S("/metrics"), 0));
    REQUIRE(a.note_route(S("/_status"), 0));
    REQUIRE(a.note_route(S("/_ready"), 0));
    REQUIRE(a.note_route(S("/_live"), 0));
    REQUIRE(a.note_route(S("/internal/debug"), 0));
    REQUIRE(a.note_route(S("/internal/dump"), 0));
    REQUIRE(!a.has_prefix_overlap());
    REQUIRE(a.distinct_first_segments() >= 4u);
    CHECK_EQ(pick_dispatch(a), &kHashFirstSegmentDispatch);
}

TEST(route_select, picks_hash_full_when_first_segments_concentrated) {
    // ≥17 routes, no prefix overlap, but first segments concentrated
    // — too few distinct first segments for HashFirstSegment to
    // distribute. Picker falls back to HashFullPath.
    RouteAnalysis a;
    // All under /api/v1/<distinct_resource> — first segment is "api"
    // for every route, so distinct_first_segments == 1. Paths don't
    // overlap because each has a different last segment.
    const char* paths[] = {
        "/api/v1/users",
        "/api/v1/orders",
        "/api/v1/products",
        "/api/v1/sessions",
        "/api/v1/events",
        "/api/v1/teams",
        "/api/v1/projects",
        "/api/v1/tasks",
        "/api/v1/comments",
        "/api/v1/tags",
        "/api/v1/files",
        "/api/v1/messages",
        "/api/v1/channels",
        "/api/v1/integrations",
        "/api/v1/policies",
        "/api/v1/notifications",
        "/api/v1/customers",
    };
    for (u32 i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        REQUIRE(a.note_route(S(paths[i]), 0));
    }
    REQUIRE(!a.has_prefix_overlap());
    REQUIRE_EQ(a.distinct_first_segments(), 1u);
    CHECK_EQ(pick_dispatch(a), &kHashFullPathDispatch);
}

TEST(route_select, falls_back_to_hash_full_on_first_seg_bucket_overflow) {
    // Synthetic case where first-segment hashing would overflow
    // HashFirstSegment's per-bucket cap. We force this by making
    // many routes share the same first segment with disjoint tails
    // — also engineered to NOT prefix-overlap each other (no
    // shorter route is a prefix of a longer one).
    RouteAnalysis a;
    // 20 paths under a single "/api" first segment, distinct enough
    // suffixes that no path is a strict prefix of another.
    const char* paths[] = {
        "/api/aa", "/api/ab", "/api/ac", "/api/ad", "/api/ae", "/api/af", "/api/ag",
        "/api/ah", "/api/ai", "/api/aj", "/api/ak", "/api/al", "/api/am", "/api/an",
        "/api/ao", "/api/ap", "/api/aq", "/api/ar", "/api/as", "/api/at",
    };
    for (u32 i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        REQUIRE(a.note_route(S(paths[i]), 0));
    }
    REQUIRE(!a.has_prefix_overlap());
    // 20 routes in one bucket > kPerBucket (16). Picker must reject
    // HashFirstSegment.
    REQUIRE(a.max_first_seg_bucket() > HashFirstSegmentTable::kPerBucket);
    CHECK_EQ(pick_dispatch(a), &kHashFullPathDispatch);
}

// ============================================================================
// Picker stability — never returns nullptr or an unknown pointer
// ============================================================================

TEST(route_select, picker_returns_canonical_singleton_for_empty_analysis) {
    RouteAnalysis a;
    const RouteDispatch* d = pick_dispatch(a);
    // Empty analysis: count == 0 ≤ 16, so LinearScan.
    CHECK_EQ(d, &kLinearScanDispatch);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
