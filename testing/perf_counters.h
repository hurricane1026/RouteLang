#pragma once

// PerfCounters — RAII wrapper around Linux perf_event_open for reading
// hardware performance counters (cycles, instructions, branch-misses,
// cache-references, cache-misses) during microbenchmarks.
//
// Modeled on nanobench's PerformanceCounters: a single event group so the
// kernel schedules all counters together (all counted in the same time
// window; no risk of counter A being descheduled while B runs). Leader is
// cycles; other counters are added with `group_fd = leader_fd`.
//
// Usage:
//   rut::bench::PerfCounters pc;
//   if (pc.open()) {
//       pc.enable();
//       <work>
//       pc.disable();
//       u64 cycles = pc.cycles();
//       u64 insts  = pc.instructions();
//       ...
//   } else {
//       // perf_event_paranoid too high, or no hardware PMU, or similar.
//       // Fall back to wall-clock only.
//   }
//
// Common blockers and how to clear them:
//   - /proc/sys/kernel/perf_event_paranoid > 2 prevents unprivileged
//     access to hardware counters. Lower it (`sudo sysctl
//     kernel.perf_event_paranoid=2`, matching the threshold the
//     group open tolerates) or run under a user with CAP_PERFMON.
//   - Containers / VMs often block PMU access entirely; open() returns
//     EACCES or ENOENT.
//   - Some kernels gate cache events behind extra permissions; those
//     counters silently return zero if they fail — check validity bits.

#include "rut/common/types.h"

#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace rut::bench {

