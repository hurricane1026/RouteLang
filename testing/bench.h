#pragma once

// Lightweight microbenchmark framework — zero stdlib dependency.
// Inspired by nanobench. Uses clock_gettime(CLOCK_MONOTONIC).
//
// Usage:
//   #include "bench.h"
//
//   int main() {
//       rout::bench::Bench b;
//       b.title("HTTP parser");
//       b.min_iterations(1000000);
//       b.warmup(100000);
//
//       b.run("rue_parse", [&] {
//           parser.reset();
//           parser.parse(buf, len, &req);
//           rout::bench::do_not_optimize(&req);
//       });
//
//       b.run("llhttp_parse", [&] { ... });
//   }

#include "rout/common/types.h"

#include <time.h>    // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>  // write

namespace rout::bench {

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
    u64 median_ns;     // median time per iteration
    u64 min_ns;        // minimum time per iteration
    u64 mean_ns;       // mean time per iteration
    u64 iterations;    // total iterations run
    u64 bytes_per_op;  // bytes processed per operation (for throughput)
};

struct Bench {
    const char* title_ = "Benchmark";
    u64 min_iters_ = 100000;
    u64 warmup_iters_ = 10000;
    u32 epochs_ = 11;      // odd number for clean median
    u64 epoch_iters_ = 0;  // 0 = auto (min_iters / epochs)
    u64 bytes_per_op_ = 0;

    Result results_[32];
    u32 result_count_ = 0;

    void title(const char* t) { title_ = t; }
    void min_iterations(u64 n) { min_iters_ = n; }
    void warmup(u64 n) { warmup_iters_ = n; }
    void epochs(u32 n) { epochs_ = n > kMaxEpochs ? kMaxEpochs : n; }
    void bytes_per_op(u64 n) { bytes_per_op_ = n; }

    template <typename Fn>
    Result run(const char* name, Fn&& fn) {
        u64 iters_per_epoch = epoch_iters_ ? epoch_iters_ : (min_iters_ / epochs_);
        if (iters_per_epoch < 1) iters_per_epoch = 1;

        // Warmup
        for (u64 i = 0; i < warmup_iters_; i++) {
            fn();
            clobber();
        }

        // Collect epoch timings
        u64 epoch_ns[kMaxEpochs];
        u64 total_ns = 0;

        for (u32 e = 0; e < epochs_; e++) {
            u64 t0 = now_ns();
            for (u64 i = 0; i < iters_per_epoch; i++) {
                fn();
                clobber();
            }
            u64 t1 = now_ns();
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
        r.iterations = iters_per_epoch * epochs_;
        r.bytes_per_op = bytes_per_op_;

        // Print result
        out("  ");
        // Pad name to 24 chars
        int name_len = 0;
        while (name[name_len]) name_len++;
        out(name);
        for (int i = name_len; i < 24; i++) out(" ");

        out("  median: ");
        out_duration_ns(r.median_ns);

        out("  min: ");
        out_duration_ns(r.min_ns);

        out("  mean: ");
        out_duration_ns(r.mean_ns);

        if (bytes_per_op_ > 0 && r.median_ns > 0) {
            u64 bytes_per_sec = (bytes_per_op_ * 1000000000ULL) / r.median_ns;
            out("  ");
            out_throughput(bytes_per_sec);
        }

        out("  (");
        out_u64_comma(r.iterations);
        out(" iters)\n");

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
                if (results_[i].median_ns <= base_ns) {
                    u64 ratio = (base_ns * 100) / results_[i].median_ns;
                    out("  ");
                    out_u64(ratio / 100);
                    out(".");
                    u64 frac = ratio % 100;
                    if (frac < 10) out("0");
                    out_u64(frac);
                    out("x faster than ");
                } else {
                    u64 ratio = (results_[i].median_ns * 100) / base_ns;
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

}  // namespace rout::bench
