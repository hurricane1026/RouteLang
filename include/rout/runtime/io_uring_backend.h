#pragma once

#include "rout/common/types.h"
#include "rout/runtime/error.h"
#include "rout/runtime/io_backend.h"

#include "core/expected.h"

#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace rout {

struct Connection;  // forward declaration for wait() signature

// Direct io_uring backend — no liburing dependency.
// Uses raw syscalls for full control (~300 lines per DESIGN.md).
//
// io_uring features (Linux 6.0+). Currently enabled marked with [*]:
//   [*] IORING_ACCEPT_MULTISHOT   — one SQE continuously accepts
//   [*] IORING_RECV_MULTISHOT     — one SQE continuously receives per connection
//   [*] IOSQE_BUFFER_SELECT       — kernel picks buffer from provided ring
//   [ ] IORING_SETUP_SQPOLL       — kernel-side SQ polling (needs CAP_SYS_NICE)
//   [*] IORING_SETUP_SINGLE_ISSUER — single-thread optimization
//   [ ] IORING_OP_SEND_ZC         — zero-copy send (future optimization)
//   [*] IORING_SETUP_COOP_TASKRUN — cooperative task running
//
struct IoUringBackend {
    // Ring file descriptor
    i32 ring_fd = -1;

    // SQ ring mapped memory
    u32* sq_head = nullptr;
    u32* sq_tail = nullptr;
    u32* sq_ring_mask = nullptr;
    u32* sq_array = nullptr;
    io_uring_sqe* sq_entries = nullptr;
    u32 sq_ring_entries = 0;

    // CQ ring mapped memory
    u32* cq_head = nullptr;
    u32* cq_tail = nullptr;
    u32* cq_ring_mask = nullptr;
    io_uring_cqe* cq_entries = nullptr;
    u32 cq_ring_entries = 0;

    // Mapped regions (for cleanup)
    void* sq_ring_ptr = nullptr;
    u64 sq_ring_sz = 0;
    void* cq_ring_ptr = nullptr;
    u64 cq_ring_sz = 0;
    void* sqes_ptr = nullptr;
    u64 sqes_sz = 0;

    // Provided buffer ring
    io_uring_buf_ring* buf_ring = nullptr;
    u8* buf_base = nullptr;  // kProvidedBufCount * kProvidedBufSize bytes

    // Listen socket
    i32 listen_fd = -1;

    // Timer fd for 1-second ticks (drives timer wheel, same as epoll backend)
    i32 timer_fd = -1;
    u64 timer_ticks_buf = 0;        // read target for IORING_OP_READ on timer_fd
    bool timer_read_armed = false;  // true if a timer read SQE is in-flight

    // Outstanding partial-send state per connection.
    // When IORING_OP_SEND completes partially, wait() re-submits the remainder.
    // Only emits Send completion when all bytes are sent (or error).
    static constexpr u32 kMaxSendState = 16384;
    struct SendState {
        const u8* src;
        i32 fd;
        u32 offset;
        u32 remaining;
    };
    SendState send_state[kMaxSendState];

    // Pending SQE count (for submission)
    u32 pending = 0;

    // --- Interface methods ---

    // Initialize the io_uring instance for this shard.
    core::Expected<void, Error> init(u32 shard_id, i32 listen_fd);

    // Submit a multishot accept on the listen socket.
    void add_accept();

    // Submit a multishot recv with provided buffer selection.
    // No user buffer needed — kernel picks from provided ring.
    void add_recv(i32 fd, u32 conn_id);

    // Submit a send (or zero-copy send).
    void add_send(i32 fd, u32 conn_id, const u8* buf, u32 len);

    // Submit a connect to upstream.
    void add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len);

    // Cancel an outstanding operation.
    void cancel(i32 fd, u32 conn_id);

    // Wait for completions. Returns number of events filled.
    // Calls io_uring_enter to submit pending SQEs and wait for CQEs.
    // conns: connection table — recv completions using provided buffers are
    // copied into conns[conn_id].recv_buf. max_conns: table size for bounds checking.
    u32 wait(IoEvent* events, u32 max_events, Connection* conns, u32 max_conns);

    // Shutdown and unmap all resources.
    void shutdown();

    // Return a provided buffer back to the ring after processing.
    void return_buffer(u16 buf_id);

private:
    // Get next available SQE. Returns nullptr if SQ is full.
    io_uring_sqe* get_sqe();

    // Encode conn_id + event type into SQE user_data.
    static u64 encode_user_data(u32 conn_id, IoEventType type);

    // Decode user_data back into conn_id + event type.
    static void decode_user_data(u64 data, u32& conn_id, IoEventType& type);

    // Setup provided buffer ring via io_uring_register.
    core::Expected<void, Error> setup_buf_ring();

    // Submit IORING_OP_READ on timer_fd to receive next tick.
    void submit_timer_read();
};

}  // namespace rout
