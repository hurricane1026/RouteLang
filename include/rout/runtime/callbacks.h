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

    u32 resp_len = sizeof(kResponse200) - 1;
    __builtin_memcpy(conn.send_buf, kResponse200, resp_len);
    conn.send_len = resp_len;

    conn.on_complete = &on_response_sent<Loop>;
    loop->submit_send(conn, conn.send_buf, conn.send_len);
}

template <typename Loop>
void on_response_sent(void* lp, Connection& conn, IoEvent ev) {
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
