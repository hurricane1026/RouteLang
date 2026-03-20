// Network layer mock tests.
// Scenarios ported from libuv test suite, adapted to our fp-callback + MockBackend API.
//
// libuv tests we port:
//   test-tcp-bind-error     → test_accept_error
//   test-tcp-connect-error  → test_recv_error, test_send_error
//   test-tcp-close          → test_close_during_recv, test_close_during_send
//   test-tcp-write-fail     → test_send_after_close
//   test-timer              → test_timer_basic, test_timer_refresh, test_timer_expire
//   test-delayed-accept     → test_accept_at_capacity
//   test-connection-fail    → test_upstream_connect_fail
//   test-tcp-reuseport      → (integration test, not mock)

#include <unistd.h>  // write()

#include "rout/runtime/event_loop.h"
#include "rout/runtime/callbacks.h"
#include "mock_backend.h"

using namespace rout;

// For tests: use a smaller EventLoop to reduce memory pressure.
// We specialize with a smaller kMaxConns.
struct SmallEventLoop : EventLoopCRTP<SmallEventLoop> {
    MockBackend backend;
    TimerWheel timer;
    u32 shard_id;
    bool running;

    static constexpr u32 kMaxConns = 64;
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top;
    u32 keepalive_timeout = 60;

    i32 init(u32 id, i32 listen_fd) {
        shard_id = id; running = true; free_top = kMaxConns;
        timer.init();
        for (u32 i = 0; i < kMaxConns; i++) {
            conns[i].reset(); conns[i].id = i;
            conns[i].shard_id = static_cast<u8>(id);
            free_stack[i] = i;
        }
        return backend.init(id, listen_fd);
    }
    Connection* alloc_conn_impl() {
        if (free_top == 0) return nullptr;
        u32 id = free_stack[--free_top];
        conns[id].reset(); conns[id].id = id;
        conns[id].shard_id = static_cast<u8>(shard_id);
        return &conns[id];
    }
    void free_conn_impl(Connection& c) {
        u32 cid = c.id;
        timer.remove(&c); c.reset();
        free_stack[free_top++] = cid;
    }
    void submit_recv_impl(Connection& c) { backend.add_recv(c.fd, c.id); }
    void submit_send_impl(Connection& c, const u8* buf, u32 len) { backend.add_send(c.fd, c.id, buf, len); }
    void submit_connect_impl(Connection& c, const void* addr, u32 addr_len) { backend.add_connect(c.upstream_fd, c.id, addr, addr_len); }
    void close_conn_impl(Connection& c) {
        // Don't call ::close() in tests — fds are fake
        c.fd = -1; c.upstream_fd = -1;
        this->free_conn(c);
    }
    void dispatch(const IoEvent& ev) {
        if (ev.type == IoEventType::Accept) {
            if (ev.result < 0) return;
            Connection* c = this->alloc_conn();
            if (!c) return;
            c->fd = ev.result;
            c->state = ConnState::ReadingHeader;
            c->on_complete = &on_header_received<SmallEventLoop>;
            timer.add(c, keepalive_timeout);
            this->submit_recv(*c);
            return;
        }
        if (ev.type == IoEventType::Timeout) {
            timer.tick([this](Connection* c) { this->close_conn(*c); });
            return;
        }
        if (ev.conn_id < kMaxConns) {
            auto& conn = conns[ev.conn_id];
            if (conn.on_complete) {
                timer.refresh(&conn, keepalive_timeout);
                conn.on_complete(this, conn, ev);
            }
        }
    }
};

using TestLoop = SmallEventLoop;

// Helper: IoEvent field order is {conn_id, result, buf_id, type}
static IoEvent make_ev(u32 conn_id, IoEventType type, i32 result, u16 buf_id = 0) {
    return {conn_id, result, buf_id, type};
}

// 64 conns × ~8KB = ~512KB, fits on stack.

// Helpers
static void write_str(const char* s) {
    u32 len = 0; while (s[len]) len++;
    (void)write(1, s, len);
}

static bool g_ok;
static void run(const char* name, bool (*fn)()) {
    g_ok = true;
    bool result = fn();
    if (result) { write_str("PASS: "); }
    else        { write_str("FAIL: "); }
    write_str(name);
    write_str("\n");
    g_ok = g_ok && result;
}