// Raw syscall wrapper — the glibc header doesn't provide one.
inline long perf_event_open_raw(
    struct perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// Keep these indexes stable — they match the order counters are opened
// and the positions in the read buffer returned by read().
enum PerfCounterIndex : u32 {
    kPerfCycles = 0,
    kPerfInstructions = 1,
    kPerfBranchMisses = 2,
    kPerfCacheRefs = 3,
    kPerfCacheMisses = 4,
    kPerfCounterCount = 5,
};

class PerfCounters {
public:
    PerfCounters() {
        for (u32 i = 0; i < kPerfCounterCount; i++) {
            fds_[i] = -1;
            last_values_[i] = 0;
            valid_[i] = false;
        }
        last_read_ok_ = true;
    }

    ~PerfCounters() { close_all(); }

    // Non-copyable: fds are owned resources.
    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    // Open the full counter group. Returns true iff the leader (cycles)
    // opens successfully — secondary counters are best-effort and may
    // individually fail without affecting the return value (callers
    // should use has() / value() defensively, e.g. check
    // has(kPerfInstructions) before computing IPC).
    //
    // Idempotent: calling open() on an already-open or partially-open
    // instance first tears down the previous group so fds don't leak
    // and stale valid_/last_values_ state can't carry over.
    bool open() {
        close_all();  // safe no-op if no fds are open
        for (u32 i = 0; i < kPerfCounterCount; i++) {
            last_values_[i] = 0;
            valid_[i] = false;
        }
        // Clear the snapshot-validity flag too so a failed read from
        // a previous open/enable cycle can't leak into the new
        // session. Callers inspecting last_read_ok() before the first
        // enable() on the re-opened group should see `true`, not a
        // stale `false` carried over from a prior teardown.
        last_read_ok_ = true;

        // Leader = CPU cycles. Disabled at start; PERF_EVENT_IOC_ENABLE
        // on the leader propagates to the whole group via
        // PERF_IOC_FLAG_GROUP.
        if (!open_counter(kPerfCycles,
                          PERF_TYPE_HARDWARE,
                          PERF_COUNT_HW_CPU_CYCLES,
                          -1,
                          /*disabled=*/true)) {
            return false;
        }
        const int leader = fds_[kPerfCycles];
        // Secondary counters join the group via group_fd = leader.
        // attr.disabled = 0 for group members; they follow the leader.
        open_counter(
            kPerfInstructions, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, leader, false);
        open_counter(
            kPerfBranchMisses, PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES, leader, false);
        open_counter(
            kPerfCacheRefs, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, leader, false);
        open_counter(
            kPerfCacheMisses, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, leader, false);
        return true;
    }

    // Reset and enable the whole group. Must be called after open();
    // sampling starts immediately.
    void enable() {
        if (fds_[kPerfCycles] < 0) return;
        ::ioctl(fds_[kPerfCycles], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ::ioctl(fds_[kPerfCycles], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
        last_read_ok_ = true;
    }

    // Disable the group and pull the counter values. Subsequent has() /
    // cycles() / etc. reads see the snapshot taken here.
    void disable() {
        if (fds_[kPerfCycles] < 0) return;
        ::ioctl(fds_[kPerfCycles], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        read_all();
    }

    // True iff the most recent disable()'s read_all() returned a
    // complete snapshot. False after a short / EINTR-final read —
    // values in this case are zero rather than stale, but callers
    // should suppress per-iteration perf output rather than print
    // phantom zeros.
    bool last_read_ok() const { return last_read_ok_; }

    // Whether the counter at `idx` was opened successfully. Secondary
    // counter opens can fail independently of the leader; callers that
    // want IPC etc. should check has(kPerfCycles) && has(kPerfInstructions).
    bool has(u32 idx) const { return idx < kPerfCounterCount && valid_[idx]; }

    u64 value(u32 idx) const { return idx < kPerfCounterCount ? last_values_[idx] : 0; }

    // Convenience accessors.
    u64 cycles() const { return value(kPerfCycles); }
    u64 instructions() const { return value(kPerfInstructions); }
    u64 branch_misses() const { return value(kPerfBranchMisses); }
    u64 cache_refs() const { return value(kPerfCacheRefs); }
    u64 cache_misses() const { return value(kPerfCacheMisses); }

    // Add counters from `other` into this one. Used to accumulate
    // across epochs.
    void accumulate(const PerfCounters& other) {
        for (u32 i = 0; i < kPerfCounterCount; i++) {
            if (other.valid_[i]) {
                last_values_[i] += other.last_values_[i];
                valid_[i] = valid_[i] || other.valid_[i];
            }
        }
    }

    void reset_values() {
        for (u32 i = 0; i < kPerfCounterCount; i++) last_values_[i] = 0;
    }

private:
    int fds_[kPerfCounterCount];
    u64 last_values_[kPerfCounterCount];
    bool valid_[kPerfCounterCount];
    bool last_read_ok_ = true;

    bool open_counter(u32 idx, u32 type, u64 config, int group_fd, bool disabled) {
        struct perf_event_attr attr;
        ::memset(&attr, 0, sizeof(attr));
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = disabled ? 1 : 0;
        attr.exclude_kernel = 1;  // measure userspace only — kernel work is noise
        attr.exclude_hv = 1;      // skip hypervisor ticks
        attr.read_format = PERF_FORMAT_GROUP;
        // pid=0 → calling process. cpu=-1 → any CPU (the scheduler may
        // migrate us; caller should pin with taskset for stability).
        // PERF_FLAG_FD_CLOEXEC sets close-on-exec on the perf fd so
        // these handles don't leak into child processes across exec()
        // — benchmarks often fork subprocesses for setup or warmup.
        long fd = perf_event_open_raw(&attr, 0, -1, group_fd, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) {
            fds_[idx] = -1;
            valid_[idx] = false;
            return false;
        }
        fds_[idx] = static_cast<int>(fd);
        valid_[idx] = true;
        return true;
    }

    void read_all() {
        // Layout with PERF_FORMAT_GROUP (no TIME_* / ID bits):
        //   u64 nr;
        //   u64 values[nr];
        // where values[] is in the order group members were opened
        // (only successfully-opened counters appear).
        //
        // Zero the snapshot up front: on short/failed read the caller
        // must see zeros, not stale values from the previous epoch —
        // otherwise a silent read failure quietly poisons the next
        // accumulate() and the printed cycles-per-iter becomes a
        // phantom from some prior run.
        //
        // Also re-optimize on success: start by assuming the read
        // will succeed and only flip last_read_ok_ false on a
        // concrete failure path. This way a successful read after a
        // prior failure clears the sticky flag without requiring a
        // matching enable() — otherwise a disable()-without-enable()
        // would keep reporting stale "invalid snapshot".
        for (u32 i = 0; i < kPerfCounterCount; i++) last_values_[i] = 0;
        last_read_ok_ = true;

        // Count how many group members were actually opened so we know
        // the minimum correct read size.
        u32 opened_count = 0;
        for (u32 i = 0; i < kPerfCounterCount; i++) {
            if (valid_[i]) opened_count++;
        }
        if (opened_count == 0) {
            last_read_ok_ = false;
            return;
        }

        constexpr u32 kBufSlots = 1 + kPerfCounterCount;
        u64 buf[kBufSlots];
        ssize_t n;
        do {
            n = ::read(fds_[kPerfCycles], buf, sizeof(buf));
        } while (n < 0 && errno == EINTR);
        const ssize_t expected = static_cast<ssize_t>((1 + opened_count) * sizeof(u64));
        if (n < expected) {
            // Short or failed read. Values stay zero (already cleared
            // above). Mark the snapshot invalid so Bench::run can
            // suppress perf output for this epoch — printing "cycles/
            // iter: 0" after a read failure would look like real data.
            last_read_ok_ = false;
            return;
        }

        const u64 nr = buf[0];
        if (nr < opened_count) {
            last_read_ok_ = false;
            return;
        }

        // Map read values back to the stable per-counter slots.
        u32 slot = 1;
        for (u32 i = 0; i < kPerfCounterCount && slot < kBufSlots && slot - 1 < nr; i++) {
            if (!valid_[i]) continue;
            last_values_[i] = buf[slot++];
        }
    }

    void close_all() {
        for (u32 i = 0; i < kPerfCounterCount; i++) {
            if (fds_[i] >= 0) {
                ::close(fds_[i]);
                fds_[i] = -1;
            }
        }
    }
};

}  // namespace rut::bench
