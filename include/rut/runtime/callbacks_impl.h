#pragma once

#include "rut/common/types.h"
#include "rut/runtime/access_log.h"
#include "rut/runtime/callbacks.h"
#include "rut/runtime/chunked_parser.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/connection_base.h"
#include "rut/runtime/http_parser.h"
#include "rut/runtime/io_event.h"
#include "rut/runtime/jit_dispatch.h"
#include "rut/runtime/route_table.h"
#include "rut/runtime/traffic_capture.h"
#include "rut/runtime/upstream_pool.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rut {

u8 map_log_method(HttpMethod method);
u8 parse_log_method_fallback(const u8* data, u32 len, u32* method_len);
void capture_request_metadata(Connection& conn);
u32 pipeline_leftover(const Connection& conn);
bool pipeline_shift(Connection& conn);
void pipeline_stash(Connection& conn);
bool pipeline_recover(Connection& conn);
void capture_stage_headers(Connection& conn);
const char* status_reason(u16 code);
void format_static_response(Connection& conn, u16 code, bool keep_alive);
void prepare_early_response_state(Connection& conn);
u32 consume_upstream_sent(Connection& conn);

extern const char kResponse200[];
extern const char kResponse200Close[];

template <typename Loop>
void on_request_complete(Loop* loop, Connection& conn, u16 status, u32 resp_size);

// ── JIT handler dispatch ───────────────────────────────────────────
// Route-matched JitHandler action → invoke the compiled handler and
// translate JitDispatchOutcome into event-loop operations (send, forward,
// register timer for resume, or 500). Shared between the initial call
// (on_header_received) and timer-driven resumes.
template <typename Loop>
void handle_jit_outcome(Loop* loop,
                        Connection& conn,
                        const JitDispatchOutcome& outcome,
                        jit::HandlerFn fn,
                        bool keep_alive);

// Called from timer.tick when the timer firing was a JIT handler yield
// (conn.pending_handler_fn != nullptr). Re-enters the handler with
// ctx.state = conn.handler_state, then re-dispatches on the outcome.
template <typename Loop>
void resume_jit_handler(Loop* loop, Connection& conn);

template <typename Loop>
void pipeline_dispatch(Loop* loop, Connection& conn);

template <typename Loop>
void handle_early_upstream_recv(Loop* loop, Connection& conn, IoEvent ev, bool send_in_flight);

