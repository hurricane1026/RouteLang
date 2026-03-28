#pragma once

#include "rut/common/buffer.h"
#include "rut/common/types.h"
#include "rut/runtime/io_event.h"

namespace rut {

enum class ConnState : u8 {
    Idle,
    ReadingHeader,
    ReadingBody,
    ExecHandler,
    Proxying,
    Sending,
};

struct Connection {
    static constexpr u32 kMaxReqPathLen = 64;
    static constexpr u32 kMaxUpstreamNameLen = 24;
    // fp callback: what to do when the next I/O completes.
    // This IS the state — no enum switch needed.
    // Typed as void* to avoid circular dependency; cast in event_loop.h.
    void (*on_complete)(void* loop, Connection& conn, IoEvent ev);

    i32 fd;
    u32 id;
    ConnState state;  // for debugging/metrics only
    u8 shard_id;
    u16 flags;
    u32 timer_slot;
    ListNode timer_node;
    ListNode idle_node;

    // Upstream (only when proxying)
    i32 upstream_fd;
    u16 upstream_idx;

    // JIT handler state
    u16 handler_state;
    void* handler_ctx;

    bool keep_alive;

    // io_uring multishot recv tracking: true while the multishot SQE is
    // armed in the kernel (set on submit, cleared on final CQE without
    // IORING_CQE_F_MORE). Separate flags for client and upstream to avoid
    // an upstream recv CQE clearing the client's armed state.
    bool recv_armed;
    bool send_armed;
    bool upstream_recv_armed;
    bool upstream_send_armed;

    // Response status (set by handler/proxy, used by access log)
    u16 resp_status;

    // Access-log metadata captured from the request/peer.
    u8 req_method;
    u32 req_size;
    u32 peer_addr;
    char req_path[kMaxReqPathLen];

    // Proxy timing/name for access log.
    u32 upstream_us;
    char upstream_name[kMaxUpstreamNameLen];
    u64 upstream_start_us;

    // Request timing (for access log)
    u64 req_start_us;

    // Outstanding I/O ops submitted to the backend. Incremented on
    // submit_recv/submit_send/etc., decremented when the final CQE
    // arrives in dispatch() (multishot CQEs with IORING_CQE_F_MORE
    // don't decrement). Used to drive CQE-based slice reclamation:
    // a closed connection's pooled slices are only returned to the pool
    // after all in-flight ops have completed (pending_ops reaches 0).
    //
    // u32: multishot recv stays armed across keep-alive cycles, but
    // on_response_sent re-submits submit_recv each cycle, growing the
    // counter by ~1 per request. u32 avoids wraparound (~4B requests).
    // The proper fix is to not re-arm multishot recv on keep-alive.
    u32 pending_ops;

    // Recv/send buffers — backed by SlicePool slices (16KB each).
    // Slices are allocated in EventLoop::alloc_conn_impl() and freed in free_conn_impl().
    // Idle/free connections hold nullptr (zero buffer memory).
    u8* recv_slice;
    u8* send_slice;
    Buffer recv_buf;
    Buffer send_buf;

    void reset() {
        on_complete = nullptr;
        fd = -1;
        id = 0;
        state = ConnState::Idle;
        shard_id = 0;
        flags = 0;
        timer_slot = 0;
        timer_node.init();
        idle_node.init();
        upstream_fd = -1;
        upstream_idx = 0;
        handler_state = 0;
        handler_ctx = nullptr;
        keep_alive = false;
        recv_armed = false;
        send_armed = false;
        upstream_recv_armed = false;
        upstream_send_armed = false;
        resp_status = 0;
        req_method = 0;
        req_size = 0;
        peer_addr = 0;
        req_path[0] = '\0';
        upstream_us = 0;
        upstream_name[0] = '\0';
        upstream_start_us = 0;
        req_start_us = 0;
        pending_ops = 0;
        recv_slice = nullptr;
        send_slice = nullptr;
        recv_buf.bind(nullptr, 0);
        send_buf.bind(nullptr, 0);
    }
};

}  // namespace rut
