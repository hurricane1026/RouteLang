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

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    conn.recv_len = static_cast<u32>(ev.result);
    conn.state = ConnState::Sending;
    conn.keep_alive = true;  // 200 OK response uses Connection: keep-alive

    u32 resp_len = sizeof(kResponse200) - 1;
    __builtin_memcpy(conn.send_buf, kResponse200, resp_len);
    conn.send_len = resp_len;

    conn.on_complete = &on_response_sent<Loop>;
    loop->submit_send(conn, conn.send_buf, conn.send_len);
}

template <typename Loop>
void on_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result < 0 || !conn.keep_alive) {
        loop->close_conn(conn);
        return;
    }

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

    if (ev.result < 0) {
        // Upstream connect failed → 502
        static const char k502[] =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Length: 11\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Bad Gateway";
        u32 len = sizeof(k502) - 1;
        __builtin_memcpy(conn.send_buf, k502, len);
        conn.send_len = len;
        conn.keep_alive = false;  // Connection: close — don't loop back
        conn.on_complete = &on_response_sent<Loop>;
        loop->submit_send(conn, conn.send_buf, conn.send_len);
        return;
    }

    conn.state = ConnState::Proxying;
    // Forward the original request to upstream (upstream_fd, not fd)
    conn.on_complete = &on_upstream_request_sent<Loop>;
    loop->submit_send_upstream(conn, conn.recv_buf, conn.recv_len);
}

// Step: request forwarded to upstream, now wait for upstream response.
template <typename Loop>
void on_upstream_request_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    conn.on_complete = &on_upstream_response<Loop>;
    loop->submit_recv_upstream(conn);
}

// Step: upstream response received, forward to client.
template <typename Loop>
void on_upstream_response(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result <= 0) {
        loop->close_conn(conn);
        return;
    }

    conn.recv_len = static_cast<u32>(ev.result);
    // Forward upstream response to downstream client
    conn.on_complete = &on_proxy_response_sent<Loop>;
    conn.state = ConnState::Sending;
    loop->submit_send(conn, conn.recv_buf, conn.recv_len);
}

// Step: response sent to client, go back to reading next request (keep-alive).
template <typename Loop>
void on_proxy_response_sent(void* lp, Connection& conn, IoEvent ev) {
    auto* loop = static_cast<Loop*>(lp);

    if (ev.result < 0) {
        loop->close_conn(conn);
        return;
    }

    conn.state = ConnState::ReadingHeader;
    conn.on_complete = &on_header_received<Loop>;
    loop->submit_recv(conn);
}

}  // namespace rout