template <typename Loop>
void on_request_complete(Loop* loop, Connection& conn, u16 status, u32 resp_size) {
    const u32 kDurationUs = static_cast<u32>(monotonic_us() - conn.req_start_us);

    // Clear req_start_us so close_conn_impl knows no request is in flight.
    conn.req_start_us = 0;

    if (loop->metrics) {
        loop->metrics->on_request_complete(kDurationUs);
    }

    if (loop->access_log) {
        AccessLogEntry entry{};
        entry.timestamp_us = realtime_us();
        entry.duration_us = kDurationUs;
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

    // TODO: avoid 8KB stack alloc + double copy by adding a reserve/commit
    // API to CaptureRing (write directly into ring slot). Acceptable for now
    // since capture is a debug feature, not always-on production path.
    if (loop->capture_ring && conn.capture_buf && conn.capture_header_len > 0) {
        CaptureEntry cap;
        __builtin_memset(&cap, 0, sizeof(cap));
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
void pipeline_dispatch(Loop* loop, Connection& conn) {
    conn.state = ConnState::ReadingHeader;
    // Refresh keepalive timer — synthetic dispatch skips the normal
    // EventLoop::dispatch() which calls timer.refresh().
    loop->timer.refresh(&conn, loop->keepalive_timeout);
    const IoEvent kSynth = {
        conn.id, static_cast<i32>(conn.recv_buf.len()), 0, 0, IoEventType::Recv, 0};
    on_header_received<Loop>(static_cast<void*>(loop), conn, kSynth);
}

template <typename Loop>
void on_header_received(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);
    // Slot: on_recv. dispatch_event guarantees ev.type == Recv.

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
            conn.set_slots(&on_header_received<Loop>, nullptr, nullptr, nullptr);
            loop->submit_recv(conn);
            return;
        }
    }

    capture_request_metadata(conn);

    conn.req_start_us = monotonic_us();
    if (loop->capture_ring) capture_stage_headers(conn);
    loop->epoch_enter();
    if (loop->metrics) loop->metrics->on_request_start();

    const bool kKeepAlive = !loop->is_draining();
    conn.keep_alive = kKeepAlive;

    // Route matching: config_ptr → active RouteConfig (may be null in tests).
    const RouteConfig* config = loop->config_ptr ? *loop->config_ptr : nullptr;
    const RouteEntry* route = nullptr;
    if (config) {
        // Map LogHttpMethod back to RouteConfig's method_char format.
        // RouteConfig::match() uses first-char matching ('G'=GET, etc.).
        // This is still ambiguous for POST/PUT/PATCH (all 'P'), which
        // is a known limitation of RouteConfig — JIT routing will use
        // the full HttpMethod enum.
        static constexpr u8 kMethodChars[] = {'G', 'P', 'P', 'D', 'P', 'H', 'O', 'C', 'T', 0};
        const u8 kMethodChar =
            conn.req_method < sizeof(kMethodChars) ? kMethodChars[conn.req_method] : 0;
        route = config->match(
            reinterpret_cast<const u8*>(conn.req_path),
            [&]() -> u32 {
                u32 n = 0;
                while (conn.req_path[n]) n++;
                return n;
            }(),
            kMethodChar);
    }

    if (route && route->action == RouteAction::Proxy) {
        conn.state = ConnState::Proxying;
        auto& target = config->upstreams[route->upstream_id];
        for (u32 i = 0; i < sizeof(conn.upstream_name) && i < target.name_len; i++)
            conn.upstream_name[i] = target.name[i];
        if (target.name_len < sizeof(conn.upstream_name))
            conn.upstream_name[target.name_len] = '\0';
        else
            conn.upstream_name[sizeof(conn.upstream_name) - 1] = '\0';
        const i32 kUpstreamFd = UpstreamPool::create_socket();
        if (kUpstreamFd < 0) {
            conn.state = ConnState::Sending;
            conn.resp_status = kStatusBadGateway;
            format_static_response(conn, 502, false);
            conn.keep_alive = false;
            conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
            loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
            return;
        }
        conn.upstream_fd = kUpstreamFd;
        conn.upstream_start_us = monotonic_us();
        conn.set_slots(nullptr, nullptr, nullptr, &on_upstream_connected<Loop>);
        loop->submit_connect(conn, &target.addr, sizeof(target.addr));
    } else if (route && route->action == RouteAction::Static) {
        conn.state = ConnState::Sending;
        conn.resp_status = route->status_code;
        format_static_response(conn, route->status_code, kKeepAlive);
        conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
    } else if (route && route->action == RouteAction::JitHandler && route->fn) {
        conn.state = ConnState::ExecHandler;
        conn.handler_state = 0;  // entry state
        jit::HandlerCtx ctx{};
        ctx.state = 0;
        auto outcome = invoke_jit_handler(route->fn,
                                          static_cast<void*>(&conn),
                                          ctx,
                                          conn.recv_buf.data(),
                                          conn.recv_buf.len(),
                                          /*arena=*/nullptr);
        handle_jit_outcome<Loop>(loop, conn, outcome, route->fn, kKeepAlive);
    } else {
        conn.state = ConnState::Sending;
        conn.resp_status = kStatusOK;
        conn.send_buf.reset();
        if (kKeepAlive)
            conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200), kResponse200Len);
        else
            conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200Close),
                                kResponse200CloseLen);
        conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
    }
}

template <typename Loop>
void on_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);
    // Send complete — clear all slots (will set on_recv for keep-alive below).
    conn.set_slots(nullptr, nullptr, nullptr, nullptr);

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

    on_request_complete(loop, conn, conn.resp_status, conn.send_buf.len());
    loop->epoch_leave();

    if (!conn.keep_alive) {
        loop->close_conn(conn);
        return;
    }

    if (pipeline_shift(conn)) {
        pipeline_dispatch<Loop>(loop, conn);
        return;
    }
    conn.pipeline_depth = 0;
    conn.recv_buf.reset();
    conn.state = ConnState::ReadingHeader;
    conn.set_slots(&on_header_received<Loop>, nullptr, nullptr, nullptr);
    loop->submit_recv(conn);
}

