#pragma once

// CpuCaps — runtime SIMD capability detection.
//
// Why runtime detection (vs compile-time SIMD_ARCH):
//   The HTTP parser's existing simd/ setup picks ONE arch at build
//   time (the binary uses that arch on every machine). For mesh
//   distribution we sometimes want the opposite — the SAME binary
//   adapts to whichever CPU it lands on. Route dispatch is the first
//   such case: pick_dispatch consults CpuCaps to choose between
//   SSE2 / AVX2 / NEON LinearScan variants based on what's available.
//
// Why injection (caller-maintained), not a global singleton:
//   The picker is a pure function — feeding caps in keeps it
//   reproducible across tests / fuzz / different deployments. The
//   caller maintains the singleton (typically rutproxy::main()
//   probes once at startup and threads the result through).
//
// Cost model:
//   detect() runs CPUID (x86) / getauxval (ARM) once. ~50 ns. NOT
//   on the hot path — call once at startup, store the struct, pass
//   by const reference thereafter.
//
// Sentinel: a default-constructed CpuCaps reports "no SIMD" so
// callers that haven't probed yet (tests, early init) get the
// scalar-only path naturally. Tests that want a specific cap
// profile construct CpuCaps explicitly.

#include "rut/common/types.h"

namespace rut {

struct CpuCaps {
    // x86_64 capabilities
    bool has_sse2 = false;
    bool has_sse42 = false;
    bool has_avx2 = false;
    bool has_avx512f = false;
    bool has_avx512bw = false;

    // ARM64 capabilities
    bool has_neon = false;
    bool has_sve = false;

    // Probe the actual CPU. Cheap (one-shot CPUID / getauxval); call
    // once at startup. The returned struct is small (POD) so callers
    // copy it freely.
    static CpuCaps detect();

    // Useful for tests / fuzz / "force-scalar" mode: a CpuCaps with
    // all flags off. Equivalent to `CpuCaps{}` but more explicit at
    // call sites.
    static constexpr CpuCaps scalar_only() { return CpuCaps{}; }
};

}  // namespace rut
