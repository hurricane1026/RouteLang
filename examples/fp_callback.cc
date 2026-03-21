// Function-pointer callback model for io_uring.
// Complete example: accept → recv → parse → proxy → send → loop.
//
// Build: included in project, or standalone:
//   c++ -std=c++23 -fno-exceptions -fno-rtti -o fp_example fp_callback.cc

#include <stdint.h>
#include <unistd.h>

// ---- Minimal types (from our codebase) ----

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using i32 = int32_t;

struct IoEvent {
    u32 conn_id;
    u8  type;
    i32 result;
    u16 buf_id;
};

struct EventLoop;

// ---- Connection ----

struct Connection {
    // The only "state" needed: what to do when I/O completes.
    void (*on_complete)(EventLoop&, Connection&, IoEvent);

    i32  fd;
    u32  id;
    i32  upstream_fd;
    bool keep_alive;

    // Buffers (simplified)
    u8   recv_buf[4096];
    u32  recv_len;
    u8   send_buf[4096];
    u32  send_len;
};

// ---- Forward declarations of all callbacks ----

static void on_header_received(EventLoop&, Connection&, IoEvent);
static void on_upstream_connected(EventLoop&, Connection&, IoEvent);
static void on_upstream_sent(EventLoop&, Connection&, IoEvent);
static void on_upstream_response(EventLoop&, Connection&, IoEvent);
static void on_response_sent(EventLoop&, Connection&, IoEvent);

// ---- Fake EventLoop (for demonstration) ----

struct EventLoop {
    Connection conns[1024];

    // In real code these submit SQEs to io_uring.
    // Here they just record what would happen.
    void submit_recv(Connection& c)    { (void)c; /* add_recv(c.fd, c.id) */ }
    void submit_send(Connection& c)    { (void)c; /* add_send(c.fd, c.id, c.send_buf, c.send_len) */ }
    void submit_connect_upstream(Connection& c) { (void)c; /* add_connect(...) */ }
    void submit_send_upstream(Connection& c)    { (void)c; /* add_send(c.upstream_fd, ...) */ }
    void submit_recv_upstream(Connection& c)    { (void)c; /* add_recv(c.upstream_fd, c.id) */ }
    void close_conn(Connection& c)     { c.fd = -1; c.on_complete = nullptr; }

    // The entire event loop: one indirect call per completion.
    void dispatch(IoEvent ev) {
        auto& conn = conns[ev.conn_id];
        conn.on_complete(*this, conn, ev);
    }
};

// ---- Callbacks: each function is one step in the pipeline ----

// Step 1: new connection accepted → start reading headers
static void on_accepted(EventLoop& loop, Connection& conn, IoEvent ev) {
    conn.fd = ev.result;
    conn.keep_alive = true;

    conn.on_complete = on_header_received;
    loop.submit_recv(conn);
}

// Step 2: headers received → parse, connect to upstream
static void on_header_received(EventLoop& loop, Connection& conn, IoEvent ev) {
    if (ev.result <= 0) { loop.close_conn(conn); return; }

    conn.recv_len = static_cast<u32>(ev.result);
    // parse_request(conn.recv_buf, conn.recv_len) → determine upstream
    // For demo: just forward the request as-is

    conn.on_complete = on_upstream_connected;
    loop.submit_connect_upstream(conn);
}

// Step 3: upstream connected → forward request
static void on_upstream_connected(EventLoop& loop, Connection& conn, IoEvent ev) {
    if (ev.result < 0) {
        // upstream connect failed → 502
        conn.send_len = 26;
        __builtin_memcpy(conn.send_buf, "HTTP/1.1 502 Bad Gateway\r\n", 26);
        conn.on_complete = on_response_sent;
        loop.submit_send(conn);
        return;
    }

    // Forward the original request to upstream
    __builtin_memcpy(conn.send_buf, conn.recv_buf, conn.recv_len);
    conn.send_len = conn.recv_len;

    conn.on_complete = on_upstream_sent;
    loop.submit_send_upstream(conn);
}

// Step 4: request sent to upstream → wait for response
static void on_upstream_sent(EventLoop& loop, Connection& conn, IoEvent ev) {
    if (ev.result < 0) { loop.close_conn(conn); return; }

    conn.on_complete = on_upstream_response;
    loop.submit_recv_upstream(conn);
}