template <typename Loop>
void handle_jit_outcome(Loop* loop,
                        Connection& conn,
                        const JitDispatchOutcome& outcome,
                        jit::HandlerFn fn,
                        bool keep_alive) {
    switch (outcome.kind) {
        case JitDispatchOutcome::Kind::ReturnStatus:
            conn.pending_handler_fn = nullptr;
            conn.state = ConnState::Sending;
            conn.resp_status = outcome.status_code;
            format_static_response(conn, outcome.status_code, keep_alive);
            conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
            loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
            return;
        case JitDispatchOutcome::Kind::TimerYield:
            // Stash fn + next_state so the tick callback can tell resume from
            // keepalive expiry and re-enter the handler. Slots stay clear:
            // no recv/send pending while sleeping. One-second resolution is
            // a known limitation of the current TimerWheel — see
            // timer_seconds_from_ms() in jit_dispatch.h.
            conn.pending_handler_fn = fn;
            conn.handler_state = outcome.next_state;
            conn.state = ConnState::ExecHandler;
            conn.set_slots(nullptr, nullptr, nullptr, nullptr);
            loop->timer.add(&conn, outcome.timer_seconds);
            return;
        case JitDispatchOutcome::Kind::Forward:
        case JitDispatchOutcome::Kind::Error:
        default:
            // Forward from JIT path not yet wired end-to-end; fall through
            // to 500 for safety so the client never hangs. Proper Forward
            // integration rides the existing Proxy path once the upstream
            // id resolution is connected — future slice.
            conn.pending_handler_fn = nullptr;
            conn.state = ConnState::Sending;
            conn.resp_status = 500;
            format_static_response(conn, 500, /*keep_alive=*/false);
            conn.keep_alive = false;
            conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
            loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
            return;
    }
}

template <typename Loop>
void resume_jit_handler(Loop* loop, Connection& conn) {
    auto* fn = conn.pending_handler_fn;
    jit::HandlerCtx ctx{};
    ctx.state = conn.handler_state;
    auto outcome = invoke_jit_handler(fn,
                                      static_cast<void*>(&conn),
                                      ctx,
                                      conn.recv_buf.data(),
                                      conn.recv_buf.len(),
                                      /*arena=*/nullptr);
    handle_jit_outcome<Loop>(loop, conn, outcome, fn, conn.keep_alive);
}

template <typename Loop>
void on_upstream_connected(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result < 0) {
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
        conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
        return;
    }

    if (conn.req_malformed) {
        loop->close_conn(conn);
        return;
    }

    if (!loop->alloc_upstream_buf(conn)) {
        loop->close_conn(conn);
        return;
    }

    conn.state = ConnState::Proxying;
    u32 req_send_len =
        conn.req_initial_send_len > 0 ? conn.req_initial_send_len : conn.recv_buf.len();
    if (req_send_len > conn.recv_buf.len()) req_send_len = conn.recv_buf.len();
    conn.set_slots(nullptr,
                   nullptr,
                   &on_early_upstream_recvd_send_inflight<Loop>,
                   &on_upstream_request_sent<Loop>);
    loop->submit_send_upstream(conn, conn.recv_buf.data(), req_send_len);
}

template <typename Loop>
void on_upstream_request_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result < 0) {
        if (conn.upstream_recv_buf.len() > 0) {
            prepare_early_response_state(conn);
            conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
            IoEvent synth = {conn.id,
                             static_cast<i32>(conn.upstream_recv_buf.len()),
                             0,
                             0,
                             IoEventType::UpstreamRecv,
                             0};
            on_upstream_response<Loop>(lp, conn, synth);
            return;
        }
        if (conn.upstream_recv_armed) {
            prepare_early_response_state(conn);
            conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
            loop->submit_recv_upstream(conn);
            return;
        }
        if (conn.upstream_fd >= 0) {
            const u32 kAvail = conn.upstream_recv_buf.write_avail();
            if (kAvail > 0) {
                ssize_t nr;
                do {
                    nr = recv(conn.upstream_fd, conn.upstream_recv_buf.write_ptr(), kAvail, 0);
                } while (nr < 0 && errno == EINTR);
                if (nr > 0) {
                    conn.upstream_recv_buf.commit(static_cast<u32>(nr));
                    prepare_early_response_state(conn);
                    conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
                    IoEvent synth = {
                        conn.id, static_cast<i32>(nr), 0, 0, IoEventType::UpstreamRecv, 0};
                    on_upstream_response<Loop>(lp, conn, synth);
                    return;
                }
            }
        }
        loop->close_conn(conn);
        return;
    }

    const bool kMoreReqBody =
        (conn.req_body_mode == BodyMode::ContentLength && conn.req_body_remaining > 0) ||
        (conn.req_body_mode == BodyMode::Chunked &&
         conn.req_chunk_parser.state != ChunkedParser::State::Complete);
    if (kMoreReqBody) {
        conn.recv_buf.reset();
        conn.set_slots(
            &on_request_body_recvd<Loop>, nullptr, &on_early_upstream_recvd<Loop>, nullptr);
        loop->submit_recv(conn);
        loop->submit_recv_upstream(conn);
        return;
    }

    pipeline_stash(conn);
    conn.recv_buf.reset();
    conn.upstream_start_us = monotonic_us();
    conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
    if (conn.upstream_recv_buf.len() > 0) {
        IoEvent synth = {conn.id,
                         static_cast<i32>(conn.upstream_recv_buf.len()),
                         0,
                         0,
                         IoEventType::UpstreamRecv,
                         0};
        on_upstream_response<Loop>(lp, conn, synth);
    } else {
        loop->submit_recv_upstream(conn);
    }
}

