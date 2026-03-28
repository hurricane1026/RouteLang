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

    const u8* data = conn.recv_buf.data();
    u32 len = conn.recv_buf.len();
    if (!data || len == 0) return;

    HttpParser parser;
    ParsedRequest req;
    parser.reset();
    if (parser.parse(data, len, &req) == ParseStatus::Complete) {
        conn.req_method = map_log_method(req.method);
        u32 copy_len = req.path.len;
        if (copy_len >= sizeof(conn.req_path)) copy_len = sizeof(conn.req_path) - 1;
        for (u32 i = 0; i < copy_len; i++) conn.req_path[i] = req.path.ptr[i];
        conn.req_path[copy_len] = '\0';
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

    // Guard: unexpected event type (e.g., multishot recv CQE arriving while
    // in Send state) indicates a protocol/IO error — close to prevent silent
    // recv_buf accumulation or masked errors.
    if (ev.type != IoEventType::Recv) {
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

    conn.req_start_us = monotonic_us();
    capture_request_metadata(conn);
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

    // Request cycle complete — reset recv_buf before reading next request.
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

    conn.state = ConnState::Proxying;
    // Forward the original request to upstream (upstream_fd, not fd)
    conn.on_complete = &on_upstream_request_sent<Loop>;
    loop->submit_send_upstream(conn, conn.recv_buf.data(), conn.recv_buf.len());
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

    // Validate full send — partial would drop part of the client request.
    if (static_cast<u32>(ev.result) != conn.recv_buf.len()) {
        loop->close_conn(conn);
        return;
    }

    // Original request forwarded — reset recv_buf for upstream response data.
    conn.upstream_start_us = monotonic_us();
    conn.recv_buf.reset();
    conn.on_complete = &on_upstream_response<Loop>;
    loop->submit_recv_upstream(conn);
}

// Step: upstream response received, forward to client.
template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    // Accept both UpstreamRecv (io_uring multishot / epoll add_recv_upstream)
    // and Recv (epoll fast-upstream: immediate connect or send completion
    // re-registers the upstream fd as Recv before submit_recv_upstream runs).
    if (ev.type != IoEventType::UpstreamRecv && ev.type != IoEventType::Recv) {
        loop->close_conn(conn);
        return;
    }

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    if (conn.upstream_start_us != 0) {
        conn.upstream_us = static_cast<u32>(monotonic_us() - conn.upstream_start_us);
        conn.upstream_start_us = 0;
    }

    // Extract status from upstream response (first line: "HTTP/1.1 NNN ...").
    // TODO: replace with proper HTTP parser when integrated.
    conn.resp_status = kStatusOK;  // default
    if (conn.recv_buf.len() >= kMinResponseLen) {
        const u8* d = conn.recv_buf.data();
        // "HTTP/1.x NNN" — status starts at offset 9
        if (d[0] == 'H' && d[4] == '/' && d[8] == ' ') {
            conn.resp_status =
                static_cast<u16>((d[9] - '0') * 100 + (d[10] - '0') * 10 + (d[11] - '0'));
        }
    }

    // During drain: rewrite the Connection header value to "close" so the
    // client knows not to reuse this connection.
    //
    // Strategy: find header boundary (\r\n\r\n), then locate the Connection
    // header line within headers only (never touch the body). If the header
    // has "keep-alive", replace with "close     " (length-preserving, trailing
    // spaces are valid per HTTP spec). If no Connection header exists, rebuild
    // the response in send_buf with "Connection: close" injected.
    if (loop->is_draining()) {
        u8* d = const_cast<u8*>(conn.recv_buf.data());
        u32 len = conn.recv_buf.len();

        // Find end of headers.
        u32 hdr_end = 0;
        for (u32 j = 0; j + 3 < len; j++) {
            if (d[j] == '\r' && d[j + 1] == '\n' && d[j + 2] == '\r' && d[j + 3] == '\n') {
                hdr_end = j;
                break;
            }
        }

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
            u32 body_len = (len > body_start) ? len - body_start : 0;
            static const char kConnClose[] = "Connection: close\r\n";
            // Only inject if it fits in send_buf.
            if (hdr_end + kConnCloseLen + kHeaderEndLen + body_len <= conn.send_buf.capacity()) {
                conn.send_buf.reset();
                conn.send_buf.write(d, hdr_end + 2);  // headers up to last \r\n
                conn.send_buf.write(reinterpret_cast<const u8*>(kConnClose), kConnCloseLen);
                conn.send_buf.write(d + hdr_end + 2, len - hdr_end - 2);  // \r\n + body
                // Route through on_response_sent which validates against send_buf.len().
                conn.keep_alive = false;
                conn.on_complete = &on_response_sent<Loop>;
                conn.state = ConnState::Sending;
                loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
                return;
            }
            // Doesn't fit — fall through and send without the header.
            // Client will see EOF on close, which is acceptable.
        }
    }

    // Forward upstream response to downstream client
    conn.on_complete = &on_proxy_response_sent<Loop>;
    conn.state = ConnState::Sending;
    loop->submit_send(conn, conn.recv_buf.data(), conn.recv_buf.len());
}

// Step: response sent to client, go back to reading next request (keep-alive).
template <typename Loop>
void on_proxy_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
        loop->close_conn(conn);
        return;
    }

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    // Validate full send — partial would truncate the proxied response.
    if (static_cast<u32>(ev.result) != conn.recv_buf.len()) {
        loop->close_conn(conn);
        return;
    }

    // Record metrics + access log only after send is confirmed successful.
    on_request_complete(loop, conn, conn.resp_status, conn.recv_buf.len());
    loop->epoch_leave();

    // During drain: close proxy connections instead of re-arming for next request.
    if (loop->is_draining()) {
        loop->close_conn(conn);
        return;
    }

    // Upstream response forwarded — reset recv_buf for next request.
    conn.recv_buf.reset();

    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

}  // namespace rut
