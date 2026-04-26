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
    // String-literal paths via S() have static storage duration, so
    // the non-owning Str views stored by note_route remain valid for
    // the whole RouteAnalysis lifetime here.
    RouteAnalysis a;
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
    // is preferred over SegmentTrie when no params are present and
    // overlaps are segment-aligned (ByteRadix wins on bench).
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api"), 0));
    REQUIRE(a.note_route(S("/api/v1"), 0));
    REQUIRE(a.note_route(S("/api/v1/users"), 0));
    REQUIRE(a.note_route(S("/api/v2"), 0));
    REQUIRE(a.note_route(S("/admin"), 0));
    REQUIRE(a.note_route(S("/admin/users"), 0));
    // Pad past the LinearScan cutoff. Padding paths are chosen so no
    // pair has a boundary-sensitive overlap (Copilot's signal would
    // route us to SegmentTrie otherwise) — every path is two
    // segments and pairwise non-prefix.
    REQUIRE(a.note_route(S("/h/a"), 0));
    REQUIRE(a.note_route(S("/h/b"), 0));
    REQUIRE(a.note_route(S("/h/c"), 0));
    REQUIRE(a.note_route(S("/h/d"), 0));
    REQUIRE(a.note_route(S("/h/e"), 0));
    REQUIRE(a.note_route(S("/h/f"), 0));
    REQUIRE(a.note_route(S("/h/g"), 0));
    REQUIRE(a.note_route(S("/h/h"), 0));
    REQUIRE(a.note_route(S("/h/i"), 0));
    REQUIRE(a.note_route(S("/h/j"), 0));
    REQUIRE(a.note_route(S("/h/k"), 0));
    CHECK(a.has_prefix_overlap());
    CHECK(!a.has_segment_boundary_sensitive_overlap());
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

TEST(route_select, picks_segment_trie_when_first_segments_concentrated) {
    // ≥17 routes, no prefix overlap, but first segments concentrated
    // — too few distinct first segments for HashFirstSegment to
    // distribute. Picker falls back to SegmentTrie. NOT HashFullPath:
    // HashFullPath is exact-match-only and would silently turn
    // /api/v1/users matching request /api/v1/users/42 into a miss
    // (Codex P1 on #47 round 1). SegmentTrie preserves prefix
    // semantics generically.
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
    CHECK_EQ(pick_dispatch(a), &kSegmentTrieDispatch);
}

TEST(route_select, falls_back_to_segment_trie_on_first_seg_bucket_overflow) {
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
    // HashFirstSegment and fall through to SegmentTrie (NOT
    // HashFullPath, which would silently break prefix routing).
    REQUIRE(a.max_first_seg_bucket() > HashFirstSegmentTable::kPerBucket);
    CHECK_EQ(pick_dispatch(a), &kSegmentTrieDispatch);
}

TEST(route_select, picks_segment_trie_for_boundary_sensitive_overlap) {
    // /api and /apix registered together — /api is a strict byte
    // prefix of /apix but the next byte ('x') is not '/'. ByteRadix's
    // byte-prefix view would mis-route an intermediate request like
    // /apij to /api; SegmentTrie treats /apij as a miss (segment
    // "apij" matches neither registered segment). The picker steers
    // boundary-sensitive configs to SegmentTrie. (Copilot on #47.)
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api"), 0));
    REQUIRE(a.note_route(S("/apix"), 0));
    // Pad past the LinearScan cutoff so the boundary branch is the
    // one being exercised, not the small-config branch.
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
    REQUIRE(a.note_route(S("/r12"), 0));
    REQUIRE(a.note_route(S("/r13"), 0));
    REQUIRE(a.note_route(S("/r14"), 0));
    REQUIRE(a.note_route(S("/r15"), 0));
    REQUIRE(a.has_prefix_overlap());
    REQUIRE(a.has_segment_boundary_sensitive_overlap());
    CHECK_EQ(pick_dispatch(a), &kSegmentTrieDispatch);
}

TEST(route_select, segment_aligned_overlap_does_not_set_boundary_sensitive_flag) {
    // /api and /api/v1 — /api is a strict prefix and the continuation
    // byte in /api/v1 IS '/'. This is segment-aligned overlap; the
    // boundary-sensitive flag must stay false (so the picker can still
    // pick ByteRadix, which is faster than SegmentTrie when correct).
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api"), 0));
    REQUIRE(a.note_route(S("/api/v1"), 0));
    CHECK(a.has_prefix_overlap());
    CHECK(!a.has_segment_boundary_sensitive_overlap());
}

