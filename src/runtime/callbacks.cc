#include "rut/runtime/callbacks_impl.h"
#include "rut/runtime/epoll_event_loop.h"
#include "rut/runtime/iouring_event_loop.h"

namespace rut {

u8 map_log_method(HttpMethod method) {
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

u8 parse_log_method_fallback(const u8* data, u32 len, u32* method_len) {
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

void capture_request_metadata(Connection& conn) {
    conn.req_method = static_cast<u8>(LogHttpMethod::Other);
    conn.req_size = conn.recv_buf.len();
    conn.req_path[0] = '/';
    conn.req_path[1] = '\0';
    conn.upstream_us = 0;
    conn.upstream_name[0] = '\0';
    conn.capture_header_len = 0;
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
        u32 chunk_consumed = 0;
        if (req.chunked) {
            conn.req_body_mode = BodyMode::Chunked;
            conn.req_body_remaining = 0;
            conn.req_chunk_parser.reset();
            u32 body_in_buf = len > parser.header_end ? len - parser.header_end : 0;
            chunk_consumed = body_in_buf;
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
            if (chunk_consumed == 0 && conn.req_body_mode == BodyMode::None) {
                conn.req_initial_send_len = 0;
            } else {
                conn.req_initial_send_len = parser.header_end + chunk_consumed;
            }
        } else if (req.has_content_length && req.content_length > 0) {
            conn.req_body_mode = BodyMode::ContentLength;
            conn.req_content_length = req.content_length;
            conn.req_body_remaining = req.content_length;
            u32 body_in_buf = len > parser.header_end ? len - parser.header_end : 0;
            if (body_in_buf >= conn.req_body_remaining)
                conn.req_body_remaining = 0;
            else
                conn.req_body_remaining -= body_in_buf;
        }
        if (conn.req_body_mode == BodyMode::None) {
            conn.req_initial_send_len = parser.header_end;
        } else if (conn.req_body_mode == BodyMode::ContentLength) {
            u32 body_in_initial = conn.req_content_length - conn.req_body_remaining;
            conn.req_initial_send_len = parser.header_end + body_in_initial;
        }
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

u32 pipeline_leftover(const Connection& conn) {
    u32 req_end = conn.req_initial_send_len;
    u32 buf_len = conn.recv_buf.len();
    if (req_end == 0 || req_end >= buf_len) return 0;
    return buf_len - req_end;
}

bool pipeline_shift(Connection& conn) {
    u32 leftover = pipeline_leftover(conn);
    if (leftover == 0) return false;
    const u8* src = conn.recv_buf.data() + conn.req_initial_send_len;
    conn.recv_buf.reset();
    u8* dst = conn.recv_buf.write_ptr();
    __builtin_memmove(dst, src, leftover);
    conn.recv_buf.commit(leftover);
    conn.pipeline_depth++;
    return true;
}

void pipeline_stash(Connection& conn) {
    u32 leftover = pipeline_leftover(conn);
    if (leftover == 0) {
        conn.pipeline_stash_len = 0;
        return;
    }
    conn.send_buf.reset();
    if (leftover > conn.send_buf.write_avail()) {
        conn.pipeline_stash_len = 0;
        return;
    }
    const u8* src = conn.recv_buf.data() + conn.req_initial_send_len;
    conn.send_buf.write(src, leftover);
    conn.pipeline_stash_len = static_cast<u16>(leftover);
}

bool pipeline_recover(Connection& conn) {
    u16 stash_len = conn.pipeline_stash_len;
    conn.pipeline_stash_len = 0;
    if (stash_len == 0) return false;
    const u8* src = conn.send_buf.data();
    conn.recv_buf.reset();
    u8* dst = conn.recv_buf.write_ptr();
    __builtin_memmove(dst, src, stash_len);
    conn.recv_buf.commit(stash_len);
    conn.send_buf.reset();
    conn.pipeline_depth++;
    return true;
}

const char kResponse200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

const char kResponse200Close[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

void capture_stage_headers(Connection& conn) {
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

const char* status_reason(u16 code) {
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

void format_static_response(Connection& conn, u16 code, bool keep_alive) {
    const char* reason = status_reason(code);
    u32 reason_len = 0;
    while (reason[reason_len]) reason_len++;
    bool no_body = (code < 200 || code == 204 || code == 304);
    u32 body_len = no_body ? 0 : reason_len;
    conn.send_buf.reset();
    conn.send_buf.write(reinterpret_cast<const u8*>("HTTP/1.1 "), 9);
    char code_buf[3];
    code_buf[0] = static_cast<char>('0' + (code / 100) % 10);
    code_buf[1] = static_cast<char>('0' + (code / 10) % 10);
    code_buf[2] = static_cast<char>('0' + code % 10);
    conn.send_buf.write(reinterpret_cast<const u8*>(code_buf), 3);
    conn.send_buf.write(reinterpret_cast<const u8*>(" "), 1);
    conn.send_buf.write(reinterpret_cast<const u8*>(reason), reason_len);
    conn.send_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);
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
    if (keep_alive)
        conn.send_buf.write(reinterpret_cast<const u8*>("Connection: keep-alive\r\n"), 24);
    else
        conn.send_buf.write(reinterpret_cast<const u8*>("Connection: close\r\n"), 19);
    conn.send_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);
    if (body_len > 0) conn.send_buf.write(reinterpret_cast<const u8*>(reason), body_len);
}

void prepare_early_response_state(Connection& conn) {
    bool has_remaining_body =
        (conn.req_body_mode == BodyMode::ContentLength && conn.req_body_remaining > 0) ||
        (conn.req_body_mode == BodyMode::Chunked &&
         conn.req_chunk_parser.state != ChunkedParser::State::Complete);
    if (has_remaining_body) {
        conn.recv_buf.reset();
        conn.keep_alive = false;
    } else {
        pipeline_stash(conn);
        conn.recv_buf.reset();
    }
    if (conn.upstream_start_us == 0) conn.upstream_start_us = monotonic_us();
}

u32 consume_upstream_sent(Connection& conn) {
    u32 sent = conn.upstream_send_len;
    u32 total = conn.upstream_recv_buf.len();
    conn.upstream_send_len = 0;
    if (sent >= total) {
        conn.upstream_recv_buf.reset();
        return 0;
    }
    u32 remaining = total - sent;
    const u8* src = conn.upstream_recv_buf.data() + sent;
    conn.upstream_recv_buf.reset();
    u8* dst = conn.upstream_recv_buf.write_ptr();
    __builtin_memmove(dst, src, remaining);
    conn.upstream_recv_buf.commit(remaining);
    return remaining;
}

#define INSTANTIATE_CALLBACKS(Loop)                                                         \
    template void on_request_complete<Loop>(Loop*, Connection&, u16, u32);                  \
    template void pipeline_dispatch<Loop>(Loop*, Connection&);                              \
    template void on_header_received<Loop>(void*, Connection&, IoEvent);                    \
    template void on_response_sent<Loop>(void*, Connection&, IoEvent);                      \
    template void on_upstream_connected<Loop>(void*, Connection&, IoEvent);                 \
    template void on_upstream_request_sent<Loop>(void*, Connection&, IoEvent);              \
    template void on_upstream_response<Loop>(void*, Connection&, IoEvent);                  \
    template void on_proxy_response_sent<Loop>(void*, Connection&, IoEvent);                \
    template void on_response_header_sent<Loop>(void*, Connection&, IoEvent);               \
    template void on_response_body_recvd<Loop>(void*, Connection&, IoEvent);                \
    template void on_response_body_sent<Loop>(void*, Connection&, IoEvent);                 \
    template void handle_early_upstream_recv<Loop>(Loop*, Connection&, IoEvent, bool);      \
    template void on_body_send_with_early_response<Loop>(void*, Connection&, IoEvent);      \
    template void on_request_body_sent<Loop>(void*, Connection&, IoEvent);                  \
    template void on_early_upstream_recvd<Loop>(void*, Connection&, IoEvent);               \
    template void on_early_upstream_recvd_send_inflight<Loop>(void*, Connection&, IoEvent); \
    template void on_request_body_recvd<Loop>(void*, Connection&, IoEvent)

INSTANTIATE_CALLBACKS(EpollEventLoop);
INSTANTIATE_CALLBACKS(IoUringEventLoop);

#undef INSTANTIATE_CALLBACKS

}  // namespace rut
