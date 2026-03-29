#pragma once

#include "rut/runtime/access_log.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/http_parser.h"
#include "rut/runtime/io_event.h"
#include "rut/runtime/metrics.h"

namespace rut {

// HTTP status codes used in callbacks.
static constexpr u16 kStatusOK = 200;
static constexpr u16 kStatusBadGateway = 502;

// Minimum bytes for a valid HTTP response status line ("HTTP/1.x NNN").
static constexpr u32 kMinResponseLen = 12;

// Length of "Connection: close\r\n".
static constexpr u32 kConnCloseLen = 19;

// Length of the header terminator "\r\n\r\n".
static constexpr u32 kHeaderEndLen = 4;

// Callbacks are template functions parameterized on the concrete EventLoop type.
// When EventLoop<Backend> sets conn.on_complete, it instantiates the callback
// for its own type. The static_cast inside is free — the compiler inlines
// the loop method calls directly. Zero virtual dispatch.
//
// The function pointer signature is still void(*)(void*, Connection&, IoEvent)
// for type-erased storage in Connection, but each instantiation is a concrete
// non-virtual call chain.

template <typename Loop>
void on_header_received(void* lp, Connection& conn, IoEvent ev);

template <typename Loop>
void on_response_sent(void* lp, Connection& conn, IoEvent ev);

// Proxy callbacks: connect upstream → send request → recv response → send to client
template <typename Loop>
void on_upstream_connected(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_upstream_request_sent(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_proxy_response_sent(void* lp, Connection& conn, IoEvent ev);

// Body streaming callbacks: multi-pass recv→send for large bodies
template <typename Loop>
void on_response_header_sent(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_response_body_recvd(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_response_body_sent(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_request_body_sent(void* lp, Connection& conn, IoEvent ev);
template <typename Loop>
void on_request_body_recvd(void* lp, Connection& conn, IoEvent ev);

// --- Implementations (header-only for inlining) ---

static inline u8 ascii_lower(u8 c) {
    if (c >= 'A' && c <= 'Z') return static_cast<u8>(c + ('a' - 'A'));
    return c;
}

static inline bool ascii_ci_eq(const u8* data, const char* lit, u32 len) {
    for (u32 i = 0; i < len; i++) {
        if (ascii_lower(data[i]) != ascii_lower(static_cast<u8>(lit[i]))) return false;
    }
    return true;
}

static inline u8 map_log_method(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET:
            return static_cast<u8>(LogHttpMethod::Get);
        case HttpMethod::POST:
            return static_cast<u8>(LogHttpMethod::Post);
        case HttpMethod::PUT:
            return static_cast<u8>(LogHttpMethod::Put);
        case HttpMethod::DELETE:
            return static_cast<u8>(LogHttpMethod::Delete);
        case HttpMethod::PATCH:
            return static_cast<u8>(LogHttpMethod::Patch);
        case HttpMethod::HEAD:
            return static_cast<u8>(LogHttpMethod::Head);
        case HttpMethod::OPTIONS:
            return static_cast<u8>(LogHttpMethod::Options);
        case HttpMethod::CONNECT:
            return static_cast<u8>(LogHttpMethod::Connect);
        case HttpMethod::TRACE:
            return static_cast<u8>(LogHttpMethod::Trace);
        case HttpMethod::Unknown:
            return static_cast<u8>(LogHttpMethod::Other);
    }
    return static_cast<u8>(LogHttpMethod::Other);
}

static inline u8 parse_log_method_fallback(const u8* data, u32 len, u32* method_len) {
    *method_len = 0;
    if (len >= 4 && data[0] == 'G' && data[1] == 'E' && data[2] == 'T' && data[3] == ' ') {
        *method_len = 3;
        return static_cast<u8>(LogHttpMethod::Get);
    }
    if (len >= 5 && data[0] == 'P' && data[1] == 'O' && data[2] == 'S' && data[3] == 'T' &&
        data[4] == ' ') {
        *method_len = 4;
        return static_cast<u8>(LogHttpMethod::Post);
    }
    if (len >= 4 && data[0] == 'P' && data[1] == 'U' && data[2] == 'T' && data[3] == ' ') {
        *method_len = 3;
        return static_cast<u8>(LogHttpMethod::Put);
    }
    if (len >= 7 && data[0] == 'D' && data[1] == 'E' && data[2] == 'L' && data[3] == 'E' &&
        data[4] == 'T' && data[5] == 'E' && data[6] == ' ') {
        *method_len = 6;
        return static_cast<u8>(LogHttpMethod::Delete);
    }
    if (len >= 6 && data[0] == 'P' && data[1] == 'A' && data[2] == 'T' && data[3] == 'C' &&
        data[4] == 'H' && data[5] == ' ') {
        *method_len = 5;
        return static_cast<u8>(LogHttpMethod::Patch);
    }
    if (len >= 5 && data[0] == 'H' && data[1] == 'E' && data[2] == 'A' && data[3] == 'D' &&
        data[4] == ' ') {
        *method_len = 4;
        return static_cast<u8>(LogHttpMethod::Head);
    }
    if (len >= 8 && data[0] == 'O' && data[1] == 'P' && data[2] == 'T' && data[3] == 'I' &&
        data[4] == 'O' && data[5] == 'N' && data[6] == 'S' && data[7] == ' ') {
        *method_len = 7;
        return static_cast<u8>(LogHttpMethod::Options);
    }
    if (len >= 8 && data[0] == 'C' && data[1] == 'O' && data[2] == 'N' && data[3] == 'N' &&
        data[4] == 'E' && data[5] == 'C' && data[6] == 'T' && data[7] == ' ') {
        *method_len = 7;
        return static_cast<u8>(LogHttpMethod::Connect);
    }
    if (len >= 6 && data[0] == 'T' && data[1] == 'R' && data[2] == 'A' && data[3] == 'C' &&
        data[4] == 'E' && data[5] == ' ') {
        *method_len = 5;
        return static_cast<u8>(LogHttpMethod::Trace);
    }
    return static_cast<u8>(LogHttpMethod::Other);
}

static inline void capture_request_metadata(Connection& conn) {
    conn.req_method = static_cast<u8>(LogHttpMethod::Other);
    conn.req_size = conn.recv_buf.len();
    conn.req_path[0] = '/';
    conn.req_path[1] = '\0';
    conn.upstream_us = 0;
    conn.upstream_name[0] = '\0';
    // Reset request body state (prevents stale Chunked mode from
    // previous keep-alive request bleeding into the next).
    conn.req_body_mode = BodyMode::None;
    conn.req_body_remaining = 0;
    conn.req_chunk_parser.reset();
    conn.req_malformed = false;
    conn.req_header_end = 0;
    conn.req_initial_send_len = 0;
    conn.req_content_length = 0;

    const u8* data = conn.recv_buf.data();
    u32 len = conn.recv_buf.len();
    if (!data || len == 0) return;

    HttpParser parser;
    ParsedRequest req;
    parser.reset();
    if (parser.parse(data, len, &req) == ParseStatus::Complete) {
        conn.req_header_end = parser.header_end;
        conn.req_method = map_log_method(req.method);
        u32 copy_len = req.path.len;
        if (copy_len >= sizeof(conn.req_path)) copy_len = sizeof(conn.req_path) - 1;
        for (u32 i = 0; i < copy_len; i++) conn.req_path[i] = req.path.ptr[i];
        conn.req_path[copy_len] = '\0';
        // Set request body mode for proxy streaming.
        u32 chunk_consumed = 0;  // bytes consumed by chunk parser in initial body
        if (req.chunked) {
            conn.req_body_mode = BodyMode::Chunked;
            conn.req_body_remaining = 0;
            conn.req_chunk_parser.reset();
            // Feed any body bytes already in the buffer through the
            // chunk parser so we detect if the body is complete.
            u32 body_in_buf = len > parser.header_end ? len - parser.header_end : 0;
            chunk_consumed = body_in_buf;  // default: all bytes parsed
            if (body_in_buf > 0) {
                const u8* body_start = data + parser.header_end;
                u32 pos = 0;
                while (pos < body_in_buf) {
                    u32 consumed = 0, out_start = 0, out_len = 0;
                    ChunkStatus cs = conn.req_chunk_parser.feed(
                        body_start + pos, body_in_buf - pos, &consumed, &out_start, &out_len);
                    pos += consumed;
                    if (cs == ChunkStatus::Done || cs == ChunkStatus::NeedMore) break;
                    if (cs == ChunkStatus::Error) {
                        conn.req_malformed = true;
                        break;
                    }
                }
                chunk_consumed = pos;
            }
            // Set initial send len. chunk_consumed == 0 with body_in_buf > 0
            // means chunk error → set to 0 as rejection sentinel.
            if (chunk_consumed == 0 && conn.req_body_mode == BodyMode::None) {
                conn.req_initial_send_len = 0;  // malformed → reject
            } else {
                conn.req_initial_send_len = parser.header_end + chunk_consumed;
            }
        } else if (req.has_content_length && req.content_length > 0) {
            conn.req_body_mode = BodyMode::ContentLength;
            conn.req_content_length = req.content_length;
            conn.req_body_remaining = req.content_length;
            // Deduct body bytes already in recv_buf after headers.
            u32 body_in_buf = len > parser.header_end ? len - parser.header_end : 0;
            if (body_in_buf >= conn.req_body_remaining)
                conn.req_body_remaining = 0;
            else
                conn.req_body_remaining -= body_in_buf;
        }
        // Compute initial send length for all modes.
        if (conn.req_body_mode == BodyMode::None) {
            conn.req_initial_send_len = parser.header_end;
        } else if (conn.req_body_mode == BodyMode::ContentLength) {
            u32 body_in_initial = conn.req_content_length - conn.req_body_remaining;
            conn.req_initial_send_len = parser.header_end + body_in_initial;
        }
        // Chunked: already set above (parser.header_end + chunk_consumed).
        // Fix req_size: use actual request boundary, not full recv_buf
        // (which may include pipelined bytes from subsequent requests).
        if (conn.req_initial_send_len > 0) conn.req_size = conn.req_initial_send_len;
        return;
    }

    u32 method_len = 0;
    conn.req_method = parse_log_method_fallback(data, len, &method_len);
    if (method_len == 0 || method_len + 1 >= len || data[method_len] != ' ') return;

    u32 path_start = method_len + 1;
    u32 path_len = 0;
    while (path_start + path_len < len && data[path_start + path_len] != ' ' &&
           data[path_start + path_len] != '\r' && data[path_start + path_len] != '\n') {
        path_len++;
    }

    if (path_len == 0 || data[path_start] != '/') return;

    u32 copy_len = path_len;
    if (copy_len >= sizeof(conn.req_path)) copy_len = sizeof(conn.req_path) - 1;
    for (u32 i = 0; i < copy_len; i++) conn.req_path[i] = static_cast<char>(data[path_start + i]);
    conn.req_path[copy_len] = '\0';
}

// --- HTTP pipelining helpers ---

// How many leftover bytes past the current request in recv_buf.
static inline u32 pipeline_leftover(const Connection& conn) {
    u32 req_end = conn.req_initial_send_len;
    u32 buf_len = conn.recv_buf.len();
    if (req_end == 0 || req_end >= buf_len) return 0;
    return buf_len - req_end;
}

// Shift leftover bytes to recv_buf start. Returns true if data shifted.
static inline bool pipeline_shift(Connection& conn) {
    u32 leftover = pipeline_leftover(conn);
    if (leftover == 0) return 0;
    const u8* src = conn.recv_buf.data() + conn.req_initial_send_len;
    conn.recv_buf.reset();
    u8* dst = conn.recv_buf.write_ptr();
    __builtin_memmove(dst, src, leftover);
    conn.recv_buf.commit(leftover);
    conn.pipeline_depth++;
    return true;
}

// Re-enter header parsing with data already in recv_buf.
template <typename Loop>
static inline void pipeline_dispatch(Loop* loop, Connection& conn) {
    conn.state = ConnState::ReadingHeader;
    // Refresh keepalive timer — synthetic dispatch skips the normal
    // EventLoop::dispatch() which calls timer.refresh().
    loop->timer.refresh(&conn, loop->keepalive_timeout);
    IoEvent synth = {conn.id, static_cast<i32>(conn.recv_buf.len()), 0, 0, IoEventType::Recv, 0};
    on_header_received<Loop>(loop, conn, synth);
}

// Stash leftover bytes from recv_buf into send_buf (proxy path).
static inline void pipeline_stash(Connection& conn) {
    u32 leftover = pipeline_leftover(conn);
    if (leftover == 0) {
        conn.pipeline_stash_len = 0;
        return;
    }
    // Reset send_buf BEFORE checking capacity (old response data may occupy it).
    conn.send_buf.reset();
    if (leftover > conn.send_buf.write_avail()) {
        conn.pipeline_stash_len = 0;
        return;
    }
    const u8* src = conn.recv_buf.data() + conn.req_initial_send_len;
    conn.send_buf.write(src, leftover);
    conn.pipeline_stash_len = static_cast<u16>(leftover);
}

// Recover stashed bytes from send_buf into recv_buf. Returns true if recovered.
static inline bool pipeline_recover(Connection& conn) {
    u16 stash_len = conn.pipeline_stash_len;
    conn.pipeline_stash_len = 0;
    if (stash_len == 0) return 0;
    const u8* src = conn.send_buf.data();
    conn.recv_buf.reset();
    u8* dst = conn.recv_buf.write_ptr();
    __builtin_memmove(dst, src, stash_len);
    conn.recv_buf.commit(stash_len);
    conn.send_buf.reset();
    conn.pipeline_depth++;
    return true;
}

static const char kResponse200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

static const char kResponse200Close[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

// Called on request completion — records metrics and writes access log.
template <typename Loop>
void on_request_complete(Loop* loop, Connection& conn, u16 status, u32 resp_size) {
    u32 duration_us = static_cast<u32>(monotonic_us() - conn.req_start_us);

    // Clear req_start_us so close_conn_impl knows no request is in flight.
    conn.req_start_us = 0;

    // Record metrics (if enabled).
    if (loop->metrics) {
        loop->metrics->on_request_complete(duration_us);
    }

    // Write access log entry (if enabled).
    if (loop->access_log) {
        AccessLogEntry entry{};
        entry.timestamp_us = realtime_us();
        entry.duration_us = duration_us;
        entry.status = status;
        entry.method = conn.req_method;
        entry.shard_id = conn.shard_id;
        entry.resp_size = resp_size;
        entry.req_size = conn.req_size;
        entry.addr = conn.peer_addr;
        entry.upstream_us = conn.upstream_us;
        for (u32 i = 0; i < sizeof(entry.path); i++) {
            entry.path[i] = conn.req_path[i];
            if (conn.req_path[i] == '\0') break;
        }
        for (u32 i = 0; i < sizeof(entry.upstream); i++) {
            entry.upstream[i] = conn.upstream_name[i];
            if (conn.upstream_name[i] == '\0') break;
        }
        loop->access_log->push(entry);
    }
}

template <typename Loop>
void on_header_received(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    // Guard: unexpected event type.
    if (ev.type != IoEventType::Recv) {
        // Stale UpstreamRecv/Send CQEs from a previous proxy request.
        // With separate buffers, UpstreamRecv data goes to upstream_recv_buf
        // (not recv_buf), so no purge needed — just ignore.
        if (ev.type == IoEventType::UpstreamRecv) return;
        if (ev.type == IoEventType::UpstreamSend) return;
        loop->close_conn(conn);
        return;
    }

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    // NOTE: recv_buf is NOT reset here — proxy flow needs recv_buf.data()
    // for upstream forwarding. Reset happens in on_response_sent / on_proxy_response_sent
    // when the cycle completes and we're about to read the next request.

    // Check if the buffer contains a complete request before proceeding.
    // For pipelined re-entries, an incomplete request should wait for more
    // data instead of getting a spurious default response.
    if (conn.pipeline_depth > 0) {
        HttpParser pre_parser;
        ParsedRequest pre_req;
        pre_parser.reset();
        if (pre_parser.parse(conn.recv_buf.data(), conn.recv_buf.len(), &pre_req) ==
            ParseStatus::Incomplete) {
            // Keep pipeline_depth > 0 so subsequent recvs also check for
            // Incomplete (multi-packet reassembly of the pipelined request).
            conn.state = ConnState::ReadingHeader;
            conn.on_complete = &on_header_received<Loop>;
            loop->submit_recv(conn);
            return;
        }
    }

    capture_request_metadata(conn);

    conn.req_start_us = monotonic_us();
    loop->epoch_enter();
    if (loop->metrics) loop->metrics->on_request_start();
    conn.state = ConnState::Sending;
    conn.resp_status = kStatusOK;

    // During drain: respond with Connection: close to signal client migration.
    if (loop->is_draining()) {
        conn.keep_alive = false;
        conn.send_buf.reset();
        conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200Close),
                            sizeof(kResponse200Close) - 1);
    } else {
        conn.keep_alive = true;
        conn.send_buf.reset();
        conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200), sizeof(kResponse200) - 1);
    }

    conn.on_complete = &on_response_sent<Loop>;
    loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
}

template <typename Loop>
void on_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
        // Pipelined client Recv with data: bytes are in recv_buf,
        // found by pipeline_shift after send completes.
        // EOF/error Recv: close immediately (peer disconnected).
        if (ev.type == IoEventType::Recv && ev.result > 0) return;
        loop->close_conn(conn);
        return;
    }

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    // Validate full send — partial sends (possible with io_uring) would serve
    // truncated responses. Close rather than risk corruption on keep-alive.
    if (static_cast<u32>(ev.result) != conn.send_buf.len()) {
        loop->close_conn(conn);
        return;
    }