template <typename Loop>
void on_response_header_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    conn.set_slots(nullptr, nullptr, &on_response_body_recvd<Loop>, nullptr);
    const u32 kRemaining = consume_upstream_sent(conn);
    if (kRemaining > 0) {
        IoEvent synth = {conn.id, static_cast<i32>(kRemaining), 0, 0, IoEventType::UpstreamRecv, 0};
        on_response_body_recvd<Loop>(lp, conn, synth);
    } else {
        loop->submit_recv_upstream(conn);
    }
}

template <typename Loop>
void on_response_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        if (conn.resp_body_mode == BodyMode::UntilClose) {
            on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
            loop->epoch_leave();
            loop->close_conn(conn);
            return;
        }
        loop->close_conn(conn);
        return;
    }

    const u32 kDataLen = conn.upstream_recv_buf.len();
    u32 send_len = kDataLen;

    if (conn.resp_body_mode == BodyMode::ContentLength) {
        u32 consume = kDataLen;
        if (consume > conn.resp_body_remaining) consume = conn.resp_body_remaining;
        conn.resp_body_remaining -= consume;
        send_len = consume;
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        const u8* body_data = conn.upstream_recv_buf.data();
        u32 pos = 0;
        while (pos < kDataLen) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            const ChunkStatus kChunkStatus = conn.resp_chunk_parser.feed(
                body_data + pos, kDataLen - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (kChunkStatus == ChunkStatus::Done) break;
            if (kChunkStatus == ChunkStatus::Error) {
                loop->close_conn(conn);
                return;
            }
            if (kChunkStatus == ChunkStatus::NeedMore) break;
        }
        send_len = pos;
    }

    conn.resp_body_sent += send_len;
    conn.upstream_send_len = send_len;
    conn.set_slots(nullptr, &on_response_body_sent<Loop>, nullptr, nullptr);
    conn.state = ConnState::Sending;
    loop->submit_send(conn, conn.upstream_recv_buf.data(), send_len);
}

template <typename Loop>
void on_response_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    conn.set_slots(nullptr, nullptr, nullptr, nullptr);
    const u32 kRemaining = consume_upstream_sent(conn);

    bool body_done = false;
    if (conn.resp_body_mode == BodyMode::ContentLength) {
        body_done = (conn.resp_body_remaining == 0);
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        body_done = (conn.resp_chunk_parser.state == ChunkedParser::State::Complete);
    }

    if (body_done) {
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
            const u16 kStashLen = conn.pipeline_stash_len;
            const u32 kLateLen = conn.recv_buf.len();
            if (static_cast<u32>(kStashLen) + kLateLen > conn.recv_buf.capacity()) {
                conn.pipeline_stash_len = 0;
                conn.send_buf.reset();
                loop->close_conn(conn);
                return;
            }
            conn.pipeline_stash_len = 0;
            conn.upstream_recv_buf.reset();
            conn.upstream_recv_buf.write(conn.recv_buf.data(), kLateLen);
            conn.recv_buf.reset();
            conn.recv_buf.write(conn.send_buf.data(), kStashLen);
            conn.recv_buf.write(conn.upstream_recv_buf.data(), kLateLen);
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
        conn.set_slots(&on_header_received<Loop>, nullptr, nullptr, nullptr);
        loop->submit_recv(conn);
        return;
    }

    conn.set_slots(nullptr, nullptr, &on_response_body_recvd<Loop>, nullptr);
    if (kRemaining > 0) {
        IoEvent synth = {conn.id, static_cast<i32>(kRemaining), 0, 0, IoEventType::UpstreamRecv, 0};
        on_response_body_recvd<Loop>(lp, conn, synth);
    } else {
        loop->submit_recv_upstream(conn);
    }
}