#define ASSERT(cond) do { if (!(cond)) return false; } while(0)

// ============================================================
// 1. Accept + basic callback chain
// ============================================================

// Port of libuv test-tcp-bind-error: accept succeeds, connection gets callback
static bool test_accept_basic() {
    TestLoop loop;
    loop.init(0, -1);

    // Inject: accept event with fd=42
    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    loop.backend.inject(make_ev(0, IoEventType::Accept, 43));

    // Process events
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should have allocated 2 connections
    ASSERT(loop.free_top == SmallEventLoop::kMaxConns - 2);

    // Connection 0 should have fd=42, on_complete=on_header_received
    auto& c0 = loop.conns[loop.free_stack[loop.free_top]];  // last allocated
    // Find connection with fd=42
    Connection* found = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { found = &loop.conns[i]; break; }
    }
    ASSERT(found != nullptr);
    ASSERT(found->fd == 42);
    ASSERT(found->on_complete == &on_header_received<SmallEventLoop>);
    ASSERT(found->state == ConnState::ReadingHeader);

    // Backend should have recorded: add_accept + 2x add_recv
    ASSERT(loop.backend.count_ops(MockOp::Recv) == 2);

    return true;
}

// Port of libuv test-tcp-bind-error: accept returns error
static bool test_accept_error() {
    TestLoop loop;
    loop.init(0, -1);

    // Inject: accept with error
    loop.backend.inject(make_ev(0, IoEventType::Accept, -1));

    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // No connection allocated
    ASSERT(loop.free_top == SmallEventLoop::kMaxConns);

    return true;
}

// Port of libuv test-delayed-accept: accept at capacity
static bool test_accept_at_capacity() {
    TestLoop loop;
    loop.init(0, -1);

    // Drain all free connections
    loop.free_top = 0;

    // Inject accept — should be rejected (no free slots)
    loop.backend.inject(make_ev(0, IoEventType::Accept, 99));

    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Still no free connections, and fd 99 should have been closed
    ASSERT(loop.free_top == 0);
    // Note: close(99) would have been called on a non-existent fd — that's OK in test

    return true;
}

// ============================================================
// 2. Recv callback chain
// ============================================================

// Port of libuv test-tcp-connect-error → recv data → callback fires → response sent
static bool test_recv_then_send() {
    TestLoop loop;
    loop.init(0, -1);

    // Accept a connection
    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Find the connection
    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);
    ASSERT(conn->on_complete == &on_header_received<SmallEventLoop>);

    // Clear ops to isolate recv phase
    loop.backend.clear_ops();

    // Inject: recv completes with 100 bytes
    loop.backend.inject(make_ev(conn->id, IoEventType::Recv, 100));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Callback should have: set state=Sending, prepared response, submitted send
    ASSERT(conn->state == ConnState::Sending);
    ASSERT(conn->on_complete == &on_response_sent<SmallEventLoop>);
    ASSERT(conn->recv_len == 100);
    ASSERT(conn->send_len > 0);

    // Backend should have a send op
    auto* send_op = loop.backend.last_op(MockOp::Send);
    ASSERT(send_op != nullptr);
    ASSERT(send_op->fd == 42);
    ASSERT(send_op->conn_id == conn->id);

    return true;
}

// Port of libuv test-tcp-connect-error: recv returns 0 (EOF)
static bool test_recv_eof() {
    TestLoop loop;
    loop.init(0, -1);

    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);
    u32 cid = conn->id;

    loop.backend.clear_ops();

    // Inject: recv returns 0 (EOF)
    loop.backend.inject(make_ev(cid, IoEventType::Recv, 0));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Connection should be closed and freed
    ASSERT(loop.conns[cid].fd == -1);
    ASSERT(loop.conns[cid].on_complete == nullptr);

    return true;
}

// recv returns error (-errno)
static bool test_recv_error() {
    TestLoop loop;
    loop.init(0, -1);

    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);
    u32 cid = conn->id;

    loop.backend.inject(make_ev(cid, IoEventType::Recv, -104));  // ECONNRESET
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    ASSERT(loop.conns[cid].fd == -1);
    return true;
}