TEST(route_select, boundary_sensitive_flag_detected_either_insertion_order) {
    // Reverse of picks_segment_trie_for_boundary_sensitive_overlap:
    // insert /apix first, then /api. The longer path was already in
    // paths_, so the scan sees the new (shorter) path strict-prefixes
    // an existing path. Same flag must fire.
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/apix"), 0));
    REQUIRE(a.note_route(S("/api"), 0));
    CHECK(a.has_segment_boundary_sensitive_overlap());
}

// ============================================================================
// ART branch — high first-byte fan-out
// ============================================================================

TEST(route_select, distinct_first_bytes_strips_leading_slash) {
    RouteAnalysis a;
    REQUIRE(a.note_route(S("/api"), 0));
    REQUIRE(a.note_route(S("/admin"), 0));
    REQUIRE(a.note_route(S("/oauth"), 0));
    // First bytes after '/' are 'a', 'a', 'o' → 2 distinct.
    CHECK_EQ(a.distinct_first_bytes(), 2u);
}

TEST(route_select, distinct_first_bytes_counts_all_registered_bytes) {
    RouteAnalysis a;
    const char* paths[] = {"/a", "/b", "/c", "/d", "/e", "/f"};
    for (u32 i = 0; i < 6; i++) REQUIRE(a.note_route(S(paths[i]), 0));
    CHECK_EQ(a.distinct_first_bytes(), 6u);
}

TEST(route_select, picks_art_when_prefix_overlap_with_high_first_byte_fanout) {
    // 18 distinct first bytes (≥ kArtFanoutThreshold = 17), plus
    // /<byte>/x deeper paths so prefix overlap is set. Picker must
    // route this to ART instead of ByteRadix because Node48's
    // byte-indexed lookup wins decisively at this fan-out.
    RouteAnalysis a;
    const char* roots[] = {"/a",
                           "/b",
                           "/c",
                           "/d",
                           "/e",
                           "/f",
                           "/g",
                           "/h",
                           "/i",
                           "/j",
                           "/k",
                           "/l",
                           "/m",
                           "/n",
                           "/o",
                           "/p",
                           "/q",
                           "/r"};
    const char* deeps[] = {"/a/x",
                           "/b/x",
                           "/c/x",
                           "/d/x",
                           "/e/x",
                           "/f/x",
                           "/g/x",
                           "/h/x",
                           "/i/x",
                           "/j/x",
                           "/k/x",
                           "/l/x",
                           "/m/x",
                           "/n/x",
                           "/o/x",
                           "/p/x",
                           "/q/x",
                           "/r/x"};
    for (u32 i = 0; i < 18; i++) REQUIRE(a.note_route(S(roots[i]), 0));
    for (u32 i = 0; i < 18; i++) REQUIRE(a.note_route(S(deeps[i]), 0));
    REQUIRE(a.has_prefix_overlap());
    REQUIRE(!a.has_segment_boundary_sensitive_overlap());
    REQUIRE_EQ(a.distinct_first_bytes(), 18u);
    CHECK_EQ(pick_dispatch(a), &kArtDispatch);
}

TEST(route_select, picks_byte_radix_when_prefix_overlap_with_low_first_byte_fanout) {
    // 16 distinct first bytes (just below the ART threshold of 17).
    // Bench shows ByteRadix's homogeneous loop beats ART's
    // polymorphic descent in this regime — picker must NOT pick ART.
    RouteAnalysis a;
    const char* roots[] = {"/a",
                           "/b",
                           "/c",
                           "/d",
                           "/e",
                           "/f",
                           "/g",
                           "/h",
                           "/i",
                           "/j",
                           "/k",
                           "/l",
                           "/m",
                           "/n",
                           "/o",
                           "/p"};
    const char* deeps[] = {"/a/x",
                           "/b/x",
                           "/c/x",
                           "/d/x",
                           "/e/x",
                           "/f/x",
                           "/g/x",
                           "/h/x",
                           "/i/x",
                           "/j/x",
                           "/k/x",
                           "/l/x",
                           "/m/x",
                           "/n/x",
                           "/o/x",
                           "/p/x"};
    for (u32 i = 0; i < 16; i++) REQUIRE(a.note_route(S(roots[i]), 0));
    for (u32 i = 0; i < 16; i++) REQUIRE(a.note_route(S(deeps[i]), 0));
    REQUIRE(a.has_prefix_overlap());
    REQUIRE_EQ(a.distinct_first_bytes(), 16u);
    CHECK_EQ(pick_dispatch(a), &kByteRadixDispatch);
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