template <typename Loop>
void handle_early_upstream_recv(Loop* loop, Connection& conn, IoEvent ev, bool send_in_flight) {
    if (ev.result <= 0 && conn.upstream_recv_buf.len() == 0) {
        loop->close_conn(conn);
        return;
    }
    if (ev.result <= 0) {
        conn.on_upstream_send = &on_body_send_with_early_response<Loop>;
        conn.on_upstream_recv = nullptr;
        return;
    }
    HttpResponseParser resp_parser;
    ParsedResponse resp;
    resp.reset();
    resp_parser.reset();
    const ParseStatus kParseStatus =
        resp_parser.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &resp);
    const bool kCanRearm = !conn.upstream_recv_armed && !send_in_flight;
    if (kParseStatus == ParseStatus::Incomplete) {
        if (kCanRearm) loop->submit_recv_upstream(conn);
        return;
    }
    if (kParseStatus == ParseStatus::Complete && resp.status_code >= 100 &&
        resp.status_code < 200 && resp.status_code != 101) {
        const u32 kInterimEnd = resp_parser.header_end;
        const u32 kTotal = conn.upstream_recv_buf.len();
        if (kInterimEnd < kTotal) {
            const u32 kRemaining = kTotal - kInterimEnd;
            const u8* src = conn.upstream_recv_buf.data() + kInterimEnd;
            conn.upstream_recv_buf.reset();
            u8* dst = conn.upstream_recv_buf.write_ptr();
            __builtin_memmove(dst, src, kRemaining);
            conn.upstream_recv_buf.commit(kRemaining);
            handle_early_upstream_recv<Loop>(loop, conn, ev, send_in_flight);
            return;
        }
        conn.upstream_recv_buf.reset();
        if (kCanRearm) loop->submit_recv_upstream(conn);
        return;
    }
    conn.on_upstream_send = &on_body_send_with_early_response<Loop>;
    conn.on_upstream_recv = nullptr;
}

template <typename Loop>
void on_body_send_with_early_response(void* lp, Connection& conn, IoEvent ev) {
    (void)ev;

    prepare_early_response_state(conn);
    conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);

    HttpResponseParser probe;
    ParsedResponse probe_resp;
    probe_resp.reset();
    probe.reset();
    const ParseStatus kParseStatus =
        probe.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &probe_resp);
    const i32 kSynthResult = (kParseStatus == ParseStatus::Incomplete)
                                 ? 0
                                 : static_cast<i32>(conn.upstream_recv_buf.len());
    IoEvent synth = {conn.id, kSynthResult, 0, 0, IoEventType::UpstreamRecv, 0};
    on_upstream_response<Loop>(lp, conn, synth);
}

template <typename Loop>
void on_request_body_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        if (conn.upstream_recv_buf.len() > 0) {
            prepare_early_response_state(conn);
            conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
            IoEvent synth = {conn.id,
                             static_cast<i32>(conn.upstream_recv_buf.len()),
                             0,
                             0,
                             IoEventType::UpstreamRecv,
                             0};
            on_upstream_response<Loop>(lp, conn, synth);
            return;
        }
        if (conn.upstream_recv_armed) {
            prepare_early_response_state(conn);
            conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
            loop->submit_recv_upstream(conn);
            return;
        }
        if (conn.upstream_fd >= 0) {
            const u32 kAvail = conn.upstream_recv_buf.write_avail();
            if (kAvail > 0) {
                ssize_t nr;
                do {
                    nr = recv(conn.upstream_fd, conn.upstream_recv_buf.write_ptr(), kAvail, 0);
                } while (nr < 0 && errno == EINTR);
                if (nr > 0) {
                    conn.upstream_recv_buf.commit(static_cast<u32>(nr));
                    prepare_early_response_state(conn);
                    conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
                    IoEvent synth = {
                        conn.id, static_cast<i32>(nr), 0, 0, IoEventType::UpstreamRecv, 0};
                    on_upstream_response<Loop>(lp, conn, synth);
                    return;
                }
            }
        }
        loop->close_conn(conn);
        return;
    }

    bool body_done = false;
    if (conn.req_body_mode == BodyMode::ContentLength) {
        body_done = (conn.req_body_remaining == 0);
    } else if (conn.req_body_mode == BodyMode::Chunked) {
        body_done = (conn.req_chunk_parser.state == ChunkedParser::State::Complete);
    }

    if (body_done) {
        pipeline_stash(conn);
        conn.recv_buf.reset();
        conn.upstream_start_us = monotonic_us();
        if (conn.upstream_recv_buf.len() == 0) conn.upstream_recv_buf.reset();
        conn.set_slots(nullptr, nullptr, &on_upstream_response<Loop>, nullptr);
        if (conn.upstream_recv_buf.len() > 0) {
            IoEvent synth = {conn.id,
                             static_cast<i32>(conn.upstream_recv_buf.len()),
                             0,
                             0,
                             IoEventType::UpstreamRecv,
                             0};
            on_upstream_response<Loop>(lp, conn, synth);
        } else {
            loop->submit_recv_upstream(conn);
        }
        return;
    }

    conn.recv_buf.reset();
    conn.set_slots(&on_request_body_recvd<Loop>, nullptr, &on_early_upstream_recvd<Loop>, nullptr);
    loop->submit_recv(conn);
    loop->submit_recv_upstream(conn);
}