    // Record metrics + access log only after send is confirmed successful.
    on_request_complete(loop, conn, conn.resp_status, conn.send_buf.len());
    loop->epoch_leave();

    if (!conn.keep_alive) {
        loop->close_conn(conn);
        return;
    }

    // Request cycle complete — check for pipelined data before re-arming recv.
    if (pipeline_shift(conn)) {
        pipeline_dispatch<Loop>(loop, conn);
        return;
    }
    conn.pipeline_depth = 0;
    conn.recv_buf.reset();
    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

// --- Proxy callbacks ---
// Flow: recv request → connect upstream → send request → recv response → send to client

// Step: upstream TCP connect completed.
template <typename Loop>
void on_upstream_connected(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::UpstreamConnect) {
        loop->close_conn(conn);
        return;
    }

    if (ev.result < 0) {
        // Upstream connect failed → 502
        static const char k502[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Bad Gateway";
        conn.send_buf.reset();
        conn.send_buf.write(reinterpret_cast<const u8*>(k502), sizeof(k502) - 1);
        conn.keep_alive = false;  // Connection: close — don't loop back
        conn.resp_status = kStatusBadGateway;
        conn.on_complete = &on_response_sent<Loop>;
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
        return;
    }

    // Reject malformed requests (e.g., invalid chunked body).
    if (conn.req_malformed) {
        loop->close_conn(conn);
        return;
    }

    // Allocate upstream recv buffer for this proxy connection.
    // This is lazy — only proxy connections pay the cost.
    if (!loop->alloc_upstream_buf(conn)) {
        // Pool exhausted — can't proxy, close.
        loop->close_conn(conn);
        return;
    }

    conn.state = ConnState::Proxying;
    // Use pre-computed initial send length (capped to request boundary).
    u32 req_send_len =
        conn.req_initial_send_len > 0 ? conn.req_initial_send_len : conn.recv_buf.len();
    if (req_send_len > conn.recv_buf.len()) req_send_len = conn.recv_buf.len();
    conn.on_complete = &on_upstream_request_sent<Loop>;
    loop->submit_send_upstream(conn, conn.recv_buf.data(), req_send_len);
}

// Step: request forwarded to upstream, now wait for upstream response.
template <typename Loop>
void on_upstream_request_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send && ev.type != IoEventType::UpstreamSend) {
        loop->close_conn(conn);
        return;
    }

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    // Backends guarantee full send of submitted length. ev.result > 0
    // is sufficient — the send may be capped to body boundary (less
    // than recv_buf.len() when pipelined bytes are present).

    // Check if there's more request body to stream from the client.
    // For Content-Length: remaining > 0. For Chunked: always stream
    // (end detected by chunk parser, not by a byte count).
    bool more_req_body =
        (conn.req_body_mode == BodyMode::ContentLength && conn.req_body_remaining > 0) ||
        (conn.req_body_mode == BodyMode::Chunked &&
         conn.req_chunk_parser.state != ChunkedParser::State::Complete);
    if (more_req_body) {
        // More body to read from client and forward to upstream.
        // Also arm upstream recv to detect early error responses
        // (401/413) before body is fully sent.
        conn.recv_buf.reset();
        conn.on_complete = &on_request_body_recvd<Loop>;
        loop->submit_recv(conn);
        loop->submit_recv_upstream(conn);
        return;
    }

    // Stash pipelined bytes before waiting for upstream response.
    pipeline_stash(conn);
    conn.recv_buf.reset();  // clear after stash — pipelined bytes are in send_buf
    conn.upstream_start_us = monotonic_us();
    conn.upstream_recv_buf.reset();
    conn.on_complete = &on_upstream_response<Loop>;
    loop->submit_recv_upstream(conn);
}