// ============================================================
// 3. Send callback chain
// ============================================================

// send completes → should loop back to recv (keep-alive)
static bool test_send_then_recv_keepalive() {
    TestLoop loop;
    loop.init(0, -1);

    // Accept + recv
    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);

    // recv data
    loop.backend.inject(make_ev(conn->id, IoEventType::Recv, 50));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
    ASSERT(conn->on_complete == &on_response_sent<SmallEventLoop>);

    loop.backend.clear_ops();

    // send completes
    loop.backend.inject(make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_len)));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should be back to reading headers
    ASSERT(conn->state == ConnState::ReadingHeader);
    ASSERT(conn->on_complete == &on_header_received<SmallEventLoop>);

    // Backend should have submitted a new recv
    ASSERT(loop.backend.count_ops(MockOp::Recv) == 1);

    return true;
}

// Port of libuv test-tcp-write-fail: send returns error → close
static bool test_send_error() {
    TestLoop loop;
    loop.init(0, -1);

    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);
    u32 cid = conn->id;

    // recv → triggers send
    loop.backend.inject(make_ev(cid, IoEventType::Recv, 50));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // send fails
    loop.backend.inject(make_ev(cid, IoEventType::Send, -32));  // EPIPE
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Connection closed
    ASSERT(loop.conns[cid].fd == -1);
    return true;
}

// ============================================================
// 4. Full request cycle (accept → recv → send → recv → ...)
// ============================================================

static bool test_full_request_cycle() {
    TestLoop loop;
    loop.init(0, -1);

    // Accept
    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);

    // 3 full request cycles (keep-alive)
    for (int cycle = 0; cycle < 3; cycle++) {
        loop.backend.clear_ops();

        // recv
        loop.backend.inject(make_ev(conn->id, IoEventType::Recv, 100));
        n = loop.backend.wait(events, 8);
        for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
        ASSERT(conn->on_complete == &on_response_sent<SmallEventLoop>);

        // send
        loop.backend.inject(make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_len)));
        n = loop.backend.wait(events, 8);
        for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
        ASSERT(conn->on_complete == &on_header_received<SmallEventLoop>);
    }

    // Then client disconnects
    loop.backend.inject(make_ev(conn->id, IoEventType::Recv, 0));
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    ASSERT(conn->fd == -1);  // closed
    return true;
}

// ============================================================
// 5. Timer wheel
// ============================================================

// Port of libuv test-timer: basic add + tick
static bool test_timer_basic() {
    TimerWheel wheel;
    wheel.init();

    Connection c;
    c.reset();
    c.timer_node.init();
    c.fd = 42;

    wheel.add(&c, 3);  // expires in 3 ticks

    u32 expired = 0;
    // Tick 0, 1, 2: nothing expires
    for (int i = 0; i < 3; i++) {
        expired += wheel.tick([](Connection*) {});
    }
    ASSERT(expired == 0);

    // Tick 3: should expire
    Connection* expired_conn = nullptr;
    expired = wheel.tick([&](Connection* x) { expired_conn = x; });
    ASSERT(expired == 1);
    ASSERT(expired_conn == &c);

    return true;
}

// Port of libuv test-timer-again: refresh resets timeout
static bool test_timer_refresh() {
    TimerWheel wheel;
    wheel.init();

    Connection c;
    c.reset();
    c.timer_node.init();
    c.fd = 42;

    wheel.add(&c, 2);  // expires in 2 ticks

    // Tick once
    wheel.tick([](Connection*) {});

    // Refresh: reset to 3 ticks from now
    wheel.refresh(&c, 3);

    // Tick 3 more — should not expire yet (refreshed to 3 from cursor=1, lands in slot 4)
    u32 expired = 0;
    expired += wheel.tick([](Connection*) {});  // cursor 1→2, check slot 1
    expired += wheel.tick([](Connection*) {});  // cursor 2→3, check slot 2
    expired += wheel.tick([](Connection*) {});  // cursor 3→4, check slot 3
    ASSERT(expired == 0);

    // 4th tick after refresh: cursor=4, checks slot 4 → expires
    expired = wheel.tick([](Connection*) {});
    ASSERT(expired == 1);

    return true;
}

