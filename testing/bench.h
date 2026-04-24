#pragma once

// Lightweight microbenchmark framework — zero stdlib dependency.
// Inspired by nanobench. Uses clock_gettime(CLOCK_MONOTONIC).
//
// Usage:
//   #include "bench.h"
//
//   int main() {
//       rut::bench::Bench b;
//       b.title("HTTP parser");
//       b.min_iterations(1000000);
//       b.warmup(100000);
//
//       b.run("rue_parse", [&] {
//           parser.reset();
//           parser.parse(buf, len, &req);
//           rut::bench::do_not_optimize(&req);
//       });
//
//       b.run("llhttp_parse", [&] { ... });
//   }

#include "perf_counters.h"
#include "rut/common/types.h"

#include <fcntl.h>
#include <time.h>    // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>  // write

namespace rut::bench {

// ============================================================================
// Prevent dead-code elimination
// ============================================================================

// Force the compiler to keep a value alive (not optimized away).
template <typename T>
inline void do_not_optimize(T const* p) {
    asm volatile("" : : "r"(p) : "memory");
}

inline void clobber() {
    asm volatile("" : : : "memory");
}

// ============================================================================
// Output helpers (no stdlib)
// ============================================================================

inline void out(const char* s) {
    int len = 0;
    while (s[len]) len++;
    (void)::write(1, s, len);
}

inline void out_u64(u64 v) {
    if (v == 0) {
        out("0");
        return;
    }
    char buf[24];
    int n = 0;
    while (v > 0) {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    for (int i = n - 1; i >= 0; i--) (void)::write(1, &buf[i], 1);
}

// Print u64 with commas: 1,234,567
inline void out_u64_comma(u64 v) {
    char buf[32];
    int n = 0;
    if (v == 0) {
        out("0");
        return;
    }
    while (v > 0) {
        if (n > 0 && n % 3 == 0) buf[n++] = ',';
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    for (int i = n - 1; i >= 0; i--) (void)::write(1, &buf[i], 1);
}

// Print fixed-point: val is in nanoseconds, print as "X ns" or "X.YY us" or "X.YY ms"
inline void out_duration_ns(u64 ns) {
    if (ns < 1000) {
        out_u64(ns);
        out(" ns");
    } else if (ns < 1000000) {
        out_u64(ns / 1000);
        out(".");
        u64 frac = (ns % 1000) / 10;  // 2 decimal places
        if (frac < 10) out("0");
        out_u64(frac);
        out(" us");
    } else if (ns < 1000000000ULL) {
        out_u64(ns / 1000000);
        out(".");
        u64 frac = (ns % 1000000) / 10000;
        if (frac < 10) out("0");
        out_u64(frac);
        out(" ms");
    } else {
        out_u64(ns / 1000000000ULL);
        out(".");
        u64 frac = (ns % 1000000000ULL) / 10000000;
        if (frac < 10) out("0");
        out_u64(frac);
        out(" s");
    }
}

// Print throughput: bytes/sec → "X.YY MB/s" or "X.YY GB/s"
inline void out_throughput(u64 bytes_per_sec) {
    if (bytes_per_sec >= 1000000000ULL) {
        out_u64(bytes_per_sec / 1000000000ULL);
        out(".");
        u64 frac = (bytes_per_sec % 1000000000ULL) / 10000000ULL;
        if (frac < 10) out("0");
        out_u64(frac);
        out(" GB/s");
    } else {
        out_u64(bytes_per_sec / 1000000);
        out(".");
        u64 frac = (bytes_per_sec % 1000000) / 10000;
        if (frac < 10) out("0");
        out_u64(frac);
        out(" MB/s");
    }
}

// ============================================================================
// Timing
// ============================================================================

inline u64 now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
}

// ============================================================================
// Sort (for median) — simple insertion sort, N is small
// ============================================================================

inline void sort_u64(u64* arr, u32 n) {
    for (u32 i = 1; i < n; i++) {
        u64 key = arr[i];
        u32 j = i;
        while (j > 0 && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

// ============================================================================
// Bench — the main benchmark runner
// ============================================================================

static constexpr u32 kMaxEpochs = 64;

struct Result {
    const char* name;
    u64 median_ns;  // median time per iteration
    u64 min_ns;     // minimum time per iteration
    u64 mean_ns;    // mean time per iteration
    // MAD = median of |epoch_ns - median_ns|. A robust-to-outliers
    // dispersion measure (see nanobench). err_pct = MAD / median × 100
    // — anything > 5% means the benchmark isn't measuring cleanly
    // (CPU frequency scaling, thermal throttling, context switches,
    // other cores touching shared caches).
    u64 mad_ns;
    u32 err_pct;       // MAD / median × 100, rounded
    u64 iterations;    // total iterations run
    u64 bytes_per_op;  // bytes processed per operation (for throughput)
    // Hardware perf counters, totalled across all epochs. Zero when
    // counters weren't enabled or when the kernel refused access.
    u64 perf_cycles;
    u64 perf_instructions;
    u64 perf_branch_misses;
    u64 perf_cache_refs;
    u64 perf_cache_misses;
    bool perf_valid;
};

struct Bench {
    const char* title_ = "Benchmark";
    u64 min_iters_ = 100000;
    u64 warmup_iters_ = 10000;
    u32 epochs_ = 11;      // odd number for clean median
    u64 epoch_iters_ = 0;  // 0 = auto (min_iters / epochs)
    u64 bytes_per_op_ = 0;
    bool perf_counters_ = false;

    Result results_[32];
    u32 result_count_ = 0;

    void title(const char* t) { title_ = t; }
    void min_iterations(u64 n) { min_iters_ = n; }
    void warmup(u64 n) { warmup_iters_ = n; }
    void epochs(u32 n) {
        epochs_ = n < 1 ? 1 : (n > kMaxEpochs ? kMaxEpochs : n);
        if (epochs_ % 2 == 0) epochs_++;  // keep odd for clean median
    }
    void bytes_per_op(u64 n) { bytes_per_op_ = n; }
    // Enable hardware perf counters (cycles, instructions, branch-misses,
    // cache refs, cache misses) for each subsequent run(). No-op if the
    // kernel refuses access (perf_event_paranoid too high); the benchmark
    // still runs, just without PMU data.
    void perf_counters(bool enable) { perf_counters_ = enable; }

    template <typename Fn>
    Result run(const char* name, Fn&& fn) {
        u64 iters_per_epoch = epoch_iters_ ? epoch_iters_ : (min_iters_ / epochs_);
        if (iters_per_epoch < 1) iters_per_epoch = 1;

        // Warmup
        for (u64 i = 0; i < warmup_iters_; i++) {
            fn();
            clobber();
        }

        // Per-run perf counters. If the user asked for them but the
        // kernel refuses (perf_event_paranoid too high), silently fall
        // back to wall-clock only — the measurement still happens.
        PerfCounters pc;
        const bool perf_ok = perf_counters_ && pc.open();

        // Collect epoch timings + cumulative perf totals.
        u64 epoch_ns[kMaxEpochs];
        u64 total_ns = 0;
        u64 total_cycles = 0, total_inst = 0, total_bmiss = 0, total_cref = 0, total_cmiss = 0;

        for (u32 e = 0; e < epochs_; e++) {
            if (perf_ok) pc.enable();
            u64 t0 = now_ns();
            for (u64 i = 0; i < iters_per_epoch; i++) {
                fn();
                clobber();
            }
            u64 t1 = now_ns();
            if (perf_ok) {
                pc.disable();
                total_cycles += pc.cycles();
                total_inst += pc.instructions();
                total_bmiss += pc.branch_misses();
                total_cref += pc.cache_refs();
                total_cmiss += pc.cache_misses();
            }
            epoch_ns[e] = (t1 - t0) / iters_per_epoch;  // ns per iteration
            total_ns += (t1 - t0);
        }

        // Statistics
        u64 sorted[kMaxEpochs];
        for (u32 i = 0; i < epochs_; i++) sorted[i] = epoch_ns[i];
        sort_u64(sorted, epochs_);

        Result r;
        r.name = name;
        r.min_ns = sorted[0];
        r.median_ns = sorted[epochs_ / 2];
        r.mean_ns = total_ns / (iters_per_epoch * epochs_);

        // Median Absolute Deviation: median of |epoch - median|. More
        // robust to outliers than stddev. err_pct is how noisy this
        // run was; > 5% means the numbers probably can't be trusted
        // for tight comparisons.
        u64 abs_dev[kMaxEpochs];
        for (u32 i = 0; i < epochs_; i++) {
            const u64 v = sorted[i];
            abs_dev[i] = v >= r.median_ns ? v - r.median_ns : r.median_ns - v;
        }
        sort_u64(abs_dev, epochs_);
        r.mad_ns = abs_dev[epochs_ / 2];
        r.err_pct = r.median_ns > 0
                        ? static_cast<u32>((r.mad_ns * 100 + r.median_ns / 2) / r.median_ns)
                        : 0;

        r.iterations = iters_per_epoch * epochs_;
        r.bytes_per_op = bytes_per_op_;
        r.perf_valid = perf_ok;
        r.perf_cycles = total_cycles;
        r.perf_instructions = total_inst;
        r.perf_branch_misses = total_bmiss;
        r.perf_cache_refs = total_cref;
        r.perf_cache_misses = total_cmiss;

        // Print result
        out("  ");
        // Pad name to 24 chars
        int name_len = 0;
        while (name[name_len]) name_len++;
        out(name);
        for (int i = name_len; i < 24; i++) out(" ");

        out("  median: ");
        out_duration_ns(r.median_ns);

        out("  ±");
        out_u64(r.err_pct);
        out("%");
        if (r.err_pct > 5) out("!");  // flag noisy measurements

        out("  min: ");
        out_duration_ns(r.min_ns);

        if (bytes_per_op_ > 0 && r.median_ns > 0) {
            u64 bytes_per_sec = (bytes_per_op_ * 1000000000ULL) / r.median_ns;
            out("  ");
            out_throughput(bytes_per_sec);
        }

        out("  (");
        out_u64_comma(r.iterations);
        out(" iters)\n");

        if (perf_ok) {
            // Per-iteration perf metrics — the numbers that actually
            // explain WHY something is faster or slower.
            const u64 it = r.iterations;
            out("                              cycles/iter: ");
            out_u64(total_cycles / it);
            if (total_inst > 0) {
                out("  inst/iter: ");
                out_u64(total_inst / it);
                // IPC × 100 (fixed-point 2 decimals)
                const u64 ipc100 = (total_inst * 100) / (total_cycles > 0 ? total_cycles : 1);
                out("  IPC: ");
                out_u64(ipc100 / 100);
                out(".");
                const u64 frac = ipc100 % 100;
                if (frac < 10) out("0");
                out_u64(frac);
            }
            if (total_cref > 0) {
                // Cache miss rate in ‰ (per-mille) for 1-decimal %
                const u64 miss_thou = (total_cmiss * 1000) / total_cref;
                out("  L1-miss: ");
                out_u64(miss_thou / 10);
                out(".");
                out_u64(miss_thou % 10);
                out("%");
            }
            if (total_bmiss > 0) {
                out("  br-miss/iter: ");
                out_u64(total_bmiss / it);
            }
            out("\n");
        }

        if (result_count_ < 32) {
            results_[result_count_++] = r;
        }

        return r;
    }

    // Print comparison table after all runs
    void compare() {
        if (result_count_ < 2) return;

        out("\n  --- Comparison ---\n");
        // Use first result as baseline
        u64 base_ns = results_[0].median_ns;
        if (base_ns == 0) base_ns = 1;  // guard against div-by-zero
        const char* base_name = results_[0].name;

        for (u32 i = 0; i < result_count_; i++) {
            out("  ");
            int name_len = 0;
            while (results_[i].name[name_len]) name_len++;
            out(results_[i].name);
            for (int j = name_len; j < 24; j++) out(" ");

            if (i == 0) {
                out("  baseline\n");
            } else {
                u64 other_ns = results_[i].median_ns;
                if (other_ns == 0) other_ns = 1;  // guard against div-by-zero
                if (other_ns <= base_ns) {
                    u64 ratio = (base_ns * 100) / other_ns;
                    out("  ");
                    out_u64(ratio / 100);
                    out(".");
                    u64 frac = ratio % 100;
                    if (frac < 10) out("0");
                    out_u64(frac);
                    out("x faster than ");
                } else {
                    u64 ratio = (other_ns * 100) / base_ns;
                    out("  ");
                    out_u64(ratio / 100);
                    out(".");
                    u64 frac = ratio % 100;
                    if (frac < 10) out("0");
                    out_u64(frac);
                    out("x slower than ");
                }
                out(base_name);
                out("\n");
            }
        }
    }

    void print_header() {
        out("=== ");
        out(title_);
        out(" ===\n");
    }
};

// ============================================================================
// Environment checks
// ============================================================================
//
// Microbenchmarks are only as reliable as the system they run on. These
// helpers read /sys and /proc to flag the well-known foot-guns:
//   - CPU governor != performance  (frequency varies during the run)
//   - Intel turbo boost on         (same — amplified)
//   - CPU unpinned (no taskset)    (scheduler migration flushes caches)
//
// Call print_environment_warnings() once at the start of main() so the
// user sees the flags before reading numbers they might otherwise trust.

// Read up to `cap-1` bytes from `path` into `buf`, zero-terminate. Returns
// the count read, or 0 on any error. Trims a single trailing newline.
inline u32 read_small_file(const char* path, char* buf, u32 cap) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = ::read(fd, buf, cap - 1);
    ::close(fd);
    if (n <= 0) return 0;
    u32 len = static_cast<u32>(n);
    if (buf[len - 1] == '\n') len--;
    buf[len] = '\0';
    return len;
}

inline bool str_eq_cstr(const char* a, const char* b) {
    u32 i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

inline void print_environment_warnings() {
    char buf[128];

    out("--- Environment ---\n");

    // CPU governor.
    if (read_small_file(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", buf, sizeof(buf))) {
        out("  governor: ");
        out(buf);
        if (!str_eq_cstr(buf, "performance")) {
            out("  [!]  (expected 'performance' for stable measurements)");
        }
        out("\n");
    }

    // Intel no_turbo flag: "1" = turbo disabled (stable), "0" = turbo on.
    if (read_small_file("/sys/devices/system/cpu/intel_pstate/no_turbo", buf, sizeof(buf))) {
        out("  intel turbo: ");
        if (str_eq_cstr(buf, "1")) {
            out("OFF");
        } else {
            out("ON  [!]  (frequency will vary)");
        }
        out("\n");
    }

    // perf_event_paranoid — gate on PMU access.
    if (read_small_file("/proc/sys/kernel/perf_event_paranoid", buf, sizeof(buf))) {
        out("  perf_event_paranoid: ");
        out(buf);
        if (buf[0] != '0' && buf[0] != '1' && buf[0] != '2') {
            out("  [!]  (perf counters will be unavailable; sudo sysctl "
                "kernel.perf_event_paranoid=2)");
        }
        out("\n");
    }

    out("  suggestion: taskset -c 0 ./bench_...    # pin to one core\n");
    out("\n");
}

}  // namespace rut::bench