// --- Response body streaming callbacks ---
// Multi-pass cycle: recv from upstream → send to client → recv more → until done.

// Called after the initial response headers + body fragment have been sent to client.
template <typename Loop>
void on_response_header_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
        // Stale UpstreamSend from body streaming interrupted by early response.
        if (ev.type == IoEventType::UpstreamSend) return;
        // Stale UpstreamRecv: upstream recv stays armed from body streaming and
        // may deliver early response body data. Ignore (response is already
        // being forwarded from upstream_recv_buf).
        if (ev.type == IoEventType::UpstreamRecv) return;
        // Client Recv: client may still be sending upload body. Drain and
        // re-arm to prevent recv_buf filling (16KB) → -ENOBUFS → premature close.
        if (ev.type == IoEventType::Recv) {
            if (ev.result > 0) {
                conn.recv_buf.reset();
                loop->submit_recv(conn);
                return;
            }
            loop->close_conn(conn);
            return;
        }
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }
    // Both backends guarantee full sends (partial sends are retried
    // internally in wait()). ev.result > 0 is sufficient — a short
    // write never reaches callbacks.

    // More body to stream — recv next chunk from upstream.
    conn.upstream_recv_buf.reset();
    conn.on_complete = &on_response_body_recvd<Loop>;
    loop->submit_recv_upstream(conn);
}