// Step 5: upstream response received → send to client
static void on_upstream_response(EventLoop& loop, Connection& conn, IoEvent ev) {
    if (ev.result <= 0) { loop.close_conn(conn); return; }

    __builtin_memcpy(conn.send_buf, conn.recv_buf, static_cast<u32>(ev.result));
    conn.send_len = static_cast<u32>(ev.result);

    conn.on_complete = on_response_sent;
    loop.submit_send(conn);
}

// Step 6: response sent → loop back or close
static void on_response_sent(EventLoop& loop, Connection& conn, IoEvent ev) {
    if (ev.result < 0) { loop.close_conn(conn); return; }

    if (conn.keep_alive) {
        // Next request on same connection
        conn.on_complete = on_header_received;
        loop.submit_recv(conn);
    } else {
        loop.close_conn(conn);
    }
}

// ---- fire-and-forget: zero extra infrastructure ----

static void on_mirror_sent(EventLoop&, Connection& mirror_conn, IoEvent) {
    // mirror done, just close
    mirror_conn.fd = -1;
    mirror_conn.on_complete = nullptr;
}

static void on_mirror_connected(EventLoop& loop, Connection& mirror_conn, IoEvent ev) {
    if (ev.result < 0) { mirror_conn.fd = -1; return; }

    // send the mirrored request
    mirror_conn.on_complete = on_mirror_sent;
    loop.submit_send(mirror_conn);
}

// Called from any step to mirror traffic — no Task, no coroutine, no spawn
static void fire_mirror(EventLoop& loop, Connection& origin) {
    // Grab a free connection for the mirror
    Connection& mc = loop.conns[999];  // simplified
    mc.fd = -1;
    mc.id = 999;
    __builtin_memcpy(mc.send_buf, origin.recv_buf, origin.recv_len);
    mc.send_len = origin.recv_len;

    mc.on_complete = on_mirror_connected;
    loop.submit_connect_upstream(mc);
    // origin continues independently — no waiting, no frame, no lifetime issues
}

// ---- Demo: simulate the full flow ----

static void write_str(const char* s) {
    int len = 0; while (s[len]) len++;
    (void)write(1, s, len);
}

int main() {
    EventLoop loop;

    // Simulate accept
    Connection& conn = loop.conns[0];
    conn.id = 0;
    conn.on_complete = on_accepted;

    IoEvent accept_ev = {0, 0, 42, 0};  // fd=42
    loop.dispatch(accept_ev);
    write_str("1. accepted, on_complete → on_header_received\n");

    // Simulate recv completion
    const char* fake_req = "GET /users/123 HTTP/1.1\r\n\r\n";
    int req_len = 0; while (fake_req[req_len]) req_len++;
    __builtin_memcpy(conn.recv_buf, fake_req, static_cast<u32>(req_len));
    IoEvent recv_ev = {0, 0, req_len, 0};
    loop.dispatch(recv_ev);
    write_str("2. headers parsed, on_complete → on_upstream_connected\n");

    // Simulate upstream connect completion
    IoEvent connect_ev = {0, 0, 0, 0};  // success
    loop.dispatch(connect_ev);
    write_str("3. upstream connected, on_complete → on_upstream_sent\n");

    // Fire mirror (no coroutine, no Task)
    fire_mirror(loop, conn);
    write_str("4. mirror fired (independent connection, zero overhead)\n");

    // Simulate upstream send completion
    IoEvent sent_ev = {0, 0, req_len, 0};
    loop.dispatch(sent_ev);
    write_str("5. request forwarded, on_complete → on_upstream_response\n");

    // Simulate upstream response
    const char* fake_resp = "HTTP/1.1 200 OK\r\n\r\n{\"id\":123}";
    int resp_len = 0; while (fake_resp[resp_len]) resp_len++;
    __builtin_memcpy(conn.recv_buf, fake_resp, static_cast<u32>(resp_len));
    IoEvent resp_ev = {0, 0, resp_len, 0};
    loop.dispatch(resp_ev);
    write_str("6. upstream responded, on_complete → on_response_sent\n");

    // Simulate client send completion
    IoEvent send_done_ev = {0, 0, resp_len, 0};
    loop.dispatch(send_done_ev);
    write_str("7. response sent, on_complete → on_header_received (keep-alive)\n");

    write_str("\nFull proxy cycle complete. Connection ready for next request.\n");
    return 0;
}