template <typename Loop>
void on_early_upstream_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);
    handle_early_upstream_recv<Loop>(loop, conn, ev, false);
    if (conn.on_upstream_send == &on_body_send_with_early_response<Loop>) {
        on_body_send_with_early_response<Loop>(lp, conn, ev);
    }
}

template <typename Loop>
void on_early_upstream_recvd_send_inflight(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);
    handle_early_upstream_recv<Loop>(loop, conn, ev, true);
}

template <typename Loop>
void on_request_body_recvd(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    const u32 kDataLen = conn.recv_buf.len();
    u32 send_len = kDataLen;
    if (conn.req_body_mode == BodyMode::ContentLength) {
        u32 consume = kDataLen;
        if (consume > conn.req_body_remaining) consume = conn.req_body_remaining;
        conn.req_body_remaining -= consume;
        send_len = consume;
    } else if (conn.req_body_mode == BodyMode::Chunked) {
        const u8* body_data = conn.recv_buf.data();
        u32 pos = 0;
        while (pos < kDataLen) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            const ChunkStatus kChunkStatus = conn.req_chunk_parser.feed(
                body_data + pos, kDataLen - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (kChunkStatus == ChunkStatus::Done) break;
            if (kChunkStatus == ChunkStatus::Error) {
                loop->close_conn(conn);
                return;
            }
            if (kChunkStatus == ChunkStatus::NeedMore) break;
        }
        send_len = pos;
    }

    conn.req_size += send_len;
    conn.req_initial_send_len = send_len;
    conn.set_slots(nullptr,
                   nullptr,
                   &on_early_upstream_recvd_send_inflight<Loop>,
                   &on_request_body_sent<Loop>);
    loop->submit_send_upstream(conn, conn.recv_buf.data(), send_len);
}