// Called when more response body data arrives from upstream.
template <typename Loop>
void on_response_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::UpstreamRecv) {
        // Client Recv during response streaming: data goes to recv_buf,
        // not upstream_recv_buf. Ignore but re-arm if multishot terminated
        // (io_uring dispatch may have cleared recv_armed on ev.more=0).
        // Don't close on client EOF — may be half-close (SHUT_WR) while
        // client still reads the response. Re-arm for data; ignore EOF.
        if (ev.type == IoEventType::Recv) {
            if (ev.result > 0) loop->submit_recv(conn);
            return;
        }
        loop->close_conn(conn);
        return;
    }

    if (ev.result <= 0) {
        // EOF or error from upstream.
        if (conn.resp_body_mode == BodyMode::UntilClose) {
            // UntilClose: EOF = body done. Must close client connection —
            // the client uses EOF to detect body end, so keep-alive is
            // impossible (RFC 7230 §3.3.3).
            on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
            loop->epoch_leave();
            loop->close_conn(conn);
            return;
        }
        // For Content-Length or Chunked, premature EOF is an error.
        loop->close_conn(conn);
        return;
    }

    u32 data_len = conn.upstream_recv_buf.len();
    // How many bytes to forward (capped to body boundary for CL mode).
    u32 send_len = data_len;

    if (conn.resp_body_mode == BodyMode::ContentLength) {
        u32 consume = data_len;
        if (consume > conn.resp_body_remaining) consume = conn.resp_body_remaining;
        conn.resp_body_remaining -= consume;
        send_len = consume;
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        // Feed through chunk parser to detect the end marker.
        // Forward raw bytes as-is (proxy pass-through, no decode).
        const u8* body_data = conn.upstream_recv_buf.data();
        u32 pos = 0;
        while (pos < data_len) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            ChunkStatus cs = conn.resp_chunk_parser.feed(
                body_data + pos, data_len - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (cs == ChunkStatus::Done) break;
            if (cs == ChunkStatus::Error) {
                loop->close_conn(conn);
                return;
            }
            if (cs == ChunkStatus::NeedMore) break;
        }
        send_len = pos;  // cap at chunk parser boundary
    }
    // UntilClose: end detected by EOF in the ev.result <= 0 check above.

    conn.resp_body_sent += send_len;

    // Forward upstream body data to client (capped to body boundary).
    conn.on_complete = &on_response_body_sent<Loop>;
    conn.state = ConnState::Sending;
    loop->submit_send(conn, conn.upstream_recv_buf.data(), send_len);
}

