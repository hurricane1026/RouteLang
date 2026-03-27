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

    // Response status (set by handler/proxy, used by access log)
    u16 resp_status;

    // Access-log metadata captured from the request/peer.
    u8 req_method;
    u32 req_size;
    u32 peer_addr;
    char req_path[64];

    // Proxy timing/name for access log.
    u32 upstream_us;
    char upstream_name[24];
    u64 upstream_start_us;

    // Request timing (for access log)
    u64 req_start_us;

    // Recv buffer: Buffer wraps recv_storage_ for typestate-safe I/O.
    // Backend recv's directly via write_ptr()/commit(); callbacks read via data()/len().
    u8 recv_storage[4096];
    Buffer recv_buf;

    // Send buffer: Buffer wraps send_storage for typestate-safe I/O.
    // Callbacks write via write()/data(); backend reads via data()/len() for send().
    u8 send_storage[4096];
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
        resp_status = 0;
        req_method = 0;
        req_size = 0;
        peer_addr = 0;
        req_path[0] = '\0';
        upstream_us = 0;
        upstream_name[0] = '\0';
        upstream_start_us = 0;
        req_start_us = 0;
        recv_buf.bind(recv_storage, sizeof(recv_storage));
        send_buf.bind(send_storage, sizeof(send_storage));
    }
};

}  // namespace rut
