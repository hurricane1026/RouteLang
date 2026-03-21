#pragma once

#include "rout/common/types.h"
#include "rout/runtime/arena.h"
#include "rout/runtime/event_loop.h"
#include "rout/runtime/route_table.h"
#include "rout/runtime/upstream_pool.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rout {

// Shard — per-core share-nothing runtime unit.
//
// Each Shard owns:
//   - One EventLoop (mmap'd, ~130MB) with backend, connections, timer wheel
//   - One listen socket (SO_REUSEPORT, kernel distributes across shards)
//   - One Arena (per-request scratch memory, reset between requests)
//   - One OS thread, optionally pinned to a CPU core
//
// Lifecycle: init() → spawn() → [running] → stop() → join() → shutdown()
//
// Thread safety: each Shard is single-threaded. Cross-shard communication
// is lock-free (atomics, per-shard counters). Only stop() is called from
// outside the shard's thread.

template <typename Backend>
struct Shard {
    u32 id = 0;
    i32 listen_fd = -1;
    bool owns_listen_fd = false;  // if true, close on shutdown

    // EventLoop — mmap'd due to size (~130MB from Connection[16384])
    EventLoop<Backend>* loop = nullptr;

    // Per-request scratch arena (reset after each request cycle)
    Arena scratch;

    // Upstream connection pool (per-shard, mmap'd due to size)
    UpstreamPool* upstream = nullptr;

    // Route config — atomically swappable for hot reload.
    // Shared across all shards (read-only after construction).
    // Pointer swapped via atomic store; old config reclaimed after epoch drain.
    const RouteConfig* route_config = nullptr;

    // Thread
    pthread_t thread = 0;
    bool thread_spawned = false;

    // Initialize shard: mmap EventLoop, init backend + arena.
    // listen_fd must already be created with SO_REUSEPORT.
    // Returns 0 on success, -errno on failure.
    i32 init(u32 shard_id, i32 lfd) {
        id = shard_id;
        listen_fd = lfd;

        // mmap the EventLoop (too large for stack)
        void* mem = mmap(nullptr,
                         sizeof(EventLoop<Backend>),
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS,
                         -1,
                         0);
        if (mem == MAP_FAILED) return -errno;
        loop = static_cast<EventLoop<Backend>*>(mem);

        i32 rc = loop->init(shard_id, listen_fd);
        if (rc < 0) {
            munmap(loop, sizeof(EventLoop<Backend>));
            loop = nullptr;
            return rc;
        }

        // Init scratch arena (64KB initial block, grows on demand)
        rc = scratch.init(65536);
        if (rc < 0) {
            loop->shutdown();
            munmap(loop, sizeof(EventLoop<Backend>));
            loop = nullptr;
            return rc;
        }

        // mmap upstream pool (UpstreamConn[4096] is ~50KB)
        void* up_mem = mmap(nullptr,
                            sizeof(UpstreamPool),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            -1,
                            0);
        if (up_mem == MAP_FAILED) {
            scratch.destroy();
            loop->shutdown();
            munmap(loop, sizeof(EventLoop<Backend>));
            loop = nullptr;
            return -errno;
        }
        upstream = static_cast<UpstreamPool*>(up_mem);
        upstream->init();

        return 0;
    }

    // Spawn the shard's thread. Optionally pin to CPU core.
    // pin_cpu: -1 = no pinning, >=0 = pin to that core.
    // Returns 0 on success, -errno on failure.
    i32 spawn(i32 pin_cpu = -1) {
        if (!loop) return -EINVAL;

        pthread_attr_t attr;
        pthread_attr_init(&attr);

        // Pin to CPU if requested
        if (pin_cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(pin_cpu, &cpuset);
            pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
        }

        i32 rc = pthread_create(&thread, &attr, thread_entry, this);
        pthread_attr_destroy(&attr);
        if (rc != 0) return -rc;
        thread_spawned = true;
        return 0;
    }

    // Signal the shard to stop (safe to call from any thread).
    void stop() {
        if (loop) loop->stop();
    }

    // Wait for the shard's thread to finish.
    void join() {
        if (thread_spawned) {
            pthread_join(thread, nullptr);
            thread_spawned = false;
        }
    }

    // Release all resources.
    void shutdown() {
        if (upstream) {
            upstream->shutdown();
            munmap(upstream, sizeof(UpstreamPool));
            upstream = nullptr;
        }
        if (loop) {
            loop->shutdown();
            munmap(loop, sizeof(EventLoop<Backend>));
            loop = nullptr;
        }
        scratch.destroy();
        if (owns_listen_fd && listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
        }
    }

private:
    static void* thread_entry(void* arg) {
        auto* self = static_cast<Shard*>(arg);
        self->loop->run();
        return nullptr;
    }
};

// Detect CPU count (online cores).
inline u32 detect_cpu_count() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? static_cast<u32>(n) : 1;
}

}  // namespace rout