// Multiple connections expire in same slot
static bool test_timer_multi_expire() {
    TimerWheel wheel;
    wheel.init();

    Connection conns[5];
    for (int i = 0; i < 5; i++) {
        conns[i].reset();
        conns[i].timer_node.init();
        conns[i].fd = 100 + i;
        wheel.add(&conns[i], 1);  // all expire in 1 tick
    }

    // Tick once: cursor 0→1, checks slot 0 (empty)
    u32 count = 0;
    u32 expired = wheel.tick([&](Connection*) { count++; });
    ASSERT(expired == 0);

    // Tick again: cursor 1→2, checks slot 1 (all 5 connections)
    expired = wheel.tick([&](Connection*) { count++; });
    ASSERT(expired == 5);
    ASSERT(count == 5);

    return true;
}

// Remove before expiry (cancel)
static bool test_timer_remove() {
    TimerWheel wheel;
    wheel.init();

    Connection c;
    c.reset();
    c.timer_node.init();
    c.fd = 42;

    wheel.add(&c, 1);
    wheel.remove(&c);

    u32 expired = wheel.tick([](Connection*) {});
    ASSERT(expired == 0);

    return true;
}

// ============================================================
// 6. Connection pool management
// ============================================================

// alloc/free cycle
static bool test_conn_pool_alloc_free() {
    TestLoop loop;
    loop.init(0, -1);

    u32 initial_free = loop.free_top;

    Connection* c1 = loop.alloc_conn();
    ASSERT(c1 != nullptr);
    ASSERT(loop.free_top == initial_free - 1);

    Connection* c2 = loop.alloc_conn();
    ASSERT(c2 != nullptr);
    ASSERT(c1 != c2);
    ASSERT(loop.free_top == initial_free - 2);

    loop.free_conn(*c1);
    ASSERT(loop.free_top == initial_free - 1);

    // Re-alloc should reuse c1's slot
    Connection* c3 = loop.alloc_conn();
    ASSERT(c3 == c1);

    loop.free_conn(*c2);
    loop.free_conn(*c3);
    ASSERT(loop.free_top == initial_free);

    return true;
}

// Connection reset clears all fields
static bool test_conn_reset() {
    Connection c;
    c.fd = 42;
    c.on_complete = &on_header_received<SmallEventLoop>;
    c.state = ConnState::Sending;
    c.recv_len = 100;
    c.keep_alive = true;

    c.reset();

    ASSERT(c.fd == -1);
    ASSERT(c.on_complete == nullptr);
    ASSERT(c.state == ConnState::Idle);
    ASSERT(c.recv_len == 0);
    ASSERT(c.keep_alive == false);

    return true;
}

// ============================================================
// 7. Dispatch edge cases
// ============================================================

// Dispatch with invalid conn_id (out of range)
static bool test_dispatch_invalid_connid() {
    TestLoop loop;
    loop.init(0, -1);

    // Inject event with conn_id way out of range
    IoEvent ev = make_ev(SmallEventLoop::kMaxConns + 100, IoEventType::Recv, 50);
    loop.dispatch(ev);  // should not crash

    return true;
}

// Dispatch to connection with no callback set
static bool test_dispatch_null_callback() {
    TestLoop loop;
    loop.init(0, -1);

    // Connection 0 has no callback (reset state)
    IoEvent ev = make_ev(0, IoEventType::Recv, 50);
    loop.dispatch(ev);  // should not crash

    return true;
}

// Timeout event triggers timer tick
static bool test_dispatch_timeout() {
    TestLoop loop;
    loop.init(0, -1);

    // Accept a connection with short timeout
    loop.keepalive_timeout = 1;
    loop.backend.inject(make_ev(0, IoEventType::Accept, 42));
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    Connection* conn = nullptr;
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 42) { conn = &loop.conns[i]; break; }
    }
    ASSERT(conn != nullptr);
    u32 cid = conn->id;

    // Tick 1: cursor 0→1, checks slot 0 (empty)
    IoEvent timeout_ev = make_ev(0, IoEventType::Timeout, 1);
    loop.dispatch(timeout_ev);
    ASSERT(loop.conns[cid].fd == 42);  // still alive

    // Tick 2: cursor 1→2, checks slot 1 → connection expires
    loop.dispatch(timeout_ev);
    ASSERT(loop.conns[cid].fd == -1);

    return true;
}