// Called when a response body chunk has been sent to client.
template <typename Loop>
void on_response_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
        // Stale UpstreamSend from body streaming interrupted by early response.
        if (ev.type == IoEventType::UpstreamSend) return;
        // Stale UpstreamRecv: upstream recv armed from body streaming.
        if (ev.type == IoEventType::UpstreamRecv) return;
        // Client Recv: drain and re-arm.
        if (ev.type == IoEventType::Recv) {
            if (ev.result > 0) {
                conn.recv_buf.reset();
                loop->submit_recv(conn);
                return;
            }
            loop->close_conn(conn);
            return;
        }
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    // Check if body is complete.
    bool body_done = false;
    if (conn.resp_body_mode == BodyMode::ContentLength) {
        body_done = (conn.resp_body_remaining == 0);
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        body_done = (conn.resp_chunk_parser.state == ChunkedParser::State::Complete);
    }
    // UntilClose: never done here; on_response_body_recvd handles EOF.

    if (body_done) {
        // Body streaming complete — reset upstream buffer (keep slice for reuse).
        conn.upstream_recv_buf.reset();

        on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
        loop->epoch_leave();

        if (conn.upstream_fd >= 0) {
            ::close(conn.upstream_fd);
            conn.upstream_fd = -1;
        }
        loop->clear_upstream_fd(conn.id);
        conn.upstream_recv_armed = false;
        conn.upstream_send_armed = false;

        if (!conn.keep_alive || loop->is_draining()) {
            loop->close_conn(conn);
            return;
        }

        if (conn.pipeline_stash_len > 0 && conn.recv_buf.len() > 0) {
            // Both stash (early) and recv_buf (late) have data.
            // Correct order: stash first, then late bytes.
            u16 slen = conn.pipeline_stash_len;
            u32 late_len = conn.recv_buf.len();
            // Check combined size fits in one buffer slice.
            if (static_cast<u32>(slen) + late_len > conn.recv_buf.capacity()) {
                // Overflow: can't merge without truncation. Close.
                conn.pipeline_stash_len = 0;
                conn.send_buf.reset();
                loop->close_conn(conn);
                return;
            }
            conn.pipeline_stash_len = 0;
            conn.upstream_recv_buf.reset();
            conn.upstream_recv_buf.write(conn.recv_buf.data(), late_len);
            conn.recv_buf.reset();
            conn.recv_buf.write(conn.send_buf.data(), slen);
            conn.recv_buf.write(conn.upstream_recv_buf.data(), late_len);
            conn.upstream_recv_buf.reset();
            conn.send_buf.reset();
            conn.pipeline_depth++;
            pipeline_dispatch<Loop>(loop, conn);
            return;
        }
        if (pipeline_recover(conn)) {
            pipeline_dispatch<Loop>(loop, conn);
            return;
        }
        if (conn.recv_buf.len() > 0) {
            conn.pipeline_depth++;
            pipeline_dispatch<Loop>(loop, conn);
            return;
        }
        conn.pipeline_depth = 0;
        conn.recv_buf.reset();
        conn.state = ConnState::ReadingHeader;
        conn.on_complete = &on_header_received<Loop>;
        loop->submit_recv(conn);
        return;
    }

    // More body to stream — recv next chunk from upstream.
    conn.upstream_recv_buf.reset();
    conn.on_complete = &on_response_body_recvd<Loop>;
    loop->submit_recv_upstream(conn);
}

// --- Request body streaming callbacks ---
// Multi-pass cycle: recv from client → send to upstream → recv more → until done.
//
// During body streaming, upstream recv is also armed to detect early error
// responses (413/401/etc) before the client body is fully forwarded.

// Helper: handle an UpstreamRecv event that arrives during request body streaming.
// Handles: EOF/error → close/502, 1xx → skip and continue, non-1xx → flag pending.
//
// IMPORTANT: This must NOT call on_upstream_response directly because an
// upstream send may still be in-flight. Epoll reports upstream send completions
// as IoEventType::Send (not UpstreamSend), so transitioning to response
// forwarding while a send is pending would cause the stale send completion
// to be misinterpreted as the client-response send ack.
//
// Instead, non-1xx responses set early_response_pending = true. The caller
// (on_request_body_sent body_done or more_body paths) checks this flag after
// the upstream send settles and transitions to on_upstream_response then.
template <typename Loop>
static inline void handle_early_upstream_recv(Loop* loop, Connection& conn, IoEvent ev) {
    if (ev.result <= 0 && conn.upstream_recv_buf.len() == 0) {
        // Upstream closed with no data — close immediately (no send in-flight
        // concern since the connection is dead anyway).
        loop->close_conn(conn);
        return;  // conn freed, early_response_pending irrelevant
    }
    if (ev.result <= 0) {
        // EOF with partial data — mark pending, on_upstream_response will 502.
        conn.early_response_pending = true;
        return;
    }
    // Parse to determine what it is.
    HttpResponseParser resp_parser;
    ParsedResponse resp;
    resp.reset();
    resp_parser.reset();
    ParseStatus ps =
        resp_parser.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &resp);
    if (ps == ParseStatus::Incomplete) {
        // Partial response header — re-arm and wait for more.
        loop->submit_recv_upstream(conn);
        return;
    }
    if (ps == ParseStatus::Complete && resp.status_code >= 100 && resp.status_code < 200 &&
        resp.status_code != 101) {
        // 1xx interim (e.g., 100 Continue): skip, continue body streaming.
        // Preserve trailing bytes — origin may coalesce 1xx + final response.
        u32 interim_end = resp_parser.header_end;
        u32 total = conn.upstream_recv_buf.len();
        if (interim_end < total) {
            u32 remaining = total - interim_end;
            const u8* src = conn.upstream_recv_buf.data() + interim_end;
            conn.upstream_recv_buf.reset();
            u8* dst = conn.upstream_recv_buf.write_ptr();
            __builtin_memmove(dst, src, remaining);
            conn.upstream_recv_buf.commit(remaining);
            // Re-enter: trailing data might be a full final response.
            handle_early_upstream_recv<Loop>(loop, conn, ev);
            return;
        }
        conn.upstream_recv_buf.reset();
        loop->submit_recv_upstream(conn);
        return;
    }
    // Non-1xx response: flag pending. Don't transition yet — upstream send
    // may still be in-flight. on_request_body_sent will check and transition
    // after the send settles.
    conn.early_response_pending = true;
}

// Transition to forwarding a pending early upstream response.
// Called after the in-flight upstream send has settled.
template <typename Loop>
static inline void forward_early_response(Loop* loop, void* lp, Connection& conn) {
    conn.early_response_pending = false;
    // Discard unread client body bytes — they're upload data, not a pipelined
    // next request. Feeding them into pipeline_dispatch would corrupt the
    // session with spurious parse failures.
    conn.recv_buf.reset();
    conn.keep_alive = false;  // can't reuse: unread body on client side
    conn.upstream_start_us = monotonic_us();
    // Synthesize an UpstreamRecv event. Use result=0 (EOF) if the upstream
    // closed, so on_upstream_response produces 502 for truncated headers.
    // If upstream_recv_buf has a complete response, result>0 tells the parser
    // to proceed normally.
    HttpResponseParser probe;
    ParsedResponse probe_resp;
    probe_resp.reset();
    probe.reset();
    ParseStatus ps =
        probe.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &probe_resp);
    // If parse is Incomplete, the upstream must have closed (EOF with partial data).
    i32 synth_result =
        (ps == ParseStatus::Incomplete) ? 0 : static_cast<i32>(conn.upstream_recv_buf.len());
    IoEvent synth = {conn.id, synth_result, 0, 0, IoEventType::UpstreamRecv, 0};
    on_upstream_response<Loop>(lp, conn, synth);
}

