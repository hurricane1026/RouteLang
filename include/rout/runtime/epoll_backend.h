#pragma once

#include "rout/common/types.h"
#include "rout/runtime/io_backend.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

namespace rout {

// epoll backend — reactor internally, proactor API externally.
// "Reactor disguised as proactor": wait() does the recv/send
// and emits IoEvent completions, identical to io_uring's output.
//
// Fallback for Linux < 6.0 (epoll available since 3.9+ with SO_REUSEPORT).
//
// Key differences from io_uring:
//   - No provided buffer ring: allocate slice on EPOLLIN readiness
//   - No multishot: accept loop inside wait()
//   - Two syscalls per I/O (epoll_wait + recv/send) vs one batched
//   - No zero-copy send
//
struct EpollBackend {
    i32 epoll_fd = -1;
    i32 timer_fd = -1;
    i32 listen_fd = -1;

    // Pending synthetic completion events (from immediate sends)
    IoEvent pending_completions[64];
    u32 pending_count = 0;

    // --- Interface methods ---

    // Initialize epoll and timerfd for this shard.
    // Returns 0 on success, -errno on failure.
    i32 init(u32 shard_id, i32 listen_fd);

    // Register listen socket for accept events.
    void add_accept();

    // Register fd for EPOLLIN — actual recv happens inside wait().
    void add_recv(i32 fd, u32 conn_id);

    // Try immediate send. If partial/EAGAIN, register EPOLLOUT.
    void add_send(i32 fd, u32 conn_id, const u8* buf, u32 len);

    // Register fd for connect completion (EPOLLOUT).
    void add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len);

    // Remove fd from epoll.
    void cancel(i32 fd, u32 conn_id);

    // Wait for events, perform I/O, return completion events.
    u32 wait(IoEvent* events, u32 max_events);

    // Shutdown and close fds.
    void shutdown();

private:
    // Encode conn_id + type into epoll_event.data.u64
    static u64 encode_data(u32 conn_id, IoEventType type);
    static void decode_data(u64 data, u32& conn_id, IoEventType& type);
};

}  // namespace rout
