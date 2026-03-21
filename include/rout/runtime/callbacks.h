#pragma once

#include "rout/runtime/connection.h"
#include "rout/runtime/io_event.h"

namespace rout {

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

static const char kResponse200[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "OK";

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

    conn.state = ConnState::Sending;
    conn.keep_alive = true;  // 200 OK response uses Connection: keep-alive

    conn.send_buf.reset();
    conn.send_buf.write(reinterpret_cast<const u8*>(kResponse200), sizeof(kResponse200) - 1);

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

    if (ev.result < 0 || !conn.keep_alive) {
        loop->close_conn(conn);
        return;
    }

    // Validate full send — partial sends (possible with io_uring) would serve
    // truncated responses. Close rather than risk corruption on keep-alive.
    if (static_cast<u32>(ev.result) != conn.send_buf.len()) {
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

    if (ev.type != IoEventType::Send) {
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
    conn.recv_buf.reset();
    conn.on_complete = &on_upstream_response<Loop>;
    loop->submit_recv_upstream(conn);
}

// Step: upstream response received, forward to client.
template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.type != IoEventType::Recv) {
        loop->close_conn(conn);
        return;
    }

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
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

    // Upstream response forwarded — reset recv_buf for next request.
    conn.recv_buf.reset();

    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

}  // namespace rout
