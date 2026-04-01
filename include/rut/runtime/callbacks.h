#pragma once

#include "rut/runtime/access_log.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/http_parser.h"
#include "rut/runtime/io_event.h"
#include "rut/runtime/metrics.h"
#include "rut/runtime/route_table.h"
#include "rut/runtime/traffic_capture.h"
#include "rut/runtime/upstream_pool.h"

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
    // Reset capture staging (prevents stale header_len from a previous
    // keep-alive request bleeding into the next if capture is toggled).
    conn.capture_header_len = 0;
    // Reset request body state (prevents stale Chunked mode from
    // previous keep-alive request bleeding into the next).
    conn.req_body_mode = BodyMode::None;
    conn.req_body_remaining = 0;
    conn.req_chunk_parser.reset();
    conn.req_malformed = false;

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

// Stage raw request headers for traffic capture. Called after
// capture_request_metadata() when capture is enabled. Copies
// recv_buf[0..header_end] into conn.capture_buf so it survives
// body streaming (which overwrites recv_buf).
//
// capture_buf must point to pre-allocated memory of at least
// CaptureEntry::kMaxHeaderLen bytes (typically from Arena or
// inline test storage).
static inline void capture_stage_headers(Connection& conn) {
    conn.capture_header_len = 0;
    if (!conn.capture_buf) return;

    const u8* data = conn.recv_buf.data();
    if (!data) return;

    u32 len = conn.req_header_end;
    if (len == 0) len = conn.recv_buf.len();
    if (len == 0) return;

    u32 copy_len = len;
    if (copy_len > CaptureEntry::kMaxHeaderLen) copy_len = CaptureEntry::kMaxHeaderLen;

    __builtin_memcpy(conn.capture_buf, data, copy_len);
    conn.capture_header_len = static_cast<u16>(copy_len);
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

// Map common status codes to reason phrases.
static inline const char* status_reason(u16 code) {
    switch (code) {
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown";
    }
}

// Format a static HTTP response into send_buf.
// For bodyless status codes (1xx, 204, 304) sends Content-Length: 0 and no body.
// For all others: body = reason phrase, Content-Length = len(reason).
static inline void format_static_response(Connection& conn, u16 code, bool keep_alive) {
    const char* reason = status_reason(code);
    u32 reason_len = 0;
    while (reason[reason_len]) reason_len++;

    // HTTP semantics: 1xx, 204, and 304 MUST NOT include a body.
    bool no_body = (code < 200 || code == 204 || code == 304);
    u32 body_len = no_body ? 0 : reason_len;

    conn.send_buf.reset();
    conn.send_buf.write(reinterpret_cast<const u8*>("HTTP/1.1 "), 9);

    // Status code as 3 digits
    char code_buf[3];
    code_buf[0] = static_cast<char>('0' + (code / 100) % 10);
    code_buf[1] = static_cast<char>('0' + (code / 10) % 10);
    code_buf[2] = static_cast<char>('0' + code % 10);
    conn.send_buf.write(reinterpret_cast<const u8*>(code_buf), 3);
    conn.send_buf.write(reinterpret_cast<const u8*>(" "), 1);
    conn.send_buf.write(reinterpret_cast<const u8*>(reason), reason_len);
    conn.send_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);

    // Content-Length
    conn.send_buf.write(reinterpret_cast<const u8*>("Content-Length: "), 16);
    if (body_len >= 100) {
        char d = static_cast<char>('0' + body_len / 100);
        conn.send_buf.write(reinterpret_cast<const u8*>(&d), 1);
    }
    if (body_len >= 10) {
        char d = static_cast<char>('0' + (body_len / 10) % 10);
        conn.send_buf.write(reinterpret_cast<const u8*>(&d), 1);
    }
    char d = static_cast<char>('0' + body_len % 10);
    conn.send_buf.write(reinterpret_cast<const u8*>(&d), 1);
    conn.send_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);

    // Connection header
    if (keep_alive)
        conn.send_buf.write(reinterpret_cast<const u8*>("Connection: keep-alive\r\n"), 24);
    else
        conn.send_buf.write(reinterpret_cast<const u8*>("Connection: close\r\n"), 19);

    conn.send_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);
    if (body_len > 0)
        conn.send_buf.write(reinterpret_cast<const u8*>(reason), body_len);
}

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

    // Write traffic capture entry (if enabled).
    if (loop->capture_ring && conn.capture_buf && conn.capture_header_len > 0) {
        CaptureEntry cap;
        // Zero only the 64-byte metadata, not the 8KB raw_headers buffer.
        __builtin_memset(&cap, 0,
                         static_cast<u64>(reinterpret_cast<u8*>(&cap.raw_headers) -
                                          reinterpret_cast<u8*>(&cap)));
        cap.timestamp_us = realtime_us();
        cap.req_content_length = conn.req_content_length;
        cap.resp_content_length = resp_size;
        cap.resp_status = status;
        cap.raw_header_len = conn.capture_header_len;
        cap.method = conn.req_method;
        cap.shard_id = conn.shard_id;
        cap.flags =
            (conn.capture_header_len == CaptureEntry::kMaxHeaderLen) ? kCaptureFlagTruncated : 0;
        constexpr u32 kCopyLen = sizeof(conn.upstream_name) < sizeof(cap.upstream_name)
                                     ? sizeof(conn.upstream_name)
                                     : sizeof(cap.upstream_name);
        for (u32 i = 0; i < kCopyLen; i++) {
            cap.upstream_name[i] = conn.upstream_name[i];
            if (conn.upstream_name[i] == '\0') break;
        }
        __builtin_memcpy(cap.raw_headers, conn.capture_buf, conn.capture_header_len);
        loop->capture_ring->push(cap);
    }
}

