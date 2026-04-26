// AVX-512 LinearScan dispatch — same first-match-wins byte-prefix
// semantics as kLinearScanDispatch. Uses 64-byte VPCMPEQB plus the
// mask-register-aware load (`_mm512_mask_loadu_epi8` / mask-and-
// compare) so a single instruction handles arbitrary route lengths
// 1..64 without a scalar tail loop. For longer routes we chunk in
// 64-byte strides.
//
// AVX-512BW is required for the byte-granularity ops (VPCMPEQB on
// __mmask64). The picker only selects this dispatch when CpuCaps
// reports both has_avx512f AND has_avx512bw.
//
// Build: always compiled. CMake adds `-mavx512f -mavx512bw` on
// x86_64. Non-x86_64 hosts compile as a delegating stub.

#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_table.h"

#if defined(__x86_64__) && defined(__AVX512F__) && defined(__AVX512BW__)
#include <immintrin.h>

namespace rut {

namespace {

u16 simd_ls_avx512_match(const RouteConfig* cfg, Str path, u8 method) {
    for (u32 i = 0; i < cfg->route_count; i++) {
        const auto& r = cfg->routes[i];
        if (r.method != 0 && r.method != method) continue;
        if (path.len < r.path_len) continue;

        u32 j = 0;
        bool matched = true;
        // 64-byte AVX-512 chunks first.
        while (matched && (r.path_len - j) >= 64) {
            const __m512i a = _mm512_loadu_si512(reinterpret_cast<const void*>(path.ptr + j));
            const __m512i b = _mm512_loadu_si512(reinterpret_cast<const void*>(r.path + j));
            const __mmask64 eq = _mm512_cmpeq_epi8_mask(a, b);
            if (eq != ~__mmask64{0}) matched = false;
            else j += 64;
        }
        // Tail (1-63 bytes): use a length mask to confine the
        // compare to bytes [0, route_len - j). This is the AVX-512
        // perk — no scalar tail loop, the masked compare handles
        // arbitrary length-mod-64 in a single op.
        if (matched && j < r.path_len) {
            const u32 tail = r.path_len - j;
            const __mmask64 lm = (tail >= 64) ? ~__mmask64{0} : ((__mmask64{1} << tail) - 1);
            const __m512i a = _mm512_maskz_loadu_epi8(lm, path.ptr + j);
            const __m512i b = _mm512_maskz_loadu_epi8(lm, r.path + j);
            const __mmask64 eq = _mm512_cmpeq_epi8_mask(a, b);
            if ((eq & lm) != lm) matched = false;
        }
        if (matched) return static_cast<u16>(i);
    }
    return kRouteIdxInvalid;
}

}  // namespace

const RouteDispatch kSimdLsAvx512Dispatch = {&simd_ls_avx512_match};

}  // namespace rut

#else  // !x86_64 || !AVX-512 — stub

namespace rut {

namespace {

u16 simd_ls_avx512_stub_match(const RouteConfig* cfg, Str path, u8 method) {
    return kLinearScanDispatch.match(cfg, path, method);
}

}  // namespace

const RouteDispatch kSimdLsAvx512Dispatch = {&simd_ls_avx512_stub_match};

}  // namespace rut

#endif
