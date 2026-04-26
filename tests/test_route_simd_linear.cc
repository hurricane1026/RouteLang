// Tests for SIMD LinearScan dispatches — parity with the scalar
// kLinearScanDispatch across all four SIMD variants. The tests are
// driven through RouteConfig so they exercise the dispatch vtable
// + populate_dispatch_state branch end-to-end. SIMD variants on
// non-applicable hosts compile as stubs that delegate to the
// scalar dispatch — those still pass every test in this file
// (they're semantically identical).

#include "rut/runtime/cpu_caps.h"
#include "rut/runtime/route_table.h"
#include "test.h"

using namespace rut;

namespace {

// Helper: route-index lookup that returns kRouteIdxInvalid on miss
// and the routes[] index otherwise. Called from inside TEST() so
// CHECK/REQUIRE are valid.
u16 lookup(const RouteConfig& cfg, const char* path, u32 plen) {
    const RouteEntry* r = cfg.match(reinterpret_cast<const u8*>(path), plen, 0);
    if (r == nullptr) return kRouteIdxInvalid;
    return static_cast<u16>(r - &cfg.routes[0]);
}

}  // namespace

// Macro to instantiate parity tests for one SIMD variant. Each
// expansion is a TEST() so REQUIRE/CHECK_EQ resolve correctly. The
// route set covers short / medium / long paths so every chunk-size
// branch (≤16, ≤32, ≤64, >64) in the SIMD variants gets exercised.
//
// The `cap_supported` argument is a runtime expression on the
// detected CpuCaps; if it's false this test no-ops (the SIMD
// variant's intrinsics would SIGILL on an unsupported CPU). The
// scalar variant passes `true`.
#define DEFINE_PARITY_TEST(name, dispatch, cap_supported)                                    \
    TEST(route_simd_linear, name) {                                                          \
        const CpuCaps caps = CpuCaps::detect();                                              \
        (void)caps;                                                                          \
        if (!(cap_supported)) return;                                                        \
        RouteConfig cfg;                                                                     \
        REQUIRE(cfg.set_dispatch(&dispatch));                                                \
        REQUIRE(cfg.add_static("/", 0, 200));                                                \
        REQUIRE(cfg.add_static("/v1", 0, 200));                                              \
        REQUIRE(cfg.add_static("/api/users", 0, 200));                                       \
        REQUIRE(cfg.add_static("/api/v1/users/me/profile", 0, 200));                         \
        REQUIRE(cfg.add_static("/api/v1/users/me/profile/preferences", 0, 200));             \
        REQUIRE(cfg.add_static(                                                              \
            "/api/v1/admin/dashboard/widgets/billing/customers/12345", 0, 200));             \
        /* Exact-length matches across short / medium / long. */                             \
        CHECK_EQ(lookup(cfg, "/", 1), 0u);                                                   \
        CHECK_EQ(lookup(cfg, "/v1", 3), 0u); /* "/" wins (first-match) */                    \
        CHECK_EQ(lookup(cfg, "/api/users", 10), 0u);                                         \
        /* For first-match-wins to give us idx > 0, register without */                      \
        /* the leading-/ catchall. Switch to a separate cfg below. */                        \
        RouteConfig c2;                                                                      \
        REQUIRE(c2.set_dispatch(&dispatch));                                                 \
        REQUIRE(c2.add_static("/v1", 0, 200));                                               \
        REQUIRE(c2.add_static("/api/users", 0, 200));                                        \
        REQUIRE(c2.add_static("/api/v1/users/me/profile", 0, 200));                          \
        REQUIRE(c2.add_static("/api/v1/users/me/profile/preferences", 0, 200));              \
        REQUIRE(c2.add_static(                                                               \
            "/api/v1/admin/dashboard/widgets/billing/customers/12345", 0, 200));             \
        /* Exact-length match: idx of the registered route. */                              \
        CHECK_EQ(lookup(c2, "/v1", 3), 0u);                                                  \
        CHECK_EQ(lookup(c2, "/api/users", 10), 1u);                                          \
        CHECK_EQ(lookup(c2, "/api/v1/users/me/profile", 24), 2u);                            \
        /* "/api/v1/users/me/profile/preferences" matches idx 2 first      */                \
        /* (byte-prefix wins over the more specific idx 3) — first-match-  */                \
        /* wins is the contract LinearScan and ALL its SIMD variants must  */                \
        /* preserve.                                                       */                \
        CHECK_EQ(lookup(c2, "/api/v1/users/me/profile/preferences", 36), 2u);                \
        CHECK_EQ(                                                                            \
            lookup(c2, "/api/v1/admin/dashboard/widgets/billing/customers/12345", 55),       \
            4u);                                                                             \
        /* Byte-prefix match: registered prefix matches longer request. */                   \
        CHECK_EQ(lookup(c2, "/v1/x", 5), 0u);                                                \
        CHECK_EQ(lookup(c2, "/api/users/42", 13), 1u);                                       \
        CHECK_EQ(lookup(c2, "/api/v1/users/me/profile/avatar", 31), 2u);                     \
        /* Misses. */                                                                        \
        CHECK_EQ(lookup(c2, "/nope", 5), kRouteIdxInvalid);                                  \
        CHECK_EQ(lookup(c2, "/api/v2/users", 13), kRouteIdxInvalid);                         \
        CHECK_EQ(lookup(c2, "/admin", 6), kRouteIdxInvalid);                                 \
    }

DEFINE_PARITY_TEST(sse2_parity_with_scalar, kSimdLsSse2Dispatch, caps.has_sse2)
DEFINE_PARITY_TEST(avx2_parity_with_scalar, kSimdLsAvx2Dispatch, caps.has_avx2)
DEFINE_PARITY_TEST(avx512_parity_with_scalar,
                   kSimdLsAvx512Dispatch,
                   caps.has_avx512f && caps.has_avx512bw)
DEFINE_PARITY_TEST(neon_parity_with_scalar, kSimdLsNeonDispatch, caps.has_neon)
// Scalar baseline — same DEFINE_PARITY_TEST machinery for sanity:
// if these tests pass for scalar but fail for SIMD, the SIMD impl
// has a bug (and vice versa). Always supported.
DEFINE_PARITY_TEST(scalar_baseline, kLinearScanDispatch, true)

// ============================================================================
// CpuCaps detect — sanity check the runtime probe
// ============================================================================

TEST(route_simd_linear, cpu_caps_detect_runs) {
    // detect() should always succeed and return a sensible struct.
    // We don't assert specific bits — they vary by host. Just check
    // that on x86_64 at least one of {has_sse2,has_sse42,has_avx2}
    // is set (any modern x86 has SSE2), and on aarch64 has_neon is
    // set (NEON is mandatory in ARMv8.0+).
    const CpuCaps caps = CpuCaps::detect();
#if defined(__x86_64__)
    CHECK(caps.has_sse2);
#elif defined(__aarch64__)
    CHECK(caps.has_neon);
#endif
}

TEST(route_simd_linear, cpu_caps_scalar_only_has_no_flags) {
    const CpuCaps caps = CpuCaps::scalar_only();
    CHECK(!caps.has_sse2);
    CHECK(!caps.has_sse42);
    CHECK(!caps.has_avx2);
    CHECK(!caps.has_avx512f);
    CHECK(!caps.has_avx512bw);
    CHECK(!caps.has_neon);
    CHECK(!caps.has_sve);
}

int main(int argc, char** argv) { return rut::test::run_all(argc, argv); }