template <typename Loop>
void on_header_received(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    // Guard: unexpected event type.
    if (ev.type != IoEventType::Recv) {
        // Stale UpstreamRecv/Send CQEs from a previous proxy request.
        // UpstreamRecv: backend already appended bytes to recv_buf — purge them.
        if (ev.type == IoEventType::UpstreamRecv) {
            conn.recv_buf.reset();
            loop->submit_recv(conn);  // re-arm client recv with clean buffer
            return;
        }
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

    conn.req_start_us = monotonic_us();
    capture_request_metadata(conn);
    // Stage raw headers for traffic capture (before body streaming overwrites recv_buf).
    if (loop->capture_ring) capture_stage_headers(conn);
    loop->epoch_enter();
    if (loop->metrics) loop->metrics->on_request_start();

    bool keep_alive = !loop->is_draining();
    conn.keep_alive = keep_alive;

    // Route matching: config_ptr → active RouteConfig (may be null in tests).
    const RouteConfig* config = loop->config_ptr ? *loop->config_ptr : nullptr;
    const RouteEntry* route = nullptr;
    if (config) {
        route = config->match(
            reinterpret_cast<const u8*>(conn.req_path),
            // strlen of null-terminated req_path
            [&]() -> u32 {
                u32 n = 0;
                while (conn.req_path[n]) n++;
                return n;
            }(),
            conn.recv_buf.data() ? conn.recv_buf.data()[0] : 0);
    }

    if (route && route->action == RouteAction::Proxy) {
        // Proxy route: connect to upstream target.
        conn.state = ConnState::Proxying;
        auto& target = config->upstreams[route->upstream_id];

        // Copy upstream name for access log + capture.
        for (u32 i = 0; i < sizeof(conn.upstream_name) && i < target.name_len; i++)
            conn.upstream_name[i] = target.name[i];
        if (target.name_len < sizeof(conn.upstream_name))
            conn.upstream_name[target.name_len] = '\0';
        else
            conn.upstream_name[sizeof(conn.upstream_name) - 1] = '\0';

        i32 ufd = UpstreamPool::create_socket();
        if (ufd < 0) {
            // Socket creation failed → 502
            conn.resp_status = kStatusBadGateway;
            format_static_response(conn, 502, false);
            conn.keep_alive = false;
            conn.on_complete = &on_response_sent<Loop>;
            loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
            return;
        }
        conn.upstream_fd = ufd;
        conn.upstream_start_us = monotonic_us();
        conn.on_complete = &on_upstream_connected<Loop>;
        loop->submit_connect(conn, &target.addr, sizeof(target.addr));
    } else if (route && route->action == RouteAction::Static) {
        // Static route: respond with configured status code.
        conn.state = ConnState::Sending;
        conn.resp_status = route->status_code;
        format_static_response(conn, route->status_code, keep_alive);
        conn.on_complete = &on_response_sent<Loop>;
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
    } else {
        // No route match or no config: default 200 OK.
        conn.state = ConnState::Sending;
        conn.resp_status = kStatusOK;
        if (keep_alive) {
            conn.send_buf.reset();
            conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200),
                                sizeof(kResponse200) - 1);
        } else {
            conn.send_buf.reset();
            conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200Close),
                                sizeof(kResponse200Close) - 1);
        }
        conn.on_complete = &on_response_sent<Loop>;
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
    }
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

    // Reject malformed requests (e.g., invalid chunked body).
    if (conn.req_malformed) {
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
        // TODO(task5): also arm upstream recv to handle early responses
        // (401/413) before body is fully sent. Currently, the proxy
        // blocks on body streaming and can miss early error responses.
        conn.recv_buf.reset();
        conn.on_complete = &on_request_body_recvd<Loop>;
        loop->submit_recv(conn);
        return;
    }

    // Original request forwarded — reset recv_buf for upstream response data.
    // TODO(task4): if recv_buf had bytes past req_initial_send_len (pipelined
    // request), they are discarded here. HTTP pipelining support should
    // preserve them for the next keep-alive cycle.
    conn.upstream_start_us = monotonic_us();
    conn.recv_buf.reset();
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
    conn.recv_buf.reset();
    conn.on_complete = &on_response_body_recvd<Loop>;
    loop->submit_recv_upstream(conn);
}

