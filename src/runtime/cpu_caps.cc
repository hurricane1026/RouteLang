#include "rut/runtime/cpu_caps.h"

#if defined(__x86_64__)
#include <cpuid.h>
#elif defined(__aarch64__)
#include <sys/auxv.h>
#ifndef HWCAP_ASIMD
#define HWCAP_ASIMD (1 << 1)  // ARM64 NEON / Advanced SIMD
#endif
#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)  // ARM64 SVE
#endif
#endif

namespace rut {

CpuCaps CpuCaps::detect() {
    CpuCaps c{};
#if defined(__x86_64__)
    // CPUID leaf 1: SSE2 / SSE4.2.
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0) {
        // EDX bit 26 = SSE2; ECX bit 20 = SSE4.2.
        c.has_sse2 = (edx & (1u << 26)) != 0;
        c.has_sse42 = (ecx & (1u << 20)) != 0;
    }
    // CPUID leaf 7 sub-leaf 0: AVX2 / AVX-512.
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) != 0) {
        // EBX bit 5 = AVX2; bit 16 = AVX-512F; bit 30 = AVX-512BW.
        // AVX2 / AVX-512 also require OS support (XSAVE / XCR0). Most
        // modern Linux kernels have it; we don't gate on XGETBV here
        // because user-space libraries that ship in production
        // distros assume AVX2 is usable when the bit is set, and
        // we'd add complexity for a corner case (containerized
        // kernel without XSAVE).
        c.has_avx2 = (ebx & (1u << 5)) != 0;
        c.has_avx512f = (ebx & (1u << 16)) != 0;
        c.has_avx512bw = (ebx & (1u << 30)) != 0;
    }
#elif defined(__aarch64__)
    const unsigned long hwcap = getauxval(AT_HWCAP);
    c.has_neon = (hwcap & HWCAP_ASIMD) != 0;
    c.has_sve = (hwcap & HWCAP_SVE) != 0;
#endif
    return c;
}

}  // namespace rut