// Called when request body chunk has been sent to upstream.
template <typename Loop>
void on_request_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send && ev.type != IoEventType::UpstreamSend) {
        // Early upstream response arrived while upstream send is in-flight.
        // Don't transition yet — flag it and wait for the send to settle.
        if (ev.type == IoEventType::UpstreamRecv) {
            handle_early_upstream_recv<Loop>(loop, conn, ev);
            return;
        }
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }
    // Upstream send completed. Check if an early response was flagged
    // while the send was in-flight — if so, handle it now.
    if (conn.early_response_pending) {
        forward_early_response<Loop>(loop, lp, conn);
        return;
    }

    // Check if request body is complete.
    bool body_done = false;
    if (conn.req_body_mode == BodyMode::ContentLength) {
        body_done = (conn.req_body_remaining == 0);
    } else if (conn.req_body_mode == BodyMode::Chunked) {
        body_done = (conn.req_chunk_parser.state == ChunkedParser::State::Complete);
    }

    if (body_done) {
        // Request body fully forwarded — stash leftovers, wait for upstream response.
        pipeline_stash(conn);
        conn.recv_buf.reset();
        conn.upstream_start_us = monotonic_us();
        // Don't reset upstream_recv_buf — it may hold a partial early response
        // header that was buffered during body streaming (Incomplete parse).
        if (conn.upstream_recv_buf.len() == 0) conn.upstream_recv_buf.reset();
        conn.on_complete = &on_upstream_response<Loop>;
        loop->submit_recv_upstream(conn);
        return;
    }

    // More body to stream — recv next chunk from client.
    // Also arm upstream recv to detect early error responses (413/401).
    conn.recv_buf.reset();
    conn.on_complete = &on_request_body_recvd<Loop>;
    loop->submit_recv(conn);
    loop->submit_recv_upstream(conn);
}

// Called when more request body data arrives from client.
template <typename Loop>
void on_request_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Recv) {
        // Early upstream response during body streaming (413/401/100 Continue).
        // No upstream send is in-flight here (we haven't submitted one yet),
        // so it's safe to transition immediately if non-1xx.
        if (ev.type == IoEventType::UpstreamRecv) {
            handle_early_upstream_recv<Loop>(loop, conn, ev);
            if (conn.early_response_pending) {
                forward_early_response<Loop>(loop, lp, conn);
            }
            return;
        }
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    u32 data_len = conn.recv_buf.len();

    // Track body consumption and compute how many bytes to forward.
    u32 send_len = data_len;
    if (conn.req_body_mode == BodyMode::ContentLength) {
        u32 consume = data_len;
        if (consume > conn.req_body_remaining) consume = conn.req_body_remaining;
        conn.req_body_remaining -= consume;
        send_len = consume;  // don't forward pipelined bytes past body end
    } else if (conn.req_body_mode == BodyMode::Chunked) {
        const u8* body_data = conn.recv_buf.data();
        u32 pos = 0;
        while (pos < data_len) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            ChunkStatus cs = conn.req_chunk_parser.feed(
                body_data + pos, data_len - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (cs == ChunkStatus::Done) break;
            if (cs == ChunkStatus::Error) {
                loop->close_conn(conn);
                return;
            }
            if (cs == ChunkStatus::NeedMore) break;
        }
        // For chunked, only forward bytes up to what was consumed by the parser.
        // After 0\r\n\r\n, remaining bytes are the next pipelined request.
        send_len = pos;
    }

    conn.req_size += send_len;             // accumulate actual body bytes (excludes pipelined)
    conn.req_initial_send_len = send_len;  // for pipeline_leftover detection
    conn.on_complete = &on_request_body_sent<Loop>;
    loop->submit_send_upstream(conn, conn.recv_buf.data(), send_len);
}