// Called when more response body data arrives from upstream.
template <typename Loop>
void on_response_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::UpstreamRecv) {
        // Client Recv during response streaming: backend already appended
        // client bytes to recv_buf at offset 0. Must discard and re-arm
        // upstream recv — otherwise the next UpstreamRecv appends after
        // the client bytes and submit_send forwards client data as response.
        if (ev.type == IoEventType::Recv) {
            conn.recv_buf.reset();
            loop->submit_recv_upstream(conn);
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

    u32 data_len = conn.recv_buf.len();
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
        const u8* body_data = conn.recv_buf.data();
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

    // Forward body data to client (capped to body boundary).
    conn.on_complete = &on_response_body_sent<Loop>;
    conn.state = ConnState::Sending;
    loop->submit_send(conn, conn.recv_buf.data(), send_len);
}

// Called when a response body chunk has been sent to client.
template <typename Loop>
void on_response_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send) {
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
        // Body streaming complete.
        on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
        loop->epoch_leave();

        if (conn.upstream_fd >= 0) {
            ::close(conn.upstream_fd);
            conn.upstream_fd = -1;
        }
        conn.upstream_recv_armed = false;
        conn.upstream_send_armed = false;

        if (!conn.keep_alive || loop->is_draining()) {
            loop->close_conn(conn);
            return;
        }

        conn.recv_buf.reset();
        conn.state = ConnState::ReadingHeader;
        conn.on_complete = &on_header_received<Loop>;
        loop->submit_recv(conn);
        return;
    }

    // More body to stream — recv next chunk from upstream.
    conn.recv_buf.reset();
    conn.on_complete = &on_response_body_recvd<Loop>;
    loop->submit_recv_upstream(conn);
}

// --- Request body streaming callbacks ---
// Multi-pass cycle: recv from client → send to upstream → recv more → until done.

