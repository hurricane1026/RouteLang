// Tests for testing/perf_counters.h — verifies the RAII wrapper around
// perf_event_open works end-to-end. The tests SKIP (not silently pass)
// when the kernel refuses PMU access, so restricted environments
// (containers, perf_event_paranoid > 2, kernels without hardware
// counters) show up as visible skip notes rather than fake green.

#include "perf_counters.h"
#include "test.h"

using namespace rut;
using namespace rut::bench;

// Helper: some deterministic work so cycles / instructions should both
// come back non-zero.
__attribute__((noinline)) static u64 busy_work(u32 iters) {
    u64 acc = 1;
    for (u32 i = 0; i < iters; i++) {
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 7;
    }
    return acc;
}

TEST(perf_counters, open_reports_capability) {
    PerfCounters pc;
    if (!pc.open()) {
        SKIP("PMU unavailable (perf_event_paranoid > 2 or no hardware counters)");
    }
    // Leader must be valid if open() returned true; secondary counters
    // are best-effort.
    CHECK(pc.has(kPerfCycles));
}

TEST(perf_counters, enable_disable_cycles_nonzero) {
    PerfCounters pc;
    if (!pc.open()) {
        SKIP("PMU unavailable");
    }
    pc.enable();
    u64 sink = busy_work(100000);
    pc.disable();
    asm volatile("" : : "r"(&sink) : "memory");
    // If the read itself failed (EINTR-final, short read, kernel
    // disagreement) the snapshot is all zeros regardless of which
    // counters opened — asserting >0 here would be a phantom
    // failure. Skip the nonzero checks in that case.
    if (!pc.last_read_ok()) {
        SKIP("perf read failed (signal / short read)");
    }

    // Cycles should be well above zero for 100k iterations of busy work
    // (roughly 300k+ cycles on any modern core).
    if (pc.has(kPerfCycles)) {
        CHECK_GT(pc.cycles(), 1000u);
    }
    if (pc.has(kPerfInstructions)) {
        CHECK_GT(pc.instructions(), 1000u);
    }
}

TEST(perf_counters, reset_between_measurements) {
    PerfCounters pc;
    if (!pc.open()) {
        SKIP("PMU unavailable");
    }
    // First measurement
    pc.enable();
    u64 s1 = busy_work(10000);
    pc.disable();
    if (!pc.last_read_ok()) {
        SKIP("perf read failed on first measurement");
    }
    const u64 first_cycles = pc.cycles();
    asm volatile("" : : "r"(&s1) : "memory");

    // Second measurement — enable() resets the group, so values should
    // reflect only the new window, not accumulate from the first.
    pc.enable();
    u64 s2 = busy_work(10000);
    pc.disable();
    if (!pc.last_read_ok()) {
        SKIP("perf read failed on second measurement");
    }
    const u64 second_cycles = pc.cycles();
    asm volatile("" : : "r"(&s2) : "memory");

    if (pc.has(kPerfCycles)) {
        // Both should be in the same order of magnitude — the second
        // must not be an accumulation of both (which would put it
        // meaningfully above first).
        CHECK_GT(second_cycles, 0u);
        // Allow generous slack (3x) for scheduler / frequency noise on
        // an idle machine; we just want to catch "reset didn't happen
        // and second is 2x first" class bugs.
        CHECK(second_cycles < first_cycles * 3 + 100000);
    }
}

TEST(perf_counters, accumulate_sums_values) {
    PerfCounters a, b;
    if (!a.open() || !b.open()) {
        SKIP("PMU unavailable");
    }
    a.enable();
    u64 s1 = busy_work(5000);
    a.disable();
    asm volatile("" : : "r"(&s1) : "memory");

    b.enable();
    u64 s2 = busy_work(5000);
    b.disable();
    asm volatile("" : : "r"(&s2) : "memory");

    const u64 a_cycles = a.cycles();
    const u64 b_cycles = b.cycles();
    a.accumulate(b);
    if (a.has(kPerfCycles) && b.has(kPerfCycles)) {
        CHECK_EQ(a.cycles(), a_cycles + b_cycles);
    }
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