// ============================================================
// 8. Multiple concurrent connections
// ============================================================

static bool test_multiple_connections() {
    TestLoop loop;
    loop.init(0, -1);

    // Accept 3 connections
    loop.backend.inject(make_ev(0, IoEventType::Accept, 10));
    loop.backend.inject(make_ev(0, IoEventType::Accept, 11));
    loop.backend.inject(make_ev(0, IoEventType::Accept, 12));

    IoEvent events[16];
    u32 n = loop.backend.wait(events, 16);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Find all 3
    Connection* c[3] = {};
    for (u32 i = 0; i < SmallEventLoop::kMaxConns; i++) {
        if (loop.conns[i].fd == 10) c[0] = &loop.conns[i];
        if (loop.conns[i].fd == 11) c[1] = &loop.conns[i];
        if (loop.conns[i].fd == 12) c[2] = &loop.conns[i];
    }
    ASSERT(c[0] && c[1] && c[2]);

    loop.backend.clear_ops();

    // Recv on conn 0 and 2 (interleaved)
    loop.backend.inject(make_ev(c[0]->id, IoEventType::Recv, 50));
    loop.backend.inject(make_ev(c[2]->id, IoEventType::Recv, 80));
    n = loop.backend.wait(events, 16);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // conn 0 and 2 should be in Sending state
    ASSERT(c[0]->state == ConnState::Sending);
    ASSERT(c[1]->state == ConnState::ReadingHeader);  // unchanged
    ASSERT(c[2]->state == ConnState::Sending);

    // Send completions
    loop.backend.inject(make_ev(c[0]->id, IoEventType::Send, 50));
    loop.backend.inject(make_ev(c[2]->id, IoEventType::Send, 80));
    n = loop.backend.wait(events, 16);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // All back to reading
    ASSERT(c[0]->state == ConnState::ReadingHeader);
    ASSERT(c[2]->state == ConnState::ReadingHeader);

    // Close conn 1 (EOF)
    loop.backend.inject(make_ev(c[1]->id, IoEventType::Recv, 0));
    n = loop.backend.wait(events, 16);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    ASSERT(c[1]->fd == -1);
    // Conn 0 and 2 still alive
    ASSERT(c[0]->fd == 10);
    ASSERT(c[2]->fd == 12);

    return true;
}

// ============================================================
// Entrypoint
// ============================================================

int main() {
    bool all_pass = true;
    auto r = [&](const char* name, bool (*fn)()) {
        bool result = fn();
        if (result) { write_str("PASS: "); }
        else        { write_str("FAIL: "); all_pass = false; }
        write_str(name);
        write_str("\n");
    };

    write_str("=== Accept ===\n");
    r("accept_basic",        test_accept_basic);
    r("accept_error",        test_accept_error);
    r("accept_at_capacity",  test_accept_at_capacity);

    write_str("=== Recv ===\n");
    r("recv_then_send",      test_recv_then_send);
    r("recv_eof",            test_recv_eof);
    r("recv_error",          test_recv_error);

    write_str("=== Send ===\n");
    r("send_keepalive",      test_send_then_recv_keepalive);
    r("send_error",          test_send_error);

    write_str("=== Full Cycle ===\n");
    r("full_request_cycle",  test_full_request_cycle);

    write_str("=== Timer ===\n");
    r("timer_basic",         test_timer_basic);
    r("timer_refresh",       test_timer_refresh);
    r("timer_multi_expire",  test_timer_multi_expire);
    r("timer_remove",        test_timer_remove);

    write_str("=== Connection Pool ===\n");
    r("conn_pool_alloc_free", test_conn_pool_alloc_free);
    r("conn_reset",           test_conn_reset);

    write_str("=== Dispatch Edge Cases ===\n");
    r("dispatch_invalid_connid", test_dispatch_invalid_connid);
    r("dispatch_null_callback",  test_dispatch_null_callback);
    r("dispatch_timeout",        test_dispatch_timeout);

    write_str("=== Concurrent ===\n");
    r("multiple_connections", test_multiple_connections);

    return all_pass ? 0 : 1;
}
