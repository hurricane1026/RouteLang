#pragma once

#include "rout/common/types.h"
#include "rout/runtime/io_event.h"

namespace rout {

enum class ConnState : u8 {
    Idle,
    ReadingHeader,
    ReadingBody,
    ExecHandler,
    Proxying,
    Sending,
};

struct Connection {
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

    // Buffers (simplified for Phase 1)
    u8 recv_buf[4096];
    u32 recv_len;
    u8 send_buf[4096];
    u32 send_len;

    void reset() {
        on_complete = nullptr;
        fd = -1;
        id = 0;
        state = ConnState::Idle;
        flags = 0;
        timer_slot = 0;
        timer_node.init();
        idle_node.init();
        upstream_fd = -1;
        upstream_idx = 0;
        handler_state = 0;
        handler_ctx = nullptr;
        keep_alive = false;
        recv_len = 0;
        send_len = 0;
    }
};

}  // namespace rout