// Step: upstream response received, forward to client.
template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    // Accept UpstreamRecv only — upstream data now goes to upstream_recv_buf,
    // so client Recv events are harmless (they go to recv_buf). Ignore them.
    if (ev.type != IoEventType::UpstreamRecv) {
        // Client Recv with data: pipelined request, harmless (in recv_buf).
        // Client Recv with EOF/error: client disconnected, close proxy.
        if (ev.type == IoEventType::Recv) {
            if (ev.result <= 0) {
                loop->close_conn(conn);
                return;
            }
            loop->submit_recv(conn);  // re-arm if multishot terminated
            return;
        }
        // Stale UpstreamSend from body streaming that was interrupted by
        // an early response. The send completed after we transitioned to
        // response forwarding — harmlessly ignore it.
        if (ev.type == IoEventType::UpstreamSend) return;
        loop->close_conn(conn);
        return;
    }

    if (conn.upstream_start_us != 0) {
        conn.upstream_us = static_cast<u32>(monotonic_us() - conn.upstream_start_us);
        conn.upstream_start_us = 0;
    }

    // EOF or error with no data → close.
    // EOF with partial data → parse will return Incomplete → 502 below.
    if (ev.result <= 0 && conn.upstream_recv_buf.len() == 0) {
        loop->close_conn(conn);
        return;
    }

    // Parse upstream response with proper HTTP parser.
    HttpResponseParser resp_parser;
    ParsedResponse resp;
    resp.reset();
    resp_parser.reset();
    ParseStatus ps =
        resp_parser.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &resp);
    if (ps == ParseStatus::Incomplete) {
        // If upstream closed (EOF), headers are truncated → 502.
        if (ev.result <= 0)
            ps = ParseStatus::Error;
        else {
            loop->submit_recv_upstream(conn);
            return;
        }
    }
    if (ps == ParseStatus::Error) {
        // Malformed/truncated upstream response → 502 Bad Gateway.
        // Close upstream fd first to stop further UpstreamRecv CQEs
        // that would be dispatched to on_response_sent (wrong type → close).
        if (conn.upstream_fd >= 0) {
            ::close(conn.upstream_fd);
            conn.upstream_fd = -1;
        }
        static const char k502[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Bad Gateway";
        conn.send_buf.reset();
        conn.send_buf.write(reinterpret_cast<const u8*>(k502), sizeof(k502) - 1);
        conn.keep_alive = false;
        conn.resp_status = kStatusBadGateway;
        conn.on_complete = &on_response_sent<Loop>;
        conn.state = ConnState::Sending;
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
        return;
    }
    conn.resp_status = resp.status_code;

    // 1xx responses are interim — skip and read the final response.
    // Exception: 101 Switching Protocols is terminal (upgrade handshake).
    if (resp.status_code >= 100 && resp.status_code < 200 && resp.status_code != 101) {
        u32 interim_end = resp_parser.header_end;
        u32 total = conn.upstream_recv_buf.len();
        if (interim_end < total) {
            // Remaining bytes after the 1xx may contain the final response.
            // Shift remaining bytes to buffer start (same backing slice).
            u32 remaining = total - interim_end;
            const u8* src = conn.upstream_recv_buf.data() + interim_end;
            conn.upstream_recv_buf.reset();
            // After reset, write_ptr() points to start of slice.
            u8* dst = conn.upstream_recv_buf.write_ptr();
            __builtin_memmove(dst, src, remaining);
            conn.upstream_recv_buf.commit(remaining);
            // Re-enter to parse the remaining data.
            on_upstream_response<Loop>(lp, conn, ev);
            return;
        }
        // No remaining data — wait for the final response.
        conn.upstream_recv_buf.reset();
        loop->submit_recv_upstream(conn);
        return;
    }

    // Determine body mode based on response characteristics.
    bool is_head = (conn.req_method == static_cast<u8>(LogHttpMethod::Head));
    bool no_body_status =
        resp.status_code == 204 || resp.status_code == 205 || resp.status_code == 304;

    if (is_head || no_body_status) {
        conn.resp_body_mode = BodyMode::None;
        conn.resp_body_remaining = 0;
    } else if (resp.chunked) {
        // RFC 7230 §3.3.3: Transfer-Encoding takes precedence over
        // Content-Length. Ignore Content-Length if both are present.
        conn.resp_body_mode = BodyMode::Chunked;
        conn.resp_chunk_parser.reset();
        conn.resp_body_remaining = 0;
    } else if (resp.has_content_length) {
        conn.resp_body_mode = BodyMode::ContentLength;
        conn.resp_body_remaining = resp.content_length;
    } else {
        // No CL/TE: read until EOF (close-delimited per RFC 7230).
        // Covers both HTTP/1.0 (default close) and non-conformant HTTP/1.1
        // origins that send bodies without framing.
        conn.resp_body_mode = BodyMode::UntilClose;
        conn.resp_body_remaining = 0;
    }

    // Calculate how much body data is already in upstream_recv_buf from the initial recv.
    u32 header_len = resp_parser.header_end;
    u32 total_len = conn.upstream_recv_buf.len();
    u32 initial_body_len = (total_len > header_len) ? total_len - header_len : 0;

    // Track initial body consumption for Content-Length mode.
    if (conn.resp_body_mode == BodyMode::ContentLength && initial_body_len > 0) {
        u32 consume = initial_body_len;
        if (consume > conn.resp_body_remaining) consume = conn.resp_body_remaining;
        conn.resp_body_remaining -= consume;
    }

    // For Chunked mode: feed initial body fragment through parser to detect early end.
    bool chunked_done = false;
    u32 chunked_consumed = initial_body_len;  // default: all bytes
    if (conn.resp_body_mode == BodyMode::Chunked && initial_body_len > 0) {
        const u8* body_start = conn.upstream_recv_buf.data() + header_len;
        u32 pos = 0;
        while (pos < initial_body_len) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            ChunkStatus cs = conn.resp_chunk_parser.feed(
                body_start + pos, initial_body_len - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (cs == ChunkStatus::Done) {
                chunked_done = true;
                break;
            }
            if (cs == ChunkStatus::Error) {
                // Malformed chunked body in initial buffer — 502.
                if (conn.upstream_fd >= 0) {
                    ::close(conn.upstream_fd);
                    conn.upstream_fd = -1;
                }
                static const char k502[] =
                    "HTTP/1.1 502 Bad Gateway\r\n"
                    "Content-Length: 11\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Bad Gateway";
                conn.send_buf.reset();
                conn.send_buf.write(reinterpret_cast<const u8*>(k502), sizeof(k502) - 1);
                conn.keep_alive = false;
                conn.resp_status = kStatusBadGateway;
                conn.on_complete = &on_response_sent<Loop>;
                conn.state = ConnState::Sending;
                loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
                return;
            }
            if (cs == ChunkStatus::NeedMore) break;
        }
        chunked_consumed = pos;
    }

    // During drain: rewrite the Connection header value to "close" so the
    // client knows not to reuse this connection.
    //
    // Strategy: use parsed header_end to locate headers, then find the
    // Connection header line within headers only (never touch the body).
    // If the header has "keep-alive", replace with "close     "
    // (length-preserving, trailing spaces are valid per HTTP spec). If no
    // Connection header exists, rebuild the response in send_buf with
    // "Connection: close" injected.
    if (loop->is_draining()) {
        u8* d = const_cast<u8*>(conn.upstream_recv_buf.data());
        u32 len = conn.upstream_recv_buf.len();

        // Use parser-provided header end offset.
        // header_end points past \r\n\r\n, so the \r\n\r\n starts at header_end - 4.
        u32 hdr_end =
            (resp_parser.header_end >= kHeaderEndLen) ? resp_parser.header_end - kHeaderEndLen : 0;

        // Search for "\r\nConnection:" using case-insensitive header-name matching.
        bool rewritten = false;
        if (hdr_end > 0) {
            for (u32 j = 0; j + 14 <= hdr_end; j++) {
                if (d[j] == '\r' && d[j + 1] == '\n' && ascii_ci_eq(d + j + 2, "connection", 10) &&
                    d[j + 12] == ':') {
                    // Found Connection header. Find the value and overwrite.
                    u32 val_start = j + 13;
                    // Skip optional whitespace after colon.
                    while (val_start < hdr_end && d[val_start] == ' ') val_start++;
                    // Find end of header value (\r\n).
                    u32 val_end = val_start;
                    while (val_end + 1 < hdr_end && !(d[val_end] == '\r' && d[val_end + 1] == '\n'))
                        val_end++;
                    u32 val_len = val_end - val_start;
                    // Overwrite: "close" + pad with spaces to preserve length.
                    if (val_len >= 5) {
                        d[val_start] = 'c';
                        d[val_start + 1] = 'l';
                        d[val_start + 2] = 'o';
                        d[val_start + 3] = 's';
                        d[val_start + 4] = 'e';
                        for (u32 k = val_start + 5; k < val_end; k++) d[k] = ' ';
                        rewritten = true;
                    }
                    break;
                }
            }
        }

        // No Connection header found: rebuild response in send_buf with one injected.
        // This handles HTTP/1.1 responses that omit Connection (default keep-alive).
        if (!rewritten && hdr_end > 0) {
            u32 body_start = hdr_end + kHeaderEndLen;  // skip \r\n\r\n
            u32 raw_body_len = (len > body_start) ? len - body_start : 0;
            // Cap body to parsed boundary (HEAD/204/CL/chunked).
            u32 body_len = raw_body_len;
            if (conn.resp_body_mode == BodyMode::None)
                body_len = 0;
            else if (conn.resp_body_mode == BodyMode::ContentLength &&
                     body_len > resp.content_length)
                body_len = resp.content_length;
            static const char kConnClose[] = "Connection: close\r\n";
            // Only inject if it fits in send_buf.
            if (hdr_end + kConnCloseLen + kHeaderEndLen + body_len <= conn.send_buf.capacity()) {
                conn.send_buf.reset();
                conn.send_buf.write(d, hdr_end + 2);  // headers up to last \r\n
                conn.send_buf.write(reinterpret_cast<const u8*>(kConnClose), kConnCloseLen);
                conn.send_buf.write(d + hdr_end + 2, 2 + body_len);  // \r\n + capped body
                conn.keep_alive = false;
                conn.resp_body_sent = conn.send_buf.len();
                // Check if the body is already complete in this buffer.
                bool drain_body_done = (conn.resp_body_mode == BodyMode::None) ||
                                       (conn.resp_body_mode == BodyMode::ContentLength &&
                                        conn.resp_body_remaining == 0) ||
                                       (conn.resp_body_mode == BodyMode::Chunked && chunked_done);
                if (!drain_body_done) {
                    conn.on_complete = &on_response_header_sent<Loop>;
                } else {
                    conn.on_complete = &on_response_sent<Loop>;
                }
                conn.state = ConnState::Sending;
                loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
                return;
            }
            // Doesn't fit — fall through and send without the header.
            // Client will see EOF on close, which is acceptable.
        }
    }

    // Determine if more body data needs to be streamed after the initial send.
    bool body_complete = false;
    if (conn.resp_body_mode == BodyMode::None) {
        body_complete = true;
    } else if (conn.resp_body_mode == BodyMode::ContentLength) {
        body_complete = (conn.resp_body_remaining == 0);
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        body_complete = chunked_done;
    }
    // UntilClose: never complete until EOF

    // Cap initial send to headers + actual body bytes (don't leak excess).
    u32 actual_body = initial_body_len;
    if (conn.resp_body_mode == BodyMode::None)
        actual_body = 0;
    else if (conn.resp_body_mode == BodyMode::ContentLength &&
             initial_body_len > resp.content_length)
        actual_body = resp.content_length;
    else if (conn.resp_body_mode == BodyMode::Chunked)
        actual_body = chunked_consumed;  // cap at chunk parser boundary
    u32 initial_send_len = header_len + actual_body;

    conn.resp_body_sent = initial_send_len;
    conn.state = ConnState::Sending;

    if (body_complete) {
        conn.on_complete = &on_proxy_response_sent<Loop>;
        loop->submit_send(conn, conn.upstream_recv_buf.data(), initial_send_len);
    } else {
        conn.on_complete = &on_response_header_sent<Loop>;
        loop->submit_send(conn, conn.upstream_recv_buf.data(), initial_send_len);
    }
}