template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (conn.upstream_start_us != 0) {
        conn.upstream_us = static_cast<u32>(monotonic_us() - conn.upstream_start_us);
        conn.upstream_start_us = 0;
    }

    if (ev.result <= 0 && conn.upstream_recv_buf.len() == 0) {
        loop->close_conn(conn);
        return;
    }

    HttpResponseParser resp_parser;
    ParsedResponse resp;
    resp.reset();
    resp_parser.reset();
    ParseStatus ps =
        resp_parser.parse(conn.upstream_recv_buf.data(), conn.upstream_recv_buf.len(), &resp);
    if (ps == ParseStatus::Incomplete) {
        if (ev.result <= 0)
            ps = ParseStatus::Error;
        else {
            loop->submit_recv_upstream(conn);
            return;
        }
    }
    if (ps == ParseStatus::Error) {
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
        conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
        conn.state = ConnState::Sending;
        loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
        return;
    }
    conn.resp_status = resp.status_code;

    if (resp.status_code >= 100 && resp.status_code < 200 && resp.status_code != 101) {
        const u32 kInterimEnd = resp_parser.header_end;
        const u32 kTotal = conn.upstream_recv_buf.len();
        if (kInterimEnd < kTotal) {
            const u32 kRemaining = kTotal - kInterimEnd;
            const u8* src = conn.upstream_recv_buf.data() + kInterimEnd;
            conn.upstream_recv_buf.reset();
            u8* dst = conn.upstream_recv_buf.write_ptr();
            __builtin_memmove(dst, src, kRemaining);
            conn.upstream_recv_buf.commit(kRemaining);
            on_upstream_response<Loop>(lp, conn, ev);
            return;
        }
        conn.upstream_recv_buf.reset();
        loop->submit_recv_upstream(conn);
        return;
    }

    const bool kIsHead = (conn.req_method == static_cast<u8>(LogHttpMethod::Head));
    const bool kNoBodyStatus =
        resp.status_code == 204 || resp.status_code == 205 || resp.status_code == 304;

    if (kIsHead || kNoBodyStatus) {
        conn.resp_body_mode = BodyMode::None;
        conn.resp_body_remaining = 0;
    } else if (resp.chunked) {
        conn.resp_body_mode = BodyMode::Chunked;
        conn.resp_chunk_parser.reset();
        conn.resp_body_remaining = 0;
    } else if (resp.has_content_length) {
        conn.resp_body_mode = BodyMode::ContentLength;
        conn.resp_body_remaining = resp.content_length;
    } else {
        conn.resp_body_mode = BodyMode::UntilClose;
        conn.resp_body_remaining = 0;
    }

    const u32 kHeaderLen = resp_parser.header_end;
    const u32 kTotalLen = conn.upstream_recv_buf.len();
    const u32 kInitialBodyLen = (kTotalLen > kHeaderLen) ? kTotalLen - kHeaderLen : 0;

    if (conn.resp_body_mode == BodyMode::ContentLength && kInitialBodyLen > 0) {
        u32 consume = kInitialBodyLen;
        if (consume > conn.resp_body_remaining) consume = conn.resp_body_remaining;
        conn.resp_body_remaining -= consume;
    }

    bool chunked_done = false;
    u32 chunked_consumed = kInitialBodyLen;
    if (conn.resp_body_mode == BodyMode::Chunked && kInitialBodyLen > 0) {
        const u8* body_start = conn.upstream_recv_buf.data() + kHeaderLen;
        u32 pos = 0;
        while (pos < kInitialBodyLen) {
            u32 consumed = 0, out_start = 0, out_len = 0;
            const ChunkStatus kChunkStatus = conn.resp_chunk_parser.feed(
                body_start + pos, kInitialBodyLen - pos, &consumed, &out_start, &out_len);
            pos += consumed;
            if (kChunkStatus == ChunkStatus::Done) {
                chunked_done = true;
                break;
            }
            if (kChunkStatus == ChunkStatus::Error) {
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
                conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
                conn.state = ConnState::Sending;
                loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
                return;
            }
            if (kChunkStatus == ChunkStatus::NeedMore) break;
        }
        chunked_consumed = pos;
    }

    if (loop->is_draining()) {
        u8* d = const_cast<u8*>(conn.upstream_recv_buf.data());
        const u32 kLen = conn.upstream_recv_buf.len();
        const u32 kHdrEnd =
            (resp_parser.header_end >= kHeaderEndLen) ? resp_parser.header_end - kHeaderEndLen : 0;

        bool rewritten = false;
        if (kHdrEnd > 0) {
            for (u32 j = 0; j + 14 <= kHdrEnd; j++) {
                if (d[j] == '\r' && d[j + 1] == '\n' && ascii_ci_eq(d + j + 2, "connection", 10) &&
                    d[j + 12] == ':') {
                    u32 val_start = j + 13;
                    while (val_start < kHdrEnd && d[val_start] == ' ') val_start++;
                    u32 val_end = val_start;
                    while (val_end + 1 < kHdrEnd && (d[val_end] != '\r' || d[val_end + 1] != '\n'))
                        val_end++;
                    const u32 kValLen = val_end - val_start;
                    if (kValLen >= 5) {
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

        if (!rewritten && kHdrEnd > 0) {
            const u32 kBodyStart = kHdrEnd + kHeaderEndLen;
            const u32 kRawBodyLen = (kLen > kBodyStart) ? kLen - kBodyStart : 0;
            u32 body_len = kRawBodyLen;
            if (conn.resp_body_mode == BodyMode::None)
                body_len = 0;
            else if (conn.resp_body_mode == BodyMode::ContentLength &&
                     body_len > resp.content_length)
                body_len = resp.content_length;
            static const char kConnClose[] = "Connection: close\r\n";
            if (kHdrEnd + kConnCloseLen + kHeaderEndLen + body_len <= conn.send_buf.capacity()) {
                conn.send_buf.reset();
                conn.send_buf.write(d, kHdrEnd + 2);
                conn.send_buf.write(reinterpret_cast<const u8*>(kConnClose), kConnCloseLen);
                conn.send_buf.write(d + kHdrEnd + 2, 2 + body_len);
                conn.keep_alive = false;
                conn.resp_body_sent = conn.send_buf.len();
                const bool kDrainBodyDone =
                    (conn.resp_body_mode == BodyMode::None) ||
                    (conn.resp_body_mode == BodyMode::ContentLength &&
                     conn.resp_body_remaining == 0) ||
                    (conn.resp_body_mode == BodyMode::Chunked && chunked_done);
                if (!kDrainBodyDone) {
                    conn.set_slots(nullptr, &on_response_header_sent<Loop>, nullptr, nullptr);
                } else {
                    conn.set_slots(nullptr, &on_response_sent<Loop>, nullptr, nullptr);
                }
                conn.state = ConnState::Sending;
                conn.upstream_send_len = conn.upstream_recv_buf.len();
                loop->submit_send(conn, conn.send_buf.data(), conn.send_buf.len());
                return;
            }
        }
    }

    bool body_complete = false;
    if (conn.resp_body_mode == BodyMode::None) {
        body_complete = true;
    } else if (conn.resp_body_mode == BodyMode::ContentLength) {
        body_complete = (conn.resp_body_remaining == 0);
    } else if (conn.resp_body_mode == BodyMode::Chunked) {
        body_complete = chunked_done;
    }

    u32 actual_body = kInitialBodyLen;
    if (conn.resp_body_mode == BodyMode::None)
        actual_body = 0;
    else if (conn.resp_body_mode == BodyMode::ContentLength &&
             kInitialBodyLen > resp.content_length)
        actual_body = resp.content_length;
    else if (conn.resp_body_mode == BodyMode::Chunked)
        actual_body = chunked_consumed;
    u32 initial_send_len = kHeaderLen + actual_body;

    conn.resp_body_sent = initial_send_len;
    conn.upstream_send_len = initial_send_len;
    conn.state = ConnState::Sending;

    if (body_complete) {
        conn.set_slots(nullptr, &on_proxy_response_sent<Loop>, nullptr, nullptr);
        loop->submit_send(conn, conn.upstream_recv_buf.data(), initial_send_len);
    } else {
        conn.set_slots(nullptr, &on_response_header_sent<Loop>, nullptr, nullptr);
        loop->submit_send(conn, conn.upstream_recv_buf.data(), initial_send_len);
    }
}

template <typename Loop>
void on_proxy_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);
    conn.set_slots(nullptr, nullptr, nullptr, nullptr);

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    on_request_complete(loop, conn, conn.resp_status, conn.resp_body_sent);
    loop->epoch_leave();

    if (loop->is_draining()) {
        loop->close_conn(conn);
        return;
    }

    conn.upstream_recv_buf.reset();

    if (conn.upstream_fd >= 0) {
        ::close(conn.upstream_fd);
        conn.upstream_fd = -1;
    }
    loop->clear_upstream_fd(conn.id);
    conn.upstream_recv_armed = false;
    conn.upstream_send_armed = false;

    if (conn.pipeline_stash_len > 0 && conn.recv_buf.len() > 0) {
        const u16 kStashLen = conn.pipeline_stash_len;
        const u32 kLateLen = conn.recv_buf.len();
        if (static_cast<u32>(kStashLen) + kLateLen > conn.recv_buf.capacity()) {
            conn.pipeline_stash_len = 0;
            conn.send_buf.reset();
            loop->close_conn(conn);
            return;
        }
        conn.pipeline_stash_len = 0;
        conn.upstream_recv_buf.reset();
        conn.upstream_recv_buf.write(conn.recv_buf.data(), kLateLen);
        conn.recv_buf.reset();
        conn.recv_buf.write(conn.send_buf.data(), kStashLen);
        conn.recv_buf.write(conn.upstream_recv_buf.data(), kLateLen);
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
    conn.set_slots(&on_header_received<Loop>, nullptr, nullptr, nullptr);
    loop->submit_recv(conn);
}

}  // namespace rut
