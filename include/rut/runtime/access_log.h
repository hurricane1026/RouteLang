#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/error.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace rut {

// HTTP methods (compact enum for log entries).
enum class HttpMethod : u8 {
    Get = 0,
    Post,
    Put,
    Delete,
    Patch,
    Head,
    Options,
    Connect,
    Trace,
    Other,
};

// Access log entry — fixed-size, written by shard thread on request completion.
// 128 bytes: fits two per cache line pair, ~64KB for 512 entries.
struct AccessLogEntry {
    u64 timestamp_us;  // microseconds since epoch (clock_realtime)
    u32 duration_us;   // request processing time
    u32 req_size;      // request size (full message until parser available)
    u32 resp_size;     // response size (full message until parser available)
    u32 upstream_us;   // upstream latency (0 if no proxy)
    u32 addr;          // client IPv4 (network byte order)
    u16 status;        // HTTP status code
    u8 method;         // HttpMethod enum
    u8 shard_id;
    char path[64];      // truncated if longer, null-terminated
    char upstream[24];  // upstream name, null-terminated
    u8 _pad[8];         // pad to 128 bytes
};

static_assert(sizeof(AccessLogEntry) == 128, "AccessLogEntry must be 128 bytes");

// Microsecond wall-clock timestamp (for access log timestamp field).
inline u64 realtime_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000ULL + static_cast<u64>(ts.tv_nsec) / 1000ULL;
}

// Monotonic microsecond clock (for elapsed duration measurement).
// Immune to NTP adjustments and wall-clock jumps.
inline u64 monotonic_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000ULL + static_cast<u64>(ts.tv_nsec) / 1000ULL;
}

// SPSC (single-producer, single-consumer) ring buffer for access log entries.
//
// Producer: shard thread (writes on request completion)
// Consumer: background flusher thread (reads + writes raw binary to fd)
//
// Lock-free via acquire/release atomics on positions.
// Overflow policy: drop newest (push returns false when full).
// Flusher catches up and future entries succeed.
//
// Capacity must be power of 2 for fast modulo.

struct AccessLogRing {
    static constexpr u32 kCapacity = 512;  // 512 * 128 = 64KB
    static constexpr u32 kMask = kCapacity - 1;

    // Cache-line aligned to prevent false sharing between producer and consumer.
    alignas(64) u32 write_pos;  // written by shard thread only
    alignas(64) u32 read_pos;   // written by flusher thread only
    AccessLogEntry entries[kCapacity];

    void init() {
        write_pos = 0;
        read_pos = 0;
    }

    // Producer: write an entry. Returns false if full (entry dropped).
    // Called from shard thread only — no contention on write_pos.
    //
    // The producer never touches read_pos. When the ring is full, the entry
    // is silently dropped. This avoids a race where the producer advances
    // read_pos while the consumer is mid-read of the slot being overwritten.
    // Dropping newest under backpressure is acceptable for access logs —
    // the flusher will catch up and future entries will succeed.
    bool push(const AccessLogEntry& entry) {
        u32 wp = __atomic_load_n(&write_pos, __ATOMIC_RELAXED);
        u32 rp = __atomic_load_n(&read_pos, __ATOMIC_ACQUIRE);

        if (wp - rp >= kCapacity) {
            return false;  // full — drop this entry
        }

        entries[wp & kMask] = entry;
        __atomic_store_n(&write_pos, wp + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Consumer: read one entry. Returns false if empty.
    // Called from flusher thread only — no contention on read_pos.
    bool pop(AccessLogEntry& out) {
        u32 rp = __atomic_load_n(&read_pos, __ATOMIC_RELAXED);
        u32 wp = __atomic_load_n(&write_pos, __ATOMIC_ACQUIRE);

        if (rp == wp) return false;  // empty

        out = entries[rp & kMask];
        __atomic_store_n(&read_pos, rp + 1, __ATOMIC_RELEASE);
        return true;
    }

    // Number of entries available to read.
    u32 available() const {
        u32 wp = __atomic_load_n(&write_pos, __ATOMIC_ACQUIRE);
        u32 rp = __atomic_load_n(&read_pos, __ATOMIC_RELAXED);
        return wp - rp;
    }
};

// Format an access log entry as a text line into buf.
// Returns bytes written. Format: "ts method path status duration_us req_size resp_size addr
// shard\n" Caller provides buf of at least 512 bytes.
u32 format_access_log_text(const AccessLogEntry& entry, char* buf, u32 buf_size);

// Background flusher — reads from all shard rings, writes text entries to fd.
// Optionally compresses output with zstd streaming compression.
//
// Modes:
//   compress=false: writes plain text lines (greppable, ~350 bytes/entry)
//   compress=true:  writes zstd-compressed text (~25-35 bytes/entry)
//
// Enable via: --access-log-compress flag or RUE_ACCESS_LOG_COMPRESS=1 env var.
//
// Lifecycle: init() → start() → [running] → stop()
// The flusher thread wakes every flush_interval_ms to drain all rings.

struct AccessLogFlusher {
    static constexpr u32 kMaxRings = 64;

    AccessLogRing* rings[kMaxRings];
    u32 ring_count;
    i32 output_fd;          // fd to write to
    u32 flush_interval_ms;  // how often to flush (default 100ms)
    bool compress;          // zstd compression enabled
    i32 compress_level;     // zstd level: 1-4 (fast/doubleFast only)

    pthread_t thread;
    bool running;  // cross-thread: accessed via __atomic builtins

    // Opaque zstd state (ZSTD_CStream*), managed in access_log.cc.
    void* zstd_ctx;

    // Max supported level — higher levels require strategies we excluded at build time.
    static constexpr i32 kMinLevel = 1;
    static constexpr i32 kMaxLevel = 4;
    static constexpr i32 kDefaultLevel = 3;

    void init(i32 fd,
              bool compress_enabled = false,
              i32 level = kDefaultLevel,
              u32 interval_ms = 100) {
        ring_count = 0;
        output_fd = fd;
        flush_interval_ms = interval_ms;
        compress = compress_enabled;
        // Clamp level to supported range (fast + doubleFast only).
        if (level < kMinLevel) level = kMinLevel;
        if (level > kMaxLevel) level = kMaxLevel;
        compress_level = level;
        thread = 0;
        running = false;
        zstd_ctx = nullptr;
        for (u32 i = 0; i < kMaxRings; i++) rings[i] = nullptr;
    }

    void add_ring(AccessLogRing* ring) {
        if (ring_count < kMaxRings) {
            rings[ring_count++] = ring;
        }
    }

    core::Expected<void, Error> start();
    void stop();

    // Flush all rings once. Returns total entries flushed.
    u32 flush_once();

    // Write a batch of formatted text, optionally compressing with zstd.
    bool flush_batch(const u8* data, u32 len);

private:
    static void* thread_entry(void* arg);
};

}  // namespace rut
