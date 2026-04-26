// AVX2 LinearScan dispatch — same first-match-wins byte-prefix
// semantics as kLinearScanDispatch, with the per-route byte
// compare done in 32-byte SIMD chunks. Wider chunks than the SSE2
// variant means fewer iterations for medium-length paths.
//
// Build: file always compiled. CMake adds `-mavx2` on x86_64 hosts.
// On non-x86_64 hosts compiles as a delegating stub.

#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_table.h"

#if defined(__x86_64__) && defined(__AVX2__)
#include <immintrin.h>

namespace rut {

namespace {

u16 simd_ls_avx2_match(const RouteConfig* cfg, Str path, u8 method) {
    for (u32 i = 0; i < cfg->route_count; i++) {
        const auto& r = cfg->routes[i];
        if (r.method != 0 && r.method != method) continue;
        if (path.len < r.path_len) continue;

        u32 j = 0;
        bool matched = true;
        // 32-byte AVX2 chunks first.
        const u32 avx_chunks = r.path_len / 32;
        for (u32 c = 0; c < avx_chunks; c++, j += 32) {
            const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(path.ptr + j));
            const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(r.path + j));
            const __m256i eq = _mm256_cmpeq_epi8(a, b);
            const i32 mask = _mm256_movemask_epi8(eq);
            if (mask != static_cast<i32>(0xffffffff)) {
                matched = false;
                break;
            }
        }
        // 16-byte SSE2 chunk if route_len % 32 ≥ 16.
        if (matched && (r.path_len - j) >= 16) {
            const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(path.ptr + j));
            const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.path + j));
            const __m128i eq = _mm_cmpeq_epi8(a, b);
            const i32 mask = _mm_movemask_epi8(eq);
            if (mask != 0xffff) {
                matched = false;
            } else {
                j += 16;
            }
        }
        // Scalar tail for the last <16 bytes.
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

const RouteDispatch kSimdLsAvx2Dispatch = {&simd_ls_avx2_match};

}  // namespace rut

#else  // !x86_64 || !AVX2 — stub

namespace rut {

namespace {

u16 simd_ls_avx2_stub_match(const RouteConfig* cfg, Str path, u8 method) {
    return kLinearScanDispatch.match(cfg, path, method);
}

}  // namespace

const RouteDispatch kSimdLsAvx2Dispatch = {&simd_ls_avx2_stub_match};

}  // namespace rut

#endif