// Step: response sent to client, go back to reading next request (keep-alive).
template <typename Loop>
void on_proxy_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
        // Stale UpstreamSend from body streaming interrupted by early response.
        if (ev.type == IoEventType::UpstreamSend) return;
        // Stale UpstreamRecv: upstream recv armed from body streaming.
        if (ev.type == IoEventType::UpstreamRecv) return;
        // Client Recv: drain and re-arm.
        if (ev.type == IoEventType::Recv) {
            if (ev.result > 0) {
                conn.recv_buf.reset();
                loop->submit_recv(conn);
                return;
            }
            loop->close_conn(conn);
            return;
        }
        loop->close_conn(conn);
        return;
    }

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    // Backends guarantee full send of submitted length. The submitted
    // length may be less than recv_buf.len() (trimmed for HEAD/204/CL cap).

    // Record metrics + access log only after send is confirmed successful.
    on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
    loop->epoch_leave();

    // During drain: close proxy connections instead of re-arming for next request.
    if (loop->is_draining()) {
        loop->close_conn(conn);
        return;
    }

    // Reset upstream buffer — keep slice for potential next proxy request.
    conn.upstream_recv_buf.reset();

    // Close upstream fd and clear armed flags before re-arming for next request.
    // Without this: upstream_recv_armed stays true → next proxy request's
    // submit_recv_upstream is a no-op → permanent hang on io_uring.
    if (conn.upstream_fd >= 0) {
        ::close(conn.upstream_fd);
        conn.upstream_fd = -1;
    }
    loop->clear_upstream_fd(conn.id);
    conn.upstream_recv_armed = false;
    conn.upstream_send_armed = false;

    // Merge stashed + late-arriving pipelined data into recv_buf.
    // pipeline_recover restores stash to recv_buf (resets it first).
    // If recv_buf also has late data, append stash to it instead.
    if (conn.pipeline_stash_len > 0 && conn.recv_buf.len() > 0) {
        // Both stash (early) and recv_buf (late): correct order is stash first.
        u16 slen = conn.pipeline_stash_len;
        u32 late_len = conn.recv_buf.len();
        if (static_cast<u32>(slen) + late_len > conn.recv_buf.capacity()) {
            conn.pipeline_stash_len = 0;
            conn.send_buf.reset();
            loop->close_conn(conn);
            return;
        }
        conn.pipeline_stash_len = 0;
        conn.upstream_recv_buf.reset();
        conn.upstream_recv_buf.write(conn.recv_buf.data(), late_len);
        conn.recv_buf.reset();
        conn.recv_buf.write(conn.send_buf.data(), slen);
        conn.recv_buf.write(conn.upstream_recv_buf.data(), late_len);
        conn.upstream_recv_buf.reset();
        conn.send_buf.reset();
        conn.pipeline_depth++;
        pipeline_dispatch<Loop>(loop, conn);
        return;
    }
    if (pipeline_recover(conn)) {
        pipeline_dispatch<Loop>(loop, conn);
        return;
    }
    if (conn.recv_buf.len() > 0) {
        conn.pipeline_depth++;
        pipeline_dispatch<Loop>(loop, conn);
        return;
    }
    conn.pipeline_depth = 0;
    conn.recv_buf.reset();
    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

}  // namespace rut
