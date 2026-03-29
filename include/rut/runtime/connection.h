#pragma once

#include "rut/common/buffer.h"
#include "rut/common/types.h"
#include "rut/runtime/chunked_parser.h"
#include "rut/runtime/io_event.h"

namespace rut {

enum class BodyMode : u8 {
    None,           // No body
    ContentLength,  // Known size via Content-Length
    Chunked,        // Transfer-Encoding: chunked
    UntilClose,     // Read until EOF (HTTP/1.0)
};

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
    static constexpr u16 kMaxPipelineDepth = 16;
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

    // HTTP pipelining state
    u16 pipeline_depth;      // pipelined requests processed on this connection
    u16 pipeline_stash_len;  // bytes of next request stashed in send_buf (proxy)

    // Body streaming state (proxy large body support)
    u32 req_header_end;        // offset past request headers (\r\n\r\n)
    u32 req_content_length;    // original Content-Length value (for send capping)
    u32 req_initial_send_len;  // max bytes to send in initial upstream forward
    bool req_malformed;        // true if request body is malformed (reject)
    BodyMode req_body_mode;
    u32 req_body_remaining;          // bytes left for request body (Content-Length)
    ChunkedParser req_chunk_parser;  // for chunked request body end detection
    BodyMode resp_body_mode;
    u32 resp_body_remaining;          // bytes left for Content-Length mode
    ChunkedParser resp_chunk_parser;  // for chunked mode end detection
    u32 resp_body_sent;               // total response body bytes sent (for access log)

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
        pipeline_depth = 0;
        pipeline_stash_len = 0;
        req_header_end = 0;
        req_content_length = 0;
        req_initial_send_len = 0;
        req_malformed = false;
        req_body_mode = BodyMode::None;
        req_body_remaining = 0;
        req_chunk_parser.reset();
        resp_body_mode = BodyMode::None;
        resp_body_remaining = 0;
        resp_chunk_parser.reset();
        resp_body_sent = 0;
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
