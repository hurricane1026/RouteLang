// NEON LinearScan dispatch — same first-match-wins byte-prefix
// semantics as kLinearScanDispatch, with the per-route byte
// compare done in 16-byte SIMD chunks via vceqq_u8 / vminvq_u8.
//
// NEON is mandatory on every ARMv8.0+ CPU (all Graviton, Apple
// Silicon, Ampere Altra, Raspberry Pi 4+) — the `has_neon` cap is
// effectively always true on aarch64 Linux, so this dispatch is
// the default ARM SIMD path. (SVE is left for a follow-up PR;
// Graviton 3 has 256-bit SVE, Graviton 4 has 128-bit SVE2 — the
// width inconsistency adds picker complexity not justified yet.)
//
// Build: always compiled. ARMv8 baseline includes NEON so no extra
// compile flag is needed. Non-aarch64 hosts compile as a delegating
// stub.

#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_table.h"

#if defined(__aarch64__)
#include <arm_neon.h>

namespace rut {

namespace {

u16 simd_ls_neon_match(const RouteConfig* cfg, Str path, u8 method) {
    for (u32 i = 0; i < cfg->route_count; i++) {
        const auto& r = cfg->routes[i];
        if (r.method != 0 && r.method != method) continue;
        if (path.len < r.path_len) continue;

        u32 j = 0;
        bool matched = true;
        // 16-byte NEON chunks. vminvq_u8 on the equality vector
        // returns 0 if any lane is 0 (mismatch), 0xFF if all match.
        const u32 chunks = r.path_len / 16;
        for (u32 c = 0; c < chunks; c++, j += 16) {
            const uint8x16_t a =
                vld1q_u8(reinterpret_cast<const u8*>(path.ptr + j));
            const uint8x16_t b = vld1q_u8(reinterpret_cast<const u8*>(r.path + j));
            const uint8x16_t eq = vceqq_u8(a, b);
            if (vminvq_u8(eq) != 0xff) {
                matched = false;
                break;
            }
        }
        if (matched) {
            for (; j < r.path_len; j++) {
                if (path.ptr[j] != r.path[j]) {
                    matched = false;
                    break;
                }
            }
        }
        if (matched) return static_cast<u16>(i);
    }
    return kRouteIdxInvalid;
}

}  // namespace

const RouteDispatch kSimdLsNeonDispatch = {&simd_ls_neon_match};

}  // namespace rut

#else  // !aarch64 — stub

namespace rut {

namespace {

u16 simd_ls_neon_stub_match(const RouteConfig* cfg, Str path, u8 method) {
    return kLinearScanDispatch.match(cfg, path, method);
}

}  // namespace

const RouteDispatch kSimdLsNeonDispatch = {&simd_ls_neon_stub_match};

}  // namespace rut

#endif