// Called when request body chunk has been sent to upstream.
template <typename Loop>
void on_request_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Send && ev.type != IoEventType::UpstreamSend) {
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }
    // Don't validate against recv_buf.len() — send may have been capped
    // to body boundary (less than full buffer). Backends guarantee full
    // send of the submitted length.

    // Check if request body is complete.
    bool body_done = false;
    if (conn.req_body_mode == BodyMode::ContentLength) {
        body_done = (conn.req_body_remaining == 0);
    } else if (conn.req_body_mode == BodyMode::Chunked) {
        body_done = (conn.req_chunk_parser.state == ChunkedParser::State::Complete);
    }

    if (body_done) {
        // Request body fully forwarded — now wait for upstream response.
        conn.upstream_start_us = monotonic_us();
        conn.recv_buf.reset();
        conn.on_complete = &on_upstream_response<Loop>;
        loop->submit_recv_upstream(conn);
        return;
    }

    // More body to stream — recv next chunk from client.
    conn.recv_buf.reset();
    conn.on_complete = &on_request_body_recvd<Loop>;
    loop->submit_recv(conn);
}

// Called when more request body data arrives from client.
template <typename Loop>
void on_request_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Recv) {
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    u32 data_len = conn.recv_buf.len();
    conn.req_size += data_len;  // accumulate for access log

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

    conn.on_complete = &on_request_body_sent<Loop>;
    loop->submit_send_upstream(conn, conn.recv_buf.data(), send_len);
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

    if (conn.upstream_start_us != 0) {
        conn.upstream_us = static_cast<u32>(monotonic_us() - conn.upstream_start_us);
        conn.upstream_start_us = 0;
    }

    // EOF or error with no data → close.
    // EOF with partial data → parse will return Incomplete → 502 below.
    if (ev.result <= 0 && conn.recv_buf.len() == 0) {
        loop->close_conn(conn);
        return;
    }

    // Parse upstream response with proper HTTP parser.
    HttpResponseParser resp_parser;
    ParsedResponse resp;
    resp.reset();
    resp_parser.reset();
    ParseStatus ps = resp_parser.parse(conn.recv_buf.data(), conn.recv_buf.len(), &resp);
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
        u32 total = conn.recv_buf.len();
        if (interim_end < total) {
            // Remaining bytes after the 1xx may contain the final response.
            // Shift remaining bytes to buffer start (same backing slice).
            u32 remaining = total - interim_end;
            const u8* src = conn.recv_buf.data() + interim_end;
            conn.recv_buf.reset();
            // After reset, write_ptr() points to start of slice.
            u8* dst = conn.recv_buf.write_ptr();
            __builtin_memmove(dst, src, remaining);
            conn.recv_buf.commit(remaining);
            // Re-enter to parse the remaining data.
            on_upstream_response<Loop>(lp, conn, ev);
            return;
        }
        // No remaining data — wait for the final response.
        conn.recv_buf.reset();
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
    } else if (!resp.keep_alive) {
        // EOF-delimited body: valid for HTTP/1.0 (default close) or
        // explicit Connection: close. keep_alive is false in both cases.
        conn.resp_body_mode = BodyMode::UntilClose;
        conn.resp_body_remaining = 0;
    } else {
        // No CL/TE: RFC 7230 says read until EOF (close-delimited).
        // Even for HTTP/1.1, non-conformant origins may send bodies
        // without framing. UntilClose prevents dropping such bodies.
        conn.resp_body_mode = BodyMode::UntilClose;
        conn.resp_body_remaining = 0;
    }

    // Calculate how much body data is already in recv_buf from the initial recv.
    u32 header_len = resp_parser.header_end;
    u32 total_len = conn.recv_buf.len();
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
        const u8* body_start = conn.recv_buf.data() + header_len;
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
        u8* d = const_cast<u8*>(conn.recv_buf.data());
        u32 len = conn.recv_buf.len();

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
        loop->submit_send(conn, conn.recv_buf.data(), initial_send_len);
    } else {
        conn.on_complete = &on_response_header_sent<Loop>;
        loop->submit_send(conn, conn.recv_buf.data(), initial_send_len);
    }
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

    // Close upstream fd and clear armed flags before re-arming for next request.
    // Without this: upstream_recv_armed stays true → next proxy request's
    // submit_recv_upstream is a no-op → permanent hang on io_uring.
    if (conn.upstream_fd >= 0) {
        ::close(conn.upstream_fd);
        conn.upstream_fd = -1;
    }
    conn.upstream_recv_armed = false;
    conn.upstream_send_armed = false;

    conn.recv_buf.reset();
    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

}  // namespace rut
