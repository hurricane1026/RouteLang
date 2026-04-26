// SSE2 LinearScan dispatch — same first-match-wins byte-prefix
// semantics as the scalar kLinearScanDispatch, with the per-route
// byte compare done in 16-byte SIMD chunks.
//
// Build: this file is always compiled (CMake always lists it). On
// x86_64 hosts CMake adds `-msse2` so __SSE2__ is defined and the
// real impl is selected. On non-x86_64 hosts (e.g. ARM CI) the file
// compiles as a stub that delegates to the scalar dispatch — kept
// identifiable so the canonical-singleton whitelist contract holds
// regardless of build target. The picker never selects an
// unsupported dispatch (CpuCaps::has_sse2 gates it), so the stub is
// unreachable in well-formed binaries but exists for safety.

#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_table.h"

#if defined(__x86_64__) && defined(__SSE2__)
#include <emmintrin.h>

namespace rut {

namespace {

u16 simd_ls_sse2_match(const RouteConfig* cfg, Str path, u8 method) {
    for (u32 i = 0; i < cfg->route_count; i++) {
        const auto& r = cfg->routes[i];
        if (r.method != 0 && r.method != method) continue;
        if (path.len < r.path_len) continue;

        // 16-byte SIMD chunks. The route's path[] is fixed-size
        // (kMaxPathLen ≥ 16) so a 16-byte load from offset 0 is
        // always in-bounds. The request's len was already filtered
        // to be ≥ r.path_len, so reading r.path_len bytes from
        // path.ptr is safe; we read in 16-byte chunks while
        // ≥ 16 bytes remain.
        u32 j = 0;
        bool matched = true;
        const u32 chunks = r.path_len / 16;
        for (u32 c = 0; c < chunks; c++, j += 16) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(path.ptr + j));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.path + j));
            const __m128i eq = _mm_cmpeq_epi8(a, b);
            const i32 mask = _mm_movemask_epi8(eq);
            if (mask != 0xffff) {
                matched = false;
                break;
            }
        }
        if (matched) {
            // Scalar tail for the last <16 bytes (route_len % 16).
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

const RouteDispatch kSimdLsSse2Dispatch = {&simd_ls_sse2_match};

}  // namespace rut

#else  // !x86_64 || !SSE2 — stub that delegates to scalar

namespace rut {

namespace {

u16 simd_ls_sse2_stub_match(const RouteConfig* cfg, Str path, u8 method) {
    // Unreachable in well-formed binaries (picker won't select this
    // dispatch when CpuCaps::has_sse2 is false). If it does happen,
    // fall through to the scalar dispatch so behaviour stays correct.
    return kLinearScanDispatch.match(cfg, path, method);
}

}  // namespace

const RouteDispatch kSimdLsSse2Dispatch = {&simd_ls_sse2_stub_match};

}  // namespace rut

#endif
