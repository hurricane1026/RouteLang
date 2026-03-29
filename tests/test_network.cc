// Mock tests — no real sockets. Ported from libuv/libevent scenarios.
#include "rut/runtime/arena.h"
#include "rut/runtime/error.h"
#include "rut/runtime/route_table.h"
#include "rut/runtime/slab_pool.h"
#include "rut/runtime/slice_pool.h"
#include "rut/runtime/upstream_pool.h"
#include "test.h"
#include "test_helpers.h"

// === Accept ===

TEST(accept, basic) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    CHECK_EQ(loop.free_top, SmallLoop::kMaxConns - 2);
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(c->state, ConnState::ReadingHeader);
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 2u);
}

TEST(accept, error) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, -1));
    CHECK_EQ(loop.free_top, SmallLoop::kMaxConns);
}

TEST(accept, at_capacity) {
    SmallLoop loop;
    loop.setup();
    loop.free_top = 0;
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 99));
    CHECK_EQ(loop.free_top, 0u);
}

// === Recv ===

TEST(recv, then_send) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    CHECK_EQ(c->state, ConnState::Sending);
    CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);
    // recv_buf preserved until on_response_sent (allows proxy to read it)
    CHECK_EQ(c->recv_buf.len(), 100u);
    CHECK_GT(c->send_buf.len(), 0u);
    auto* op = loop.backend.last_op(MockOp::Send);
    REQUIRE(op != nullptr);
    CHECK_EQ(op->fd, 42);
}

TEST(recv, eof) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 0));
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK(loop.conns[cid].on_complete == nullptr);
}

TEST(recv, error_connreset) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, -104));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Send ===

TEST(send, keepalive_loop) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);
    loop.backend.clear_ops();
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(c->state, ConnState::ReadingHeader);
    CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
}

TEST(send, error_epipe) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -32));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Full Cycle ===

TEST(cycle, three_requests_then_eof) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    for (int i = 0; i < 3; i++) {
        loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
        CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);
        loop.inject_and_dispatch(
            make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
        CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);
    }
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 0));
    CHECK_EQ(c->fd, -1);
}

// === Timer ===

TEST(timer, fire_after_n) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    c.fd = 1;
    w.add(&c, 5);
    for (int i = 0; i < 5; i++) CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 1u);
}

TEST(timer, restart_replaces) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 2);
    w.tick([](Connection*) {});
    w.remove(&c);
    w.add(&c, 3);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 1u);
}

TEST(timer, zero_timeout) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 0);
    CHECK_EQ(w.tick([](Connection*) {}), 1u);
}

TEST(timer, large_timeout_wrap) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 1000);  // 1000 & 63 = 40
    for (u32 i = 0; i < 40; i++) CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 1u);
}

TEST(timer, refresh_resets) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 3);
    w.tick([](Connection*) {});
    w.tick([](Connection*) {});
    w.refresh(&c, 3);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
    CHECK_EQ(w.tick([](Connection*) {}), 1u);
}

TEST(timer, cancel) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 1);
    w.remove(&c);
    w.tick([](Connection*) {});
    CHECK_EQ(w.tick([](Connection*) {}), 0u);
}

TEST(timer, multi_same_slot) {
    TimerWheel w;
    w.init();
    Connection conns[100];
    for (int i = 0; i < 100; i++) {
        conns[i].reset();
        conns[i].timer_node.init();
        w.add(&conns[i], 1);
    }
    w.tick([](Connection*) {});
    u32 count = 0;
    w.tick([&](Connection*) { count++; });
    CHECK_EQ(count, 100u);
}

TEST(timer, multiple_diff_timeouts) {
    TimerWheel w;
    w.init();
    Connection conns[4];
    for (int i = 0; i < 4; i++) {
        conns[i].reset();
        conns[i].timer_node.init();
        w.add(&conns[i], static_cast<u32>(i + 1));
    }
    u32 total = 0;
    for (int t = 0; t < 6; t++) total += w.tick([](Connection*) {});
    CHECK_EQ(total, 4u);
}

TEST(timer, self_restart) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 0);  // slot 0
    int fires = 0;
    // tick: cursor=0, fires at slot 0. Callback re-adds at (0+1)&63=1. cursor→1.
    w.tick([&](Connection* conn) {
        fires++;
        if (fires < 3) {
            conn->timer_node.init();
            w.add(conn, 1);
        }
    });
    CHECK_EQ(fires, 1);
    // tick: cursor=1, checks slot 1 → fires. Re-adds at (1+1)=2. cursor→2.
    w.tick([&](Connection* conn) {
        fires++;
        if (fires < 3) {
            conn->timer_node.init();
            w.add(conn, 1);
        }
    });
    CHECK_EQ(fires, 2);
    // tick: cursor=2, checks slot 2 → fires. fires=3, no re-add. cursor→3.
    w.tick([&](Connection*) { fires++; });
    CHECK_EQ(fires, 3);
}

TEST(timer, empty_tick) {
    TimerWheel w;
    w.init();
    for (int i = 0; i < 100; i++) CHECK_EQ(w.tick([](Connection*) {}), 0u);
}

TEST(timer, huge_no_crash) {
    TimerWheel w;
    w.init();
    Connection c;
    c.reset();
    c.timer_node.init();
    w.add(&c, 0xFFFFFFFF);
    for (int i = 0; i < 100; i++) w.tick([](Connection*) {});
    w.remove(&c);
    CHECK(true);  // no crash
}

TEST(timer, stress_200_wraparound) {
    TimerWheel w;
    w.init();
    Connection conns[200];
    for (int i = 0; i < 200; i++) {
        conns[i].reset();
        conns[i].timer_node.init();
        w.add(&conns[i], static_cast<u32>(i % 64 + 1));
    }
    u32 total = 0;
    for (int t = 0; t < 70; t++) total += w.tick([](Connection*) {});
    CHECK_EQ(total, 200u);
}

// === Connection Pool ===

TEST(pool, alloc_free_reuse) {
    SmallLoop loop;
    loop.setup();
    u32 init = loop.free_top;
    Connection* c1 = loop.alloc_conn();
    Connection* c2 = loop.alloc_conn();
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    CHECK_NE(c1, c2);
    CHECK_EQ(loop.free_top, init - 2);
    loop.free_conn(*c1);
    Connection* c3 = loop.alloc_conn();
    CHECK_EQ(c3, c1);
    loop.free_conn(*c2);
    loop.free_conn(*c3);
    CHECK_EQ(loop.free_top, init);
}

TEST(pool, exhaust_and_recover) {
    SmallLoop loop;
    loop.setup();
    u32 init = loop.free_top;
    Connection* ptrs[SmallLoop::kMaxConns];
    for (u32 i = 0; i < SmallLoop::kMaxConns; i++) {
        ptrs[i] = loop.alloc_conn();
        REQUIRE(ptrs[i] != nullptr);
    }
    CHECK(loop.alloc_conn() == nullptr);
    for (u32 i = 0; i < SmallLoop::kMaxConns; i++) loop.free_conn(*ptrs[i]);
    CHECK_EQ(loop.free_top, init);
    CHECK(loop.alloc_conn() != nullptr);
}

TEST(pool, reset_clears_fields) {
    SmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    c->on_complete = &on_header_received<SmallLoop>;
    c->state = ConnState::Sending;
    // Write some data into recv_buf
    const u8 data[] = "hello";
    c->recv_buf.write(data, 5);
    c->keep_alive = true;
    u32 cid = c->id;
    loop.free_conn(*c);
    // After free, connection is fully reset (sync backend frees immediately).
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK(loop.conns[cid].on_complete == nullptr);
    CHECK_EQ(loop.conns[cid].state, ConnState::Idle);
    CHECK_EQ(loop.conns[cid].keep_alive, false);
    CHECK_EQ(loop.conns[cid].recv_slice, nullptr);
    CHECK_EQ(loop.conns[cid].send_slice, nullptr);
    CHECK_EQ(loop.conns[cid].recv_buf.write_avail(), 0u);
    CHECK_EQ(loop.conns[cid].send_buf.write_avail(), 0u);
}

// === Dispatch Edge Cases ===

TEST(dispatch, invalid_connid) {
    SmallLoop loop;
    loop.setup();
    loop.dispatch(make_ev(SmallLoop::kMaxConns + 100, IoEventType::Recv, 50));
    CHECK(true);  // no crash
}

TEST(dispatch, null_callback) {
    SmallLoop loop;
    loop.setup();
    loop.dispatch(make_ev(0, IoEventType::Recv, 50));
    CHECK(true);
}

TEST(dispatch, timeout_expires_conn) {
    SmallLoop loop;
    loop.setup();
    loop.keepalive_timeout = 1;
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, 42);  // still alive
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);  // expired
}

// === Concurrent ===

TEST(concurrent, multiple_connections) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 10));
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 11));
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 12));
    auto* c0 = loop.find_fd(10);
    auto* c1 = loop.find_fd(11);
    auto* c2 = loop.find_fd(12);
    REQUIRE(c0 && c1 && c2);
    loop.inject_and_dispatch(make_ev(c0->id, IoEventType::Recv, 50));
    loop.inject_and_dispatch(make_ev(c2->id, IoEventType::Recv, 80));
    CHECK_EQ(c0->state, ConnState::Sending);
    CHECK_EQ(c1->state, ConnState::ReadingHeader);
    CHECK_EQ(c2->state, ConnState::Sending);
    loop.inject_and_dispatch(make_ev(c1->id, IoEventType::Recv, 0));
    CHECK_EQ(c1->fd, -1);
    CHECK_EQ(c0->fd, 10);
    CHECK_EQ(c2->fd, 12);
}

TEST(concurrent, alternating_accept_close) {
    SmallLoop loop;
    loop.setup();
    for (int i = 0; i < 20; i++) {
        loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 200 + i));
        auto* c = loop.find_fd(200 + i);
        REQUIRE(c != nullptr);
        loop.close_conn(*c);
    }
    CHECK_EQ(loop.free_top, SmallLoop::kMaxConns);
}

TEST(concurrent, all_conns_recv_simultaneously) {
    SmallLoop loop;
    loop.setup();
    for (int i = 0; i < 10; i++) loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 100 + i));
    for (u32 i = 0; i < SmallLoop::kMaxConns; i++)
        if (loop.conns[i].fd >= 100 && loop.conns[i].fd < 110)
            loop.inject_and_dispatch(make_ev(i, IoEventType::Recv, 50));
    int sending = 0;
    for (u32 i = 0; i < SmallLoop::kMaxConns; i++)
        if (loop.conns[i].state == ConnState::Sending) sending++;
    CHECK_EQ(sending, 10);
}

// === Copilot-found issues — regression tests ===

// Copilot #1: IoEvent.has_buf must distinguish "no buffer" from buf_id=0.
// Without has_buf, buf_id=0 is ambiguous (valid buffer vs no buffer).
TEST(copilot, has_buf_distinguishes_no_buffer) {
    // Event with no buffer
    IoEvent no_buf = make_ev(0, IoEventType::Recv, 100);
    CHECK_EQ(no_buf.has_buf, 0u);
    CHECK_EQ(no_buf.buf_id, 0u);

    // Event with buffer id 0 (valid)
    IoEvent with_buf = {0, 100, 0, 1, IoEventType::Recv};
    CHECK_EQ(with_buf.has_buf, 1u);
    CHECK_EQ(with_buf.buf_id, 0u);

    // They must be distinguishable
    CHECK_NE(no_buf.has_buf, with_buf.has_buf);
}

// Copilot #3: epoll add_recv after add_send must not fail with EEXIST.
// After send completes → callback calls submit_recv → add_recv.
// If the fd was already registered for EPOLLOUT, EPOLL_CTL_ADD fails.
// Fix: add_recv uses MOD first, falls back to ADD.
TEST(copilot, recv_after_send_keepalive_works) {
    // This is already covered by send.keepalive_loop, but let's be explicit:
    // accept → recv → send → (send completes) → recv again
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // recv → triggers send
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);

    // send completes → callback calls submit_recv
    loop.backend.clear_ops();
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));

    // The submit_recv call must have succeeded (add_recv recorded)
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
    CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);

    // Do it again — second cycle
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    loop.backend.clear_ops();
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
}

// Copilot #6: timer wheel comment said 60 slots but code uses 64.
// Verify the actual slot count.
TEST(copilot, timer_wheel_has_64_slots) {
    CHECK_EQ(TimerWheel::kSlots, 64u);
}

// Copilot #4: io_backend.h documented init as void, but it returns i32.
// Verify MockBackend.init returns 0 on success.
TEST(copilot, backend_init_returns_success) {
    SmallLoop loop;
    loop.setup();
    // setup() calls backend.init which returns Expected<void, Error>
    CHECK(loop.backend.init(0, -1).has_value());
}

// === Proxy (mock) ===

// Full proxy cycle: accept → recv → connect upstream → send to upstream →
//                   recv from upstream → send to client
TEST(proxy, full_cycle) {
    SmallLoop loop;
    loop.setup();
    // Accept + recv request
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));

    // Switch to proxy mode
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
    CHECK_EQ(conn->state, ConnState::Proxying);
    CHECK_EQ(conn->on_complete, &on_upstream_request_sent<SmallLoop>);
    CHECK_EQ(loop.backend.count_ops(MockOp::Send), 1u);
    // Verify send went to upstream_fd (100), not client fd (42)
    auto* send_op = loop.backend.last_op(MockOp::Send);
    CHECK(send_op != nullptr);
    CHECK_EQ(send_op->fd, 100);  // upstream_fd, not client fd

    // Request sent to upstream → wait for response
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
    CHECK_EQ(conn->on_complete, &on_upstream_response<SmallLoop>);
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);

    // Upstream response → forward to client (inject valid HTTP response)
    loop.backend.clear_ops();
    inject_upstream_response(loop, *conn);
    CHECK_EQ(conn->state, ConnState::Sending);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
    CHECK_EQ(loop.backend.count_ops(MockOp::Send), 1u);

    // Response sent to client → back to reading (keep-alive)
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
}

// Upstream connect fails → 502 Bad Gateway
TEST(proxy, connect_fail_502) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;

    // Connect fails
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, -111));
    // Should send 502 to client
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    CHECK_GT(conn->send_buf.len(), 0u);
    // Verify "502" in send buffer
    bool has_502 = false;
    const u8* sb = conn->send_buf.data();
    for (u32 i = 0; i + 2 < conn->send_buf.len(); i++) {
        if (sb[i] == '5' && sb[i + 1] == '0' && sb[i + 2] == '2') {
            has_502 = true;
            break;
        }
    }
    CHECK(has_502);
    // 502 sets keep_alive=false → on_response_sent will close, not loop
    CHECK_EQ(conn->keep_alive, false);
    // Simulate send completion → connection should be closed
    u32 cid = conn->id;
    loop.inject_and_dispatch(
        make_ev(cid, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
    CHECK_EQ(loop.conns[cid].fd, -1);  // closed, not looped back
}

// Upstream send fails → close
TEST(proxy, upstream_send_error) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;

    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -32));  // EPIPE
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// Upstream response EOF → close
TEST(proxy, upstream_eof) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;

    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 100));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamRecv, 0));  // EOF
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// Client send error after proxy → close
TEST(proxy, client_send_error) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;

    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, 100));
    inject_upstream_response(loop, *conn);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -32));  // client EPIPE
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// Two proxy cycles on same connection (keep-alive)
TEST(proxy, keepalive_two_cycles) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    for (int cycle = 0; cycle < 2; cycle++) {
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
        conn->upstream_fd = 100 + cycle;
        conn->on_complete = &on_upstream_connected<SmallLoop>;
        conn->state = ConnState::Proxying;
        loop.submit_connect(*conn, nullptr, 0);
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
        inject_upstream_response(loop, *conn);
        loop.inject_and_dispatch(
            make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));
        CHECK_EQ(conn->state, ConnState::ReadingHeader);
    }
}

// === recv-into-buffer integration ===

// Verify recv_buf data() pointer is stable and readable after recv
TEST(recv_buf, data_accessible_after_recv) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // Manually write real data to simulate what epoll wait() does
    c->recv_buf.reset();
    const u8 req[] = "GET / HTTP/1.1\r\n\r\n";
    c->recv_buf.write(req, sizeof(req) - 1);

    CHECK_EQ(c->recv_buf.len(), sizeof(req) - 1);
    CHECK_EQ(c->recv_buf.data()[0], 'G');
    CHECK_EQ(c->recv_buf.data()[1], 'E');
    CHECK_EQ(c->recv_buf.data()[2], 'T');
}

// Verify recv_buf doesn't accumulate across keepalive cycles.
// Callback resets recv_buf after consuming; backend appends to clean buffer.
TEST(recv_buf, reset_between_keepalive_cycles) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // First request: 200 bytes → on_header_received preserves recv_buf
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 200));
    CHECK_EQ(c->recv_buf.len(), 200u);  // still has data

    // Send response → on_response_sent resets recv_buf before next recv
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(c->state, ConnState::ReadingHeader);
    CHECK_EQ(c->recv_buf.len(), 0u);  // reset by on_response_sent

    // Second request: 50 bytes → appended to clean buffer
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->recv_buf.len(), 50u);  // preserved until send completes
    CHECK_EQ(c->state, ConnState::Sending);
}

// Verify recv_buf capacity via direct Buffer API (not through dispatch)
TEST(recv_buf, capacity_boundary) {
    SmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    // Fill recv_buf to capacity
    u32 avail = c->recv_buf.write_avail();
    CHECK_EQ(avail, SmallLoop::kBufSize);
    c->recv_buf.commit(avail);
    CHECK_EQ(c->recv_buf.len(), SmallLoop::kBufSize);
    CHECK_EQ(c->recv_buf.write_avail(), 0u);
    loop.free_conn(*c);
}

// Verify recv_buf survives connection reuse (alloc → close → alloc)
TEST(recv_buf, survives_connection_reuse) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // recv → preserved → send response (resets) → EOF → close
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(c->recv_buf.len(), 100u);  // preserved until send
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(c->state, ConnState::ReadingHeader);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 0));  // EOF → close
    CHECK_EQ(loop.conns[cid].fd, -1);

    // Reuse same slot
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 55));
    // Find which slot got reused
    auto* c2 = loop.find_fd(55);
    REQUIRE(c2 != nullptr);
    // recv_buf must be fresh after reset
    CHECK_EQ(c2->recv_buf.len(), 0u);
    CHECK(c2->recv_buf.valid());
    CHECK_EQ(c2->recv_buf.write_avail(), SmallLoop::kBufSize);
}

// Proxy: verify recv_buf.data() is what gets forwarded upstream
TEST(recv_buf, proxy_forwards_recv_data) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Simulate recv with real request data
    conn->recv_buf.reset();
    const u8 req[] = "GET /api HTTP/1.1\r\nHost: upstream\r\n\r\n";
    conn->recv_buf.write(req, sizeof(req) - 1);

    // Switch to proxy mode
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds → should forward recv_buf content
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->fd, 100);  // upstream_fd
    // Verify the send buffer points to recv_buf data
    CHECK_EQ(send_op->send_buf, conn->recv_buf.data());
    CHECK_EQ(send_op->send_len, conn->recv_buf.len());
}

// Verify Buffer invariants hold through full proxy round-trip
TEST(recv_buf, buffer_state_through_proxy_cycle) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // recv request — preserved in recv_buf
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
    CHECK_EQ(conn->recv_buf.len(), 100u);
    CHECK(!conn->recv_buf.is_released());

    // proxy flow
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));

    // upstream response → upstream_recv_buf gets new data (valid HTTP response)
    inject_upstream_response(loop, *conn);
    CHECK_EQ(conn->upstream_recv_buf.len(), kMockHttpResponseLen);
    CHECK(!conn->upstream_recv_buf.is_released());

    // send to client → back to ReadingHeader
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    // Buffer still valid for next request
    CHECK(conn->recv_buf.valid());
}

// recv error doesn't corrupt buffer state; connection is closed
TEST(recv_buf, error_closes_cleanly) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // First recv → callback consumes + resets
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->recv_buf.len(), 50u);  // preserved

    // Send response → on_response_sent resets recv_buf
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(c->recv_buf.len(), 0u);  // reset

    // Error recv — connection closed
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, -104));
    CHECK_EQ(loop.conns[cid].fd, -1);  // closed
}

// === Semantic correctness: append-based recv + callback-driven reset ===

// Multi-packet recv: two recv events accumulate before callback runs.
// This tests the core semantic fix — backend appends, callback resets.
TEST(recv_semantic, multi_packet_accumulation) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // Manually simulate two recv packets WITHOUT going through dispatch.
    // This mimics what a real backend does when two packets arrive before
    // the event loop dispatches the first.
    c->recv_buf.commit(100);            // first packet
    c->recv_buf.commit(50);             // second packet appended
    CHECK_EQ(c->recv_buf.len(), 150u);  // accumulated

    // Now dispatch a recv event — callback sees total accumulated data
    IoEvent ev = make_ev(c->id, IoEventType::Recv, 150);
    loop.dispatch(ev);
    CHECK_EQ(c->state, ConnState::Sending);
    CHECK_EQ(c->recv_buf.len(), 150u);  // preserved until on_response_sent
}

// recv when recv_buf is full: backend should not crash or corrupt
TEST(recv_semantic, buffer_full_no_crash) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // Fill recv_buf to capacity
    c->recv_buf.commit(4096);
    CHECK_EQ(c->recv_buf.write_avail(), 0u);

    // inject_and_dispatch with result > 0 tries to commit but buf is full.
    // Mock converts to -ENOBUFS, callback sees error → closes connection.
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    CHECK_EQ(loop.conns[cid].fd, -1);  // closed due to -ENOBUFS
}

// send_state cleanup: verify partial send state doesn't leak across connection reuse
TEST(recv_semantic, send_state_no_leak_across_reuse) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Normal recv-send cycle
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(c->send_buf.len())));

    // Close connection
    loop.close_conn(*c);
    CHECK_EQ(loop.conns[cid].fd, -1);

    // Reuse slot — recv_buf and send_buf should be clean
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 55));
    auto* c2 = loop.find_fd(55);
    REQUIRE(c2 != nullptr);
    CHECK_EQ(c2->recv_buf.len(), 0u);
    CHECK_EQ(c2->send_buf.len(), 0u);
    CHECK_EQ(c2->recv_buf.write_avail(), SmallLoop::kBufSize);
    CHECK_EQ(c2->send_buf.write_avail(), SmallLoop::kBufSize);

    // New cycle works cleanly
    loop.inject_and_dispatch(make_ev(c2->id, IoEventType::Recv, 80));
    CHECK_EQ(c2->state, ConnState::Sending);
    CHECK_GT(c2->send_buf.len(), 0u);
}

// Proxy: recv_buf not reset until proxy response fully sent
TEST(recv_semantic, proxy_recv_buf_lifetime) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Receive original request — preserved for proxy forwarding
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
    CHECK_EQ(conn->recv_buf.len(), 100u);  // preserved

    // Switch to proxy mode and connect upstream
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;

    // Upstream connect succeeds → forwards recv_buf to upstream
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Upstream request sent → on_upstream_request_sent resets upstream_recv_buf for response.
    // recv_buf retains original request data (not touched).
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
    CHECK_EQ(conn->upstream_recv_buf.len(), 0u);  // reset by on_upstream_request_sent

    // Upstream response received → data goes into upstream_recv_buf (valid HTTP response)
    inject_upstream_response(loop, *conn);
    // on_upstream_response does NOT reset upstream_recv_buf (send still in progress)
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // Proxy response sent → on_proxy_response_sent resets upstream_recv_buf and recv_buf.
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));
    CHECK_EQ(conn->upstream_recv_buf.len(), 0u);  // reset by on_proxy_response_sent
    CHECK_EQ(conn->recv_buf.len(), 0u);           // reset by on_proxy_response_sent (keep-alive)
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
}

// Connection alloc+free clears buffers, re-alloc rebinds them
TEST(recv_semantic, reset_clears_both_buffers) {
    SmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);

    // Write data to both buffers
    c->recv_buf.write(reinterpret_cast<const u8*>("GET"), 3);
    c->send_buf.write(reinterpret_cast<const u8*>("HTTP"), 4);
    CHECK_EQ(c->recv_buf.len(), 3u);
    CHECK_EQ(c->send_buf.len(), 4u);

    loop.free_conn(*c);

    // Re-alloc: buffers should be rebound and empty
    Connection* c2 = loop.alloc_conn();
    REQUIRE(c2 != nullptr);
    CHECK_EQ(c2->recv_buf.len(), 0u);
    CHECK_EQ(c2->send_buf.len(), 0u);
    CHECK(c2->recv_buf.valid());
    CHECK(c2->send_buf.valid());
    CHECK_EQ(c2->recv_buf.write_avail(), SmallLoop::kBufSize);
    CHECK_EQ(c2->send_buf.write_avail(), SmallLoop::kBufSize);
    CHECK(!c2->recv_buf.is_released());
    CHECK(!c2->send_buf.is_released());
    loop.free_conn(*c2);
}

// === Callback type guard: close on unexpected event type ===

// on_header_received expects Recv — Send event should close
TEST(type_guard, header_recv_rejects_send) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    // Dispatch a Send event while expecting Recv → close
    loop.dispatch(make_ev(cid, IoEventType::Send, 100));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// on_response_sent expects Send — Recv event should close
TEST(type_guard, response_sent_rejects_recv) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);
    // Dispatch a Recv event while expecting Send → ignored (pipelined data).
    loop.dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK(loop.conns[cid].fd >= 0);  // not closed — bytes buffered for pipeline
}

// on_upstream_connected expects UpstreamConnect — Recv should close
TEST(type_guard, upstream_connected_rejects_recv) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    // Dispatch Recv instead of UpstreamConnect → close
    loop.dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// on_upstream_request_sent expects Send — Recv should close
TEST(type_guard, upstream_request_sent_rejects_recv) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    conn->on_complete = &on_upstream_request_sent<SmallLoop>;
    loop.dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// on_upstream_response expects UpstreamRecv — Send should close
TEST(type_guard, upstream_response_rejects_send) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    conn->on_complete = &on_upstream_response<SmallLoop>;
    loop.dispatch(make_ev(cid, IoEventType::Send, 50));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// on_proxy_response_sent expects Send — Recv should close
TEST(type_guard, proxy_response_sent_rejects_recv) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;
    conn->on_complete = &on_proxy_response_sent<SmallLoop>;
    loop.dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Timer tick overflow clamp ===

// Verify dispatch clamps large tick counts to TimerWheel::kSlots
TEST(timer_clamp, large_tick_count_clamped) {
    SmallLoop loop;
    loop.setup();
    loop.keepalive_timeout = 0;  // expire on first tick

    // Accept a connection
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Dispatch a Timeout with a huge tick count — should be clamped to kSlots,
    // not overflow i32 or loop billions of times
    loop.dispatch(make_ev(0, IoEventType::Timeout, 0x7FFFFFFF));

    // Connection should be expired (clamped 64 ticks > timeout 0)
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// Verify dispatch handles tick count of 0 gracefully (coerces to 1)
TEST(timer_clamp, zero_tick_coerced_to_one) {
    SmallLoop loop;
    loop.setup();
    loop.keepalive_timeout = 1;
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Dispatch Timeout with result=0 — should still advance one tick
    loop.dispatch(make_ev(0, IoEventType::Timeout, 0));
    // After 1 tick, keepalive_timeout=1 conn is in slot but not yet expired
    CHECK_EQ(loop.conns[cid].fd, 42);
    // Second tick expires it
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Copilot round 4 regression tests ===

// io_uring init returns -errno (not -1) on failure.
// We can't easily make mmap fail, but verify the convention:
// successful MockBackend init returns success.
TEST(copilot4, init_returns_success) {
    SmallLoop loop;
    loop.setup();
    CHECK(loop.backend.init(0, -1).has_value());
}

// Arena init returns Expected on failure.
// Verify success returns has_value().
TEST(copilot4, arena_init_returns_success) {
    Arena a;
    CHECK(a.init(4096).has_value());
    a.destroy();
}

// Arena init with absurdly large size should fail gracefully.
// mmap of near-max u64 will fail → should return error.
TEST(copilot4, arena_init_huge_fails) {
    Arena a;
    auto rc = a.init(static_cast<u64>(-1));  // ~18 exabytes
    CHECK(!rc);
    // Should carry a real errno code
    CHECK(rc.error().code > 0);
}

// Arena alloc overflow protection
TEST(copilot4, arena_alloc_overflow_returns_null) {
    Arena a;
    REQUIRE(a.init(4096).has_value());
    // size close to u64 max → overflow in (size+7) alignment
    void* p = a.alloc(static_cast<u64>(-1));
    CHECK(p == nullptr);
    a.destroy();
}

// === Partial send (TODO #1) ===

// Verify send_buf uses Buffer API correctly
TEST(send_buf, write_and_data) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // After recv, callback writes HTTP response to send_buf via Buffer::write()
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_GT(c->send_buf.len(), 0u);
    CHECK(c->send_buf.valid());

    // Verify the response content is readable via data()
    const u8* data = c->send_buf.data();
    REQUIRE(data != nullptr);
    // First bytes should be "HTTP/1.1 200 OK"
    CHECK_EQ(data[0], 'H');
    CHECK_EQ(data[1], 'T');
    CHECK_EQ(data[2], 'T');
    CHECK_EQ(data[3], 'P');
}

// Verify send_buf is properly reset between keepalive cycles
TEST(send_buf, reset_between_cycles) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // First cycle
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    u32 first_len = c->send_buf.len();
    CHECK_GT(first_len, 0u);

    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(first_len)));
    CHECK_EQ(c->state, ConnState::ReadingHeader);

    // Second cycle — send_buf should have fresh response, same length
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 80));
    CHECK_EQ(c->send_buf.len(), first_len);  // same 200 OK response
}

// Verify send_buf survives connection reuse
TEST(send_buf, survives_reuse) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_GT(c->send_buf.len(), 0u);

    // Close connection
    loop.close_conn(*c);
    CHECK_EQ(loop.conns[cid].fd, -1);

    // Reuse
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 55));
    auto* c2 = loop.find_fd(55);
    REQUIRE(c2 != nullptr);
    CHECK_EQ(c2->send_buf.len(), 0u);
    CHECK(c2->send_buf.valid());
    CHECK_EQ(c2->send_buf.write_avail(), SmallLoop::kBufSize);
}

// EpollBackend::add_send immediate success → synthetic completion with byte count
TEST(partial_send, immediate_full_send) {
    // Test through the mock: verify callback flow on immediate send completion.
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // Recv triggers response
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->on_complete, &on_response_sent<SmallLoop>);

    // Mock send completion with full byte count
    loop.backend.clear_ops();
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    // Should go back to reading (keep-alive)
    CHECK_EQ(c->state, ConnState::ReadingHeader);
    CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);
}

// Verify that send_buf.data() pointer passed to backend matches buffer content
TEST(partial_send, backend_gets_correct_buffer) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));

    auto* op = loop.backend.last_op(MockOp::Send);
    REQUIRE(op != nullptr);
    // The send buffer pointer should match send_buf.data()
    CHECK_EQ(op->send_buf, c->send_buf.data());
    CHECK_EQ(op->send_len, c->send_buf.len());
}

// === io_uring integration (TODO #3 + #4) ===

// Verify Timeout event with tick count is properly dispatched
TEST(uring_timer, timeout_dispatches_tick) {
    SmallLoop loop;
    loop.setup();
    loop.keepalive_timeout = 2;
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Two ticks should expire the connection (keepalive_timeout=2)
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, 42);  // still alive after 1 tick
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, 42);  // still alive (timer wheel schedules +2 slots)
    loop.dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(loop.conns[cid].fd, -1);  // expired after 3 ticks (slot 0→add at slot 2→fire)
}

// Verify that has_buf=0 events (post-buffer-return) work correctly in dispatch
TEST(uring_buf, recv_without_has_buf) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // io_uring wait() now copies data and clears has_buf before dispatch.
    // Simulate this: event with has_buf=0, data already in recv_buf.
    c->recv_buf.reset();
    const u8 data[] = "GET / HTTP/1.1\r\n\r\n";
    c->recv_buf.write(data, sizeof(data) - 1);

    // Dispatch recv event — callback should see recv_buf data
    IoEvent ev = make_ev(c->id, IoEventType::Recv, static_cast<i32>(sizeof(data) - 1));
    loop.dispatch(ev);
    CHECK_EQ(c->state, ConnState::Sending);
    CHECK_GT(c->send_buf.len(), 0u);
}

// Verify that provided buffer events with has_buf=1 still include buf_id
// (This tests the IoEvent structure itself)
TEST(uring_buf, event_preserves_buf_id) {
    IoEvent ev = {5, 100, 42, 1, IoEventType::Recv};
    CHECK_EQ(ev.conn_id, 5u);
    CHECK_EQ(ev.result, 100);
    CHECK_EQ(ev.buf_id, 42u);
    CHECK_EQ(ev.has_buf, 1u);

    // After io_uring wait() processes it: has_buf=0, buf_id=0 (buffer returned)
    IoEvent processed = {5, 100, 0, 0, IoEventType::Recv};
    CHECK_EQ(processed.has_buf, 0u);
    CHECK_EQ(processed.buf_id, 0u);
}

// Verify recv_buf is consumed between cycles (callback resets).
// Backend appends, callback consumes — no stale data leaks across cycles.
TEST(uring_buf, recv_buf_clean_between_cycles) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // First recv → preserved until send completes
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 200));
    CHECK_EQ(c->recv_buf.len(), 200u);

    // Send response → on_response_sent resets recv_buf
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(c->recv_buf.len(), 0u);  // reset by on_response_sent

    // Second recv → appended to clean buffer, preserved
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(c->recv_buf.len(), 50u);
    CHECK_EQ(c->state, ConnState::Sending);
}

// === Copilot round 5 regression tests ===

// Regression: epoll add_recv MOD+ADD fallback.
// 10 echo cycles on same connection — each cycle goes recv→send→recv.
// Without MOD fallback, 2nd recv would fail because fd is still registered.
TEST(copilot5, mock_10_keepalive_cycles) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    for (int i = 0; i < 10; i++) {
        loop.backend.clear_ops();
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
        CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
        CHECK_EQ(loop.backend.count_ops(MockOp::Send), 1u);

        loop.backend.clear_ops();
        loop.inject_and_dispatch(
            make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
        CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
        CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
    }
    CHECK_EQ(conn->fd, 42);  // still alive
}

// Regression: partial send TODO exists (code documents the limitation).
// Verify that add_send with immediate success queues a synthetic completion.
TEST(copilot5, add_send_immediate_success) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 50));
    // After recv, callback sets on_complete=on_response_sent and calls submit_send.
    // MockBackend should have a Send op.
    auto* op = loop.backend.last_op(MockOp::Send);
    CHECK(op != nullptr);
    CHECK_GT(op->send_len, 0u);
}

// Regression: keepalive_timeout must be 60 after init(), not 0.
// Without explicit assignment in init(), mmap-zeroed EventLoop gets 0.
TEST(copilot6, keepalive_timeout_is_60) {
    SmallLoop loop;
    loop.setup();
    CHECK_EQ(loop.keepalive_timeout, 60u);
}

// === RouteTable ===

TEST(route, match_prefix) {
    RouteConfig cfg;
    auto up = cfg.add_upstream("backend", 0x7F000001, 8080);
    REQUIRE(up.has_value());
    cfg.add_proxy("/api/", 0, static_cast<u16>(up.value()));

    const u8 path1[] = "/api/users";
    auto* r = cfg.match(path1, sizeof(path1) - 1, 0);
    REQUIRE(r != nullptr);
    CHECK_EQ(r->action, RouteAction::Proxy);
    CHECK_EQ(r->upstream_id, static_cast<u16>(up.value()));

    const u8 path2[] = "/health";
    CHECK(cfg.match(path2, sizeof(path2) - 1, 0) == nullptr);
}

TEST(route, method_filter) {
    RouteConfig cfg;
    cfg.add_static("/status", 'G', 200);

    const u8 path[] = "/status";
    CHECK(cfg.match(path, sizeof(path) - 1, 'G') != nullptr);
    CHECK(cfg.match(path, sizeof(path) - 1, 'P') == nullptr);
}

TEST(route, first_match_wins) {
    RouteConfig cfg;
    auto up1 = cfg.add_upstream("v1", 0x7F000001, 8081);
    auto up2 = cfg.add_upstream("v2", 0x7F000001, 8082);
    cfg.add_proxy("/api/v1/", 0, static_cast<u16>(up1.value()));
    cfg.add_proxy("/api/", 0, static_cast<u16>(up2.value()));

    const u8 path[] = "/api/v1/users";
    auto* r = cfg.match(path, sizeof(path) - 1, 0);
    REQUIRE(r != nullptr);
    CHECK_EQ(r->upstream_id, static_cast<u16>(up1.value()));
}

TEST(route, static_response) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);

    const u8 path[] = "/health";
    auto* r = cfg.match(path, sizeof(path) - 1, 0);
    REQUIRE(r != nullptr);
    CHECK_EQ(r->action, RouteAction::Static);
    CHECK_EQ(r->status_code, 200u);
}

TEST(route, empty_config_no_match) {
    RouteConfig cfg;
    const u8 path[] = "/anything";
    CHECK(cfg.match(path, sizeof(path) - 1, 0) == nullptr);
}

TEST(route, upstream_target_addr) {
    RouteConfig cfg;
    auto idx = cfg.add_upstream("api", 0x0A000101, 9090);
    REQUIRE(idx.has_value());
    auto& t = cfg.upstreams[idx.value()];
    CHECK_EQ(t.addr.sin_family, AF_INET);
    CHECK_EQ(__builtin_bswap16(t.addr.sin_port), 9090u);
    CHECK_EQ(__builtin_bswap32(t.addr.sin_addr.s_addr), 0x0A000101u);
    CHECK_EQ(t.name[0], 'a');
}

// === UpstreamPool ===

TEST(upstream_pool, alloc_free) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    CHECK_EQ(c->fd, -1);
    CHECK(!c->idle);
    pool.free(c);
}

TEST(upstream_pool, find_idle) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    c->fd = 42;
    c->upstream_id = 5;

    CHECK(pool.find_idle(5) == nullptr);
    pool.return_idle(c);
    CHECK(c->idle);

    auto* found = pool.find_idle(5);
    CHECK_EQ(found, c);
    CHECK(!found->idle);

    pool.return_idle(c);
    CHECK(pool.find_idle(99) == nullptr);

    c->fd = -1;
    pool.free(c);
}

TEST(upstream_pool, create_socket) {
    i32 fd = UpstreamPool::create_socket();
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

TEST(upstream_pool, shutdown_closes_fds) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    c->fd = UpstreamPool::create_socket();
    REQUIRE(c->fd >= 0);
    i32 saved_fd = c->fd;
    pool.shutdown();
    CHECK(close(saved_fd) < 0);
}

// === SlicePool ===

TEST(slice_pool, init_destroy) {
    SlicePool pool;
    REQUIRE(pool.init(64).has_value());
    // Lazy commit: count starts at 0, grows on first alloc.
    CHECK_EQ(pool.max_count, 64u);
    CHECK_EQ(pool.count, 0u);
    CHECK_EQ(pool.available(), 0u);
    pool.destroy();
}

TEST(slice_pool, alloc_free) {
    SlicePool pool;
    REQUIRE(pool.init(4).has_value());

    u8* s1 = pool.alloc();
    u8* s2 = pool.alloc();
    REQUIRE(s1 != nullptr);
    REQUIRE(s2 != nullptr);
    CHECK_NE(s1, s2);
    CHECK_EQ(pool.available(), 2u);
    CHECK_EQ(pool.in_use(), 2u);

    // Write to slices — verify no overlap (16KB apart)
    s1[0] = 'A';
    s1[SlicePool::kSliceSize - 1] = 'Z';
    s2[0] = 'B';
    CHECK_EQ(s1[0], 'A');
    CHECK_EQ(s1[SlicePool::kSliceSize - 1], 'Z');
    CHECK_EQ(s2[0], 'B');

    pool.free(s1);
    CHECK_EQ(pool.available(), 3u);
    pool.free(s2);
    CHECK_EQ(pool.available(), 4u);
    pool.destroy();
}

TEST(slice_pool, exhaust_and_recover) {
    SlicePool pool;
    REQUIRE(pool.init(2).has_value());

    u8* s1 = pool.alloc();
    u8* s2 = pool.alloc();
    REQUIRE(s1 != nullptr);
    REQUIRE(s2 != nullptr);
    CHECK(pool.alloc() == nullptr);  // exhausted
    CHECK_EQ(pool.available(), 0u);

    pool.free(s1);
    u8* s3 = pool.alloc();
    CHECK(s3 != nullptr);  // recovered
    CHECK_EQ(s3, s1);      // reuses same slice

    pool.free(s2);
    pool.free(s3);
    pool.destroy();
}

TEST(slice_pool, slice_size) {
    SlicePool pool;
    REQUIRE(pool.init(1).has_value());
    CHECK_EQ(SlicePool::kSliceSize, 16384u);
    u8* s = pool.alloc();
    REQUIRE(s != nullptr);
    // Verify we can write to the full 16KB without crash
    for (u32 i = 0; i < SlicePool::kSliceSize; i++) s[i] = static_cast<u8>(i & 0xFF);
    CHECK_EQ(s[0], 0u);
    CHECK_EQ(s[255], 255u);
    CHECK_EQ(s[16383], static_cast<u8>(16383 & 0xFF));
    pool.free(s);
    pool.destroy();
}

TEST(slice_pool, free_null_safe) {
    SlicePool pool;
    REQUIRE(pool.init(2).has_value());
    pool.free(nullptr);              // should not crash
    CHECK_EQ(pool.available(), 0u);  // lazy: no slices committed yet
    pool.destroy();
}

// SlicePool: out-of-order free
TEST(slice_pool, out_of_order_free) {
    SlicePool pool;
    REQUIRE(pool.init(4).has_value());
    u8* s1 = pool.alloc();
    u8* s2 = pool.alloc();
    u8* s3 = pool.alloc();
    REQUIRE(s1 && s2 && s3);

    // Free in non-LIFO order
    pool.free(s2);
    pool.free(s1);
    pool.free(s3);
    CHECK_EQ(pool.available(), 4u);

    // Re-alloc should still work
    u8* r1 = pool.alloc();
    u8* r2 = pool.alloc();
    CHECK(r1 != nullptr);
    CHECK(r2 != nullptr);
    pool.free(r1);
    pool.free(r2);
    pool.destroy();
}

// SlicePool: multiple alloc-free cycles don't corrupt free-stack
TEST(slice_pool, stress_cycles) {
    SlicePool pool;
    REQUIRE(pool.init(8).has_value());
    for (int cycle = 0; cycle < 100; cycle++) {
        u8* ptrs[8];
        for (int j = 0; j < 8; j++) {
            ptrs[j] = pool.alloc();
            REQUIRE(ptrs[j] != nullptr);
        }
        CHECK(pool.alloc() == nullptr);  // exhausted each cycle
        for (int j = 7; j >= 0; j--) pool.free(ptrs[j]);
        CHECK_EQ(pool.available(), 8u);
    }
    pool.destroy();
}

// SlicePool: destroy is idempotent
TEST(slice_pool, destroy_idempotent) {
    SlicePool pool;
    REQUIRE(pool.init(4).has_value());
    pool.destroy();
    pool.destroy();  // second destroy should not crash
    CHECK(pool.base == nullptr);
    CHECK(pool.free_stack == nullptr);
}

// SlicePool: alloc after destroy returns nullptr
TEST(slice_pool, alloc_after_destroy) {
    SlicePool pool;
    REQUIRE(pool.init(4).has_value());
    pool.destroy();
    CHECK(pool.alloc() == nullptr);
}

// SlicePool: large pool (verify mmap works at scale)
TEST(slice_pool, large_pool) {
    SlicePool pool;
    REQUIRE(pool.init(1024).has_value());  // max 1024 slices, lazy commit
    CHECK_EQ(pool.max_count, 1024u);

    // Alloc a few, verify they don't overlap
    u8* first = pool.alloc();
    u8* last = pool.alloc();
    REQUIRE(first != nullptr);
    REQUIRE(last != nullptr);
    // Must be at least 16KB apart
    u64 dist = (first > last) ? static_cast<u64>(first - last) : static_cast<u64>(last - first);
    CHECK(dist >= SlicePool::kSliceSize);

    pool.free(first);
    pool.free(last);
    pool.destroy();
}

// === SlabPool ===

struct TestObj {
    i32 value;
    u8 data[60];  // pad to 64 bytes
};

TEST(slab_pool, init_destroy) {
    SlabPool<TestObj, 128> pool;
    REQUIRE(pool.init().has_value());
    CHECK_EQ(pool.capacity(), 128u);
    CHECK_EQ(pool.available(), 128u);
    CHECK_EQ(pool.in_use(), 0u);
    pool.destroy();
}

TEST(slab_pool, alloc_free_by_ptr) {
    SlabPool<TestObj, 4> pool;
    REQUIRE(pool.init().has_value());

    TestObj* a = pool.alloc();
    TestObj* b = pool.alloc();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK_NE(a, b);
    CHECK_EQ(pool.in_use(), 2u);

    a->value = 42;
    b->value = 99;
    CHECK_EQ(a->value, 42);
    CHECK_EQ(b->value, 99);

    pool.free(a);
    pool.free(b);
    CHECK_EQ(pool.available(), 4u);
    pool.destroy();
}

TEST(slab_pool, alloc_with_id) {
    SlabPool<TestObj, 8> pool;
    REQUIRE(pool.init().has_value());

    u32 idx = 0;
    TestObj* obj = pool.alloc_with_id(idx);
    REQUIRE(obj != nullptr);
    obj->value = 7;
    CHECK_EQ(pool[idx].value, 7);
    CHECK_EQ(pool.index_of(obj), idx);

    pool.free(idx);
    CHECK_EQ(pool.available(), 8u);
    pool.destroy();
}

TEST(slab_pool, exhaust) {
    SlabPool<TestObj, 2> pool;
    REQUIRE(pool.init().has_value());

    TestObj* a = pool.alloc();
    TestObj* b = pool.alloc();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(pool.alloc() == nullptr);

    pool.free(a);
    TestObj* c = pool.alloc();
    CHECK_EQ(c, a);

    pool.free(b);
    pool.free(c);
    pool.destroy();
}

TEST(slab_pool, index_of) {
    SlabPool<TestObj, 16> pool;
    REQUIRE(pool.init().has_value());

    TestObj* a = pool.alloc();
    TestObj* b = pool.alloc();
    u32 ia = pool.index_of(a);
    u32 ib = pool.index_of(b);
    CHECK_NE(ia, ib);
    CHECK_EQ(&pool[ia], a);
    CHECK_EQ(&pool[ib], b);

    pool.free(a);
    pool.free(b);
    pool.destroy();
}

// SlabPool: capacity 1
TEST(slab_pool, capacity_one) {
    SlabPool<TestObj, 1> pool;
    REQUIRE(pool.init().has_value());
    CHECK_EQ(pool.capacity(), 1u);

    TestObj* a = pool.alloc();
    REQUIRE(a != nullptr);
    CHECK(pool.alloc() == nullptr);  // full

    a->value = 77;
    CHECK_EQ(pool[0].value, 77);

    pool.free(a);
    TestObj* b = pool.alloc();
    CHECK_EQ(b, a);  // reused

    pool.free(b);
    pool.destroy();
}

// SlabPool: free by index vs free by pointer consistency
TEST(slab_pool, free_by_index_vs_ptr) {
    SlabPool<TestObj, 4> pool;
    REQUIRE(pool.init().has_value());

    u32 idx1 = 0;
    TestObj* a = pool.alloc_with_id(idx1);
    u32 idx2 = 0;
    TestObj* b = pool.alloc_with_id(idx2);
    REQUIRE(a && b);

    // Free a by index, b by pointer
    pool.free(idx1);
    pool.free(b);
    CHECK_EQ(pool.available(), 4u);

    // Both slots reusable
    TestObj* c = pool.alloc();
    TestObj* d = pool.alloc();
    CHECK(c != nullptr);
    CHECK(d != nullptr);

    pool.free(c);
    pool.free(d);
    pool.destroy();
}

// SlabPool: alloc_with_id when empty returns nullptr
TEST(slab_pool, alloc_with_id_empty) {
    SlabPool<TestObj, 1> pool;
    REQUIRE(pool.init().has_value());

    u32 idx = 999;
    pool.alloc();  // take the only slot

    TestObj* obj = pool.alloc_with_id(idx);
    CHECK(obj == nullptr);
    CHECK_EQ(idx, 0u);  // idx set to 0 on failure

    // Cleanup
    pool.free(static_cast<u32>(0));
    pool.destroy();
}

// SlabPool: destroy idempotent
TEST(slab_pool, destroy_idempotent) {
    SlabPool<TestObj, 4> pool;
    REQUIRE(pool.init().has_value());
    pool.destroy();
    pool.destroy();  // second destroy should not crash
    CHECK(pool.objects == nullptr);
    CHECK(pool.free_stack == nullptr);
}

// SlabPool: stress alloc-free cycles
TEST(slab_pool, stress_cycles) {
    SlabPool<TestObj, 16> pool;
    REQUIRE(pool.init().has_value());
    for (int cycle = 0; cycle < 50; cycle++) {
        TestObj* ptrs[16];
        for (int j = 0; j < 16; j++) {
            ptrs[j] = pool.alloc();
            REQUIRE(ptrs[j] != nullptr);
            ptrs[j]->value = cycle * 16 + j;
        }
        CHECK(pool.alloc() == nullptr);
        // Verify values
        for (int j = 0; j < 16; j++) CHECK_EQ(ptrs[j]->value, cycle * 16 + j);
        // Free all
        for (int j = 0; j < 16; j++) pool.free(ptrs[j]);
        CHECK_EQ(pool.available(), 16u);
    }
    pool.destroy();
}

// SlabPool: different object type (small)
struct SmallObj {
    u8 tag;
};

TEST(slab_pool, small_object) {
    SlabPool<SmallObj, 32> pool;
    REQUIRE(pool.init().has_value());
    SmallObj* a = pool.alloc();
    REQUIRE(a != nullptr);
    a->tag = 0xAB;
    CHECK_EQ(&pool[pool.index_of(a)], a);  // index_of round-trips to same pointer
    CHECK_EQ(a->tag, 0xABu);
    pool.free(a);
    pool.destroy();
}

// === Double-free detection ===

TEST(slice_pool, double_free_rejected) {
    SlicePool pool;
    REQUIRE(pool.init(4));
    u8* s = pool.alloc();
    REQUIRE(s != nullptr);
    CHECK_EQ(pool.available(), 3u);
    pool.free(s);
    CHECK_EQ(pool.available(), 4u);
    pool.free(s);                    // double-free: should be silently rejected
    CHECK_EQ(pool.available(), 4u);  // unchanged
    pool.destroy();
}

TEST(slab_pool, double_free_by_ptr_rejected) {
    SlabPool<TestObj, 4> pool;
    REQUIRE(pool.init());
    TestObj* a = pool.alloc();
    REQUIRE(a != nullptr);
    CHECK_EQ(pool.in_use(), 1u);
    pool.free(a);
    CHECK_EQ(pool.in_use(), 0u);
    pool.free(a);                 // double-free
    CHECK_EQ(pool.in_use(), 0u);  // unchanged
    pool.destroy();
}

TEST(slab_pool, double_free_by_idx_rejected) {
    SlabPool<TestObj, 4> pool;
    REQUIRE(pool.init());
    u32 idx = 0;
    pool.alloc_with_id(idx);
    pool.free(idx);
    CHECK_EQ(pool.available(), 4u);
    pool.free(idx);                  // double-free
    CHECK_EQ(pool.available(), 4u);  // unchanged
    pool.destroy();
}

TEST(upstream_pool, double_free_rejected) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    u32 before = pool.free_top;
    c->fd = -1;  // no real fd to close
    pool.free(c);
    CHECK_EQ(pool.free_top, before + 1);
    pool.free(c);                         // double-free: allocated=false now
    CHECK_EQ(pool.free_top, before + 1);  // unchanged
}

// === UpstreamPool validation ===

TEST(upstream_pool, return_idle_null_safe) {
    UpstreamPool pool;
    pool.init();
    pool.return_idle(nullptr);  // should not crash
}

TEST(upstream_pool, return_idle_requires_allocated) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    c->fd = -1;
    pool.free(c);
    // c is now free — return_idle should reject
    pool.return_idle(c);
    CHECK(!c->idle);  // not marked idle (allocated=false)
}

TEST(upstream_pool, shutdown_is_idempotent) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    c->fd = UpstreamPool::create_socket();
    pool.shutdown();
    CHECK_EQ(pool.free_top, UpstreamPool::kMaxConns);
    pool.shutdown();  // second shutdown
    CHECK_EQ(pool.free_top, UpstreamPool::kMaxConns);
}

TEST(upstream_pool, alloc_resets_upstream_id) {
    UpstreamPool pool;
    pool.init();
    auto* c = pool.alloc();
    REQUIRE(c != nullptr);
    c->upstream_id = 42;
    c->fd = -1;
    pool.free(c);
    auto* c2 = pool.alloc();
    CHECK_EQ(c2->upstream_id, 0u);  // reset on alloc
    c2->fd = -1;
    pool.free(c2);
}

// === RouteTable validation ===

TEST(route, add_proxy_invalid_upstream_id) {
    RouteConfig cfg;
    // No upstreams added — upstream_id 0 is invalid
    CHECK(!cfg.add_proxy("/api/", 0, 0));
    CHECK_EQ(cfg.route_count, 0u);
}

TEST(route, add_proxy_path_too_long) {
    RouteConfig cfg;
    (void)cfg.add_upstream("x", 0x7F000001, 80);
    char long_path[256];
    for (u32 i = 0; i < 255; i++) long_path[i] = 'a';
    long_path[255] = '\0';
    CHECK(!cfg.add_proxy(long_path, 0, 0));  // exceeds 128-char RouteEntry::path
    CHECK_EQ(cfg.route_count, 0u);
}

TEST(route, add_static_path_too_long) {
    RouteConfig cfg;
    char long_path[256];
    for (u32 i = 0; i < 255; i++) long_path[i] = 'b';
    long_path[255] = '\0';
    CHECK(!cfg.add_static(long_path, 0, 200));
    CHECK_EQ(cfg.route_count, 0u);
}

TEST(route, add_upstream_at_capacity) {
    RouteConfig cfg;
    for (u32 i = 0; i < RouteConfig::kMaxUpstreams; i++) {
        CHECK(cfg.add_upstream("x", 0x7F000001, static_cast<u16>(8080 + i)).has_value());
    }
    CHECK(!cfg.add_upstream("overflow", 0x7F000001, 9999).has_value());  // full
}

TEST(route, add_route_at_capacity) {
    RouteConfig cfg;
    (void)cfg.add_upstream("x", 0x7F000001, 80);
    for (u32 i = 0; i < RouteConfig::kMaxRoutes; i++) {
        char path[8];
        path[0] = '/';
        path[1] = static_cast<char>('0' + (i / 100) % 10);
        path[2] = static_cast<char>('0' + (i / 10) % 10);
        path[3] = static_cast<char>('0' + i % 10);
        path[4] = '\0';
        CHECK(cfg.add_proxy(path, 0, 0));
    }
    CHECK(!cfg.add_proxy("/overflow", 0, 0));  // full
}

// === Error source ===

TEST(error, from_errno_captures_source) {
    errno = ENOMEM;
    Error e = Error::from_errno(Error::Source::Mmap);
    CHECK_EQ(e.code, ENOMEM);
    CHECK_EQ(static_cast<u8>(e.source), static_cast<u8>(Error::Source::Mmap));
}

TEST(error, make_with_code) {
    Error e = Error::make(EINVAL, Error::Source::Socket);
    CHECK_EQ(e.code, EINVAL);
    CHECK_EQ(static_cast<u8>(e.source), static_cast<u8>(Error::Source::Socket));
}

// === SlicePool integration ===

TEST(slice_conn, alloc_binds_slices) {
    SmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    CHECK(c->recv_slice != nullptr);
    CHECK(c->send_slice != nullptr);
    CHECK(c->recv_buf.valid());
    CHECK(c->send_buf.valid());
    CHECK_EQ(c->recv_buf.write_avail(), SmallLoop::kBufSize);
    CHECK_EQ(c->send_buf.write_avail(), SmallLoop::kBufSize);
    loop.free_conn(*c);
}

TEST(slice_conn, free_clears_slices) {
    SmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.free_conn(*c);
    // Sync backend: slices freed immediately, pointers cleared by reset.
    CHECK_EQ(loop.conns[cid].recv_slice, nullptr);
    CHECK_EQ(loop.conns[cid].send_slice, nullptr);
    CHECK_EQ(loop.conns[cid].recv_buf.write_avail(), 0u);
    CHECK_EQ(loop.conns[cid].send_buf.write_avail(), 0u);
}

TEST(slice_conn, slice_reuse_after_free) {
    SmallLoop loop;
    loop.setup();
    Connection* c1 = loop.alloc_conn();
    REQUIRE(c1 != nullptr);
    u8* r1 = c1->recv_slice;
    u8* s1 = c1->send_slice;
    // Write data then free
    c1->recv_buf.write(reinterpret_cast<const u8*>("hello"), 5);
    loop.free_conn(*c1);

    // Re-alloc should get same slot back (LIFO) with inline storage
    Connection* c2 = loop.alloc_conn();
    REQUIRE(c2 != nullptr);
    // SmallLoop uses per-slot inline arrays; same slot ⇒ same storage.
    CHECK(c2->recv_slice == r1);
    CHECK(c2->send_slice == s1);
    // Buffer should be fresh (reset by bind)
    CHECK_EQ(c2->recv_buf.len(), 0u);
    loop.free_conn(*c2);
}

TEST(slice_conn, pool_exhaustion_returns_null) {
    SmallLoop loop;
    loop.setup();
    Connection* conns[SmallLoop::kMaxConns];
    u32 alloc_count = 0;
    // Allocate all connections
    for (u32 i = 0; i < SmallLoop::kMaxConns; i++) {
        conns[i] = loop.alloc_conn();
        if (conns[i]) alloc_count++;
    }
    CHECK_EQ(alloc_count, SmallLoop::kMaxConns);
    // Next alloc should fail
    CHECK(loop.alloc_conn() == nullptr);
    // Free one and retry
    loop.free_conn(*conns[0]);
    Connection* c = loop.alloc_conn();
    CHECK(c != nullptr);
    CHECK(c->recv_buf.valid());
    // Cleanup
    loop.free_conn(*c);
    for (u32 i = 1; i < alloc_count; i++) loop.free_conn(*conns[i]);
}

TEST(slice_conn, buffers_usable_through_request_cycle) {
    SmallLoop loop;
    loop.setup();
    // Accept → recv → build response → send → free
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    CHECK(c->recv_buf.valid());
    CHECK(c->send_buf.valid());

    // Recv fills recv_buf
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    CHECK(c->recv_buf.len() > 0);
    CHECK(c->send_buf.len() > 0);  // response built by on_header_received

    // Send completes
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));
    // Keep-alive: buffers reset for next request
    CHECK_EQ(c->recv_buf.len(), 0u);
    CHECK(c->recv_buf.valid());
}

TEST(slice_conn, real_eventloop_pool_init) {
    // Verify real EventLoop allocates pool with 2*kMaxConns slices.
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    auto rc = loop->init(0, lfd);
    REQUIRE(rc.has_value());

    // Lazy commit: pool starts empty, max set to 2 * kMaxConns.
    CHECK_EQ(loop->pool.max_count, RealLoop::kMaxConns * 2);
    CHECK_EQ(loop->pool.count, 0u);

    // Alloc a connection — triggers lazy grow, consumes 2 slices
    Connection* c = loop->alloc_conn();
    REQUIRE(c != nullptr);
    CHECK_GT(loop->pool.count, 0u);  // grew from 0
    CHECK_EQ(c->recv_buf.write_avail(), SlicePool::kSliceSize);
    CHECK_EQ(c->send_buf.write_avail(), SlicePool::kSliceSize);

    // Sync backend (epoll): slices returned to pool immediately on free.
    u32 avail_before = loop->pool.available();
    loop->free_conn(*c);
    CHECK_EQ(loop->pool.available(), avail_before + 2);

    // Realloc works normally.
    Connection* c2 = loop->alloc_conn();
    REQUIRE(c2 != nullptr);

    loop->free_conn(*c2);
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

// === close_conn cancels in-flight I/O before freeing ===

// Closing a connection in Sending state must cancel I/O on its fd
// before the slot (and its pooled slices) become reusable.
TEST(close_cancel, cancels_client_fd) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Drive to Sending state (recv → response built → waiting for send completion).
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(c->state, ConnState::Sending);

    loop.backend.clear_ops();
    loop.close_conn(*c);

    // A Cancel op must have been recorded for the client fd before free.
    const MockOp* cancel_op = loop.backend.last_op(MockOp::Cancel);
    REQUIRE(cancel_op != nullptr);
    CHECK_EQ(cancel_op->conn_id, cid);
}

// Closing a proxying connection must cancel I/O on both client and upstream fds.
TEST(close_cancel, cancels_both_fds_when_proxying) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Simulate a proxying connection with both fds active.
    c->upstream_fd = 99;
    c->state = ConnState::Proxying;

    loop.backend.clear_ops();
    loop.close_conn(*c);

    // Should have two Cancel ops: one for client fd, one for upstream fd.
    CHECK_EQ(loop.backend.count_ops(MockOp::Cancel), 2u);
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK_EQ(loop.conns[cid].upstream_fd, -1);
}

// After close+cancel, the freed slot is fully reusable.
TEST(close_cancel, slot_reusable_after_cancel) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));

    loop.close_conn(*c);

    // Reuse the slot with a new accept.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 55));
    auto* c2 = loop.find_fd(55);
    REQUIRE(c2 != nullptr);
    CHECK_EQ(c2->recv_buf.len(), 0u);
    CHECK_EQ(c2->send_buf.len(), 0u);
}

// No cancel should be emitted for fds that are already -1 (idle connection).
TEST(close_cancel, no_cancel_for_idle_conn) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // Complete the full request-response cycle so fds get cleared normally.
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Now force-close a connection whose fd is already -1 (keep-alive idle
    // would not have fd=-1, but test the guard path).
    c->fd = -1;
    c->upstream_fd = -1;
    loop.backend.clear_ops();
    loop.close_conn(*c);
    CHECK_EQ(loop.backend.count_ops(MockOp::Cancel), 0u);
}

// === SlicePool prealloc ===

TEST(pool_prealloc, prealloc_commits_slices) {
    SlicePool pool;
    auto rc = pool.init(64, 32);
    REQUIRE(rc.has_value());
    // prealloc rounds up to kGrowStep (256), so count >= 32.
    CHECK_GE(pool.count, 32u);
    // All committed slices are on free stack (none allocated yet).
    CHECK_EQ(pool.available(), pool.count);
    pool.destroy();
}

TEST(pool_prealloc, prealloc_zero_is_lazy) {
    SlicePool pool;
    auto rc = pool.init(64, 0);
    REQUIRE(rc.has_value());
    CHECK_EQ(pool.count, 0u);
    CHECK_EQ(pool.available(), 0u);
    pool.destroy();
}

// === Async reclaim (io_uring-style deferred reclamation) ===

TEST(async_reclaim, pending_ops_tracks_submits) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    CHECK_EQ(c->pending_ops, 0u);
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    loop.submit_send(*c, c->send_buf.data(), 10);
    CHECK_EQ(c->pending_ops, 2u);
}

TEST(async_reclaim, pending_ops_decrements_on_dispatch) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    c->state = ConnState::ReadingHeader;
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    // Clear on_complete so dispatch only does CQE accounting, not callbacks.
    c->on_complete = nullptr;
    // Dispatch a recv CQE with more=0 (final).
    loop.dispatch(make_ev_more(c->id, IoEventType::Recv, 50, 0));
    CHECK_EQ(c->pending_ops, 0u);
}

TEST(async_reclaim, pending_ops_no_decrement_on_more) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    c->state = ConnState::ReadingHeader;
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    // Clear on_complete so dispatch only does CQE accounting.
    c->on_complete = nullptr;
    // Dispatch a recv CQE with more=1 (multishot intermediate).
    loop.dispatch(make_ev_more(c->id, IoEventType::Recv, 50, 1));
    CHECK_EQ(c->pending_ops, 1u);
    CHECK_EQ(c->recv_armed, true);
}

TEST(async_reclaim, reclaim_pending_skips_nonzero_ops) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    c->fd = 42;
    // Submit recv so pending_ops > 0.
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    u32 free_before = loop.free_top;
    // Close the connection: pending_ops>0 so it goes to pending_free.
    loop.close_conn(*c);
    CHECK_EQ(loop.pending_free_count, 1u);
    CHECK_EQ(loop.free_top, free_before);  // not reclaimed yet
    // Call reclaim_pending: pending_ops still > 0, should stay deferred.
    loop.reclaim_pending();
    CHECK_EQ(loop.pending_free_count, 1u);
    CHECK_EQ(loop.free_top, free_before);
    // Verify the slot is the one we closed.
    CHECK_EQ(loop.pending_free[0], cid);
}

TEST(async_reclaim, reclaim_pending_reclaims_zero_ops) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    c->fd = 42;
    // Submit recv so pending_ops > 0, then close.
    loop.submit_recv(*c);
    loop.close_conn(*c);
    CHECK_EQ(loop.pending_free_count, 1u);
    u32 free_before = loop.free_top;
    // Manually zero out pending_ops to simulate CQEs having drained.
    loop.conns[cid].pending_ops = 0;
    loop.reclaim_pending();
    CHECK_EQ(loop.pending_free_count, 0u);
    CHECK_EQ(loop.free_top, free_before + 1);
}

TEST(async_reclaim, inline_reclaim_on_stale_cqe) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    u32 cid = c->id;
    c->fd = 42;
    // Submit recv (pending_ops=1), then close.
    // close_conn adds cancel count to pending_ops (pending_ops=1+1=2),
    // then defers to pending_free since pending_ops > 0.
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    loop.close_conn(*c);
    CHECK_EQ(loop.pending_free_count, 1u);
    CHECK_GT(loop.conns[cid].pending_ops, 1u);  // recv + cancel CQEs

    u32 free_before = loop.free_top;
    u32 ops = loop.conns[cid].pending_ops;
    // Dispatch stale CQEs until pending_ops reaches 0.
    // Each CQE represents either the cancelled recv or the cancel op itself.
    for (u32 i = 0; i < ops; i++) {
        IoEvent stale = make_ev_more(cid, IoEventType::Recv, -125, 0);
        loop.dispatch(stale);
    }
    CHECK_EQ(loop.conns[cid].pending_ops, 0u);
    CHECK_EQ(loop.pending_free_count, 0u);
    CHECK_EQ(loop.free_top, free_before + 1);
}

TEST(async_reclaim, recv_armed_skips_duplicate_submit) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    loop.submit_recv(*c);
    CHECK_EQ(c->recv_armed, true);
    CHECK_EQ(c->pending_ops, 1u);
    u32 ops_before = loop.backend.count_ops(MockOp::Recv);
    // Second submit should be a no-op.
    loop.submit_recv(*c);
    CHECK_EQ(c->pending_ops, 1u);
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), ops_before);
}

TEST(async_reclaim, upstream_recv_armed_independent) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    c->upstream_fd = 43;
    c->state = ConnState::Proxying;
    // Submit both client recv and upstream recv.
    loop.submit_recv(*c);
    loop.submit_recv_upstream(*c);
    CHECK_EQ(c->recv_armed, true);
    CHECK_EQ(c->upstream_recv_armed, true);
    CHECK_EQ(c->pending_ops, 2u);
    // Clear on_complete so dispatch only does CQE accounting.
    c->on_complete = nullptr;
    // Dispatch final upstream recv CQE (more=0).
    IoEvent ev = make_ev_more(c->id, IoEventType::UpstreamRecv, 100, 0);
    loop.dispatch(ev);
    // upstream_recv_armed cleared, but recv_armed still set.
    CHECK_EQ(c->upstream_recv_armed, false);
    CHECK_EQ(c->recv_armed, true);
    CHECK_EQ(c->pending_ops, 1u);
}

TEST(async_reclaim, close_skips_cancel_when_no_ops) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    // pending_ops == 0: no in-flight I/O.
    CHECK_EQ(c->pending_ops, 0u);
    loop.backend.clear_ops();
    loop.close_conn(*c);
    CHECK_EQ(loop.backend.count_ops(MockOp::Cancel), 0u);
}

TEST(async_reclaim, close_cancels_when_ops_pending) {
    AsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    loop.submit_recv(*c);
    CHECK_GT(c->pending_ops, 0u);
    loop.backend.clear_ops();
    loop.close_conn(*c);
    CHECK_GT(loop.backend.count_ops(MockOp::Cancel), 0u);
}

TEST(async_reclaim, sqe_fail_no_pending_ops_increment) {
    FailRecvAsyncSmallLoop loop;
    loop.setup();
    Connection* c = loop.alloc_conn();
    REQUIRE(c != nullptr);
    c->fd = 42;
    CHECK_EQ(c->pending_ops, 0u);
    loop.submit_recv(*c);
    // add_recv returned false: pending_ops must not have incremented.
    CHECK_EQ(c->pending_ops, 0u);
    CHECK_EQ(c->recv_armed, false);
}

// === Body Streaming ===

// Helper: inject custom data into recv_buf and dispatch an event.
// Does NOT use inject_and_dispatch (which writes garbage bytes).
template <typename Loop>
static void inject_custom_recv(
    Loop& loop, Connection& conn, IoEventType type, const u8* data, u32 len) {
    // Route to upstream_recv_buf for UpstreamRecv, recv_buf for Recv.
    auto& buf = (type == IoEventType::UpstreamRecv) ? conn.upstream_recv_buf : conn.recv_buf;
    buf.reset();
    u8* dst = buf.write_ptr();
    u32 avail = buf.write_avail();
    u32 n = len < avail ? len : avail;
    for (u32 j = 0; j < n; j++) dst[j] = data[j];
    buf.commit(n);
    IoEvent ev = make_ev(conn.id, type, static_cast<i32>(n));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 ne = loop.backend.wait(events, 8);
    for (u32 i = 0; i < ne; i++) loop.dispatch(events[i]);
}

// Helper: set up a proxy connection and send request to upstream.
// Returns the connection pointer (upstream_fd = 100, client fd = 42).
static Connection* setup_proxy_conn(SmallLoop& loop) {
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    if (!conn) return nullptr;

    // Simulate receiving an HTTP request.
    // Write a valid GET request into recv_buf.
    const char* req = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    u32 req_len = 30;
    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Switch to proxy mode.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Request sent to upstream (send result = recv_buf.len()).
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->recv_buf.len())));

    // Now conn is waiting for upstream response (on_upstream_response).
    return conn;
}

// Helper: set up proxy with HEAD request method.
static Connection* setup_proxy_conn_head(SmallLoop& loop) {
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    if (!conn) return nullptr;

    // Write a HEAD request.
    const char* req = "HEAD / HTTP/1.1\r\nHost: test\r\n\r\n";
    u32 req_len = 31;
    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);

    // Dispatch the recv event (this parses the request and captures metadata).
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Switch to proxy mode.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->recv_buf.len())));

    return conn;
}

// Large Content-Length response body that requires multiple recv→send cycles.
// SmallLoop has 4KB buffers. A 10KB body needs 3 recv→send cycles.
TEST(streaming, large_content_length) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream response: headers + first body fragment.
    // Total body = 10000 bytes. Headers + some initial body fit in 4KB.
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10000\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    // Build initial upstream_recv_buf: headers + as much body as fits in 4KB.
    u32 initial_body = SmallLoop::kBufSize - hdr_len;
    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    for (u32 i = 0; i < initial_body; i++) dst[hdr_len + i] = static_cast<u8>(i & 0xFF);
    conn->upstream_recv_buf.commit(hdr_len + initial_body);

    // Dispatch: upstream response with headers + initial body.
    IoEvent ev =
        make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len + initial_body));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should be in streaming mode (not on_proxy_response_sent).
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);

    // Simulate send completion of the initial headers+body.
    loop.backend.clear_ops();
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len + initial_body)));
    // Should now be waiting for more upstream body data.
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Track remaining body.
    u32 body_sent = initial_body;
    u32 total_body = 10000;

    // Stream body in chunks until complete.
    while (body_sent < total_body) {
        u32 chunk = total_body - body_sent;
        if (chunk > SmallLoop::kBufSize) chunk = SmallLoop::kBufSize;

        // Inject upstream body data.
        u8 body_chunk[SmallLoop::kBufSize];
        for (u32 i = 0; i < chunk; i++) body_chunk[i] = static_cast<u8>(i & 0xFF);
        inject_custom_recv(loop, *conn, IoEventType::UpstreamRecv, body_chunk, chunk);

        // Should have forwarded to client.
        CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

        // Simulate send completion.
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(chunk)));
        body_sent += chunk;

        if (body_sent < total_body) {
            // More body to stream.
            CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);
        }
    }

    // Body complete — should be back to reading next request (keep-alive).
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
}

// Chunked response body streaming.
TEST(streaming, chunked_response) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream response with chunked transfer encoding (headers only, no body yet).
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    conn->upstream_recv_buf.commit(hdr_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should be streaming (headers sent, waiting for send completion).
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::Chunked);

    // Send completion of headers.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // First chunk: "A\r\n0123456789\r\n" (10 bytes of data).
    const char* chunk1 = "A\r\n0123456789\r\n";
    u32 chunk1_len = 0;
    while (chunk1[chunk1_len]) chunk1_len++;
    inject_custom_recv(
        loop, *conn, IoEventType::UpstreamRecv, reinterpret_cast<const u8*>(chunk1), chunk1_len);
    CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

    // Send completion.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(chunk1_len)));
    // Not done yet — recv more.
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Final chunk: "0\r\n\r\n".
    const char* chunk_end = "0\r\n\r\n";
    u32 end_len = 5;
    inject_custom_recv(
        loop, *conn, IoEventType::UpstreamRecv, reinterpret_cast<const u8*>(chunk_end), end_len);
    CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

    // Send completion of final chunk.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(end_len)));

    // Body complete — back to keep-alive.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
}

// 204 No Content — no body regardless of headers.
TEST(streaming, no_body_204) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 204 No Content\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // No body — should go directly to on_proxy_response_sent (single-buffer path).
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::None);
}

// HEAD response — no body despite Content-Length header.
TEST(streaming, head_no_body) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn_head(loop);
    REQUIRE(conn != nullptr);

    // Response has Content-Length but HEAD requests have no body.
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5000\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // HEAD response: no body, single-buffer path.
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::None);
}

// Small Content-Length body that fits entirely in initial recv — no streaming needed.
TEST(streaming, small_body_no_streaming) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Small body: headers + 2 bytes body fits easily.
    inject_upstream_response(loop, *conn);

    // Should use single-buffer path (on_proxy_response_sent).
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);
    CHECK_EQ(conn->resp_body_remaining, 0u);
}

// UntilClose body mode: read until upstream EOF.
TEST(streaming, until_close) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Response with no Content-Length and no chunked — UntilClose mode.
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    conn->upstream_recv_buf.commit(hdr_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // UntilClose: streaming mode since body end is unknown.
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_mode, BodyMode::UntilClose);

    // Send headers.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // First body chunk.
    u8 body[100];
    for (u32 i = 0; i < 100; i++) body[i] = static_cast<u8>(i);
    inject_custom_recv(loop, *conn, IoEventType::UpstreamRecv, body, 100);
    CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

    // Send completion.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // EOF from upstream — signals end of UntilClose body.
    // recv_buf was already reset by on_response_body_sent callback.
    IoEvent eof_ev = make_ev(conn->id, IoEventType::UpstreamRecv, 0);
    loop.backend.inject(eof_ev);
    IoEvent eof_events[8];
    u32 ne = loop.backend.wait(eof_events, 8);
    for (u32 i = 0; i < ne; i++) loop.dispatch(eof_events[i]);

    // UntilClose: client must be closed too (client uses EOF to detect
    // body end, so keep-alive is impossible).
    CHECK_EQ(conn->fd, -1);
}

// Upstream response with both Transfer-Encoding: chunked AND Content-Length.
// RFC 7230 §3.3.3: chunked takes precedence over Content-Length.
TEST(streaming, chunked_over_content_length) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 999\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Chunked must take precedence over Content-Length.
    CHECK_EQ(conn->resp_body_mode, BodyMode::Chunked);
    // The entire chunked body (5\r\nhello\r\n0\r\n\r\n) was in the initial recv,
    // so the body is complete — should go to single-buffer path.
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
}

// HEAD response with trailing bytes after headers — only headers forwarded.
TEST(streaming, no_body_head_strips_trailing_bytes) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn_head(loop);
    REQUIRE(conn != nullptr);

    // Response with Content-Length AND trailing garbage bytes after headers.
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "GARBAGE";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    // Compute header length (up to and including \r\n\r\n).
    u32 hdr_len = 0;
    for (u32 i = 0; i + 3 < resp_len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_len = i + 4;
            break;
        }
    }
    REQUIRE(hdr_len > 0);

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    loop.backend.clear_ops();
    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // HEAD request — body mode must be None.
    CHECK_EQ(conn->resp_body_mode, BodyMode::None);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // The send should contain only headers, not "GARBAGE".
    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->send_len, hdr_len);
}

// Content-Length: 5 but 10 body bytes received — only 5 forwarded.
TEST(streaming, content_length_excess_bytes_trimmed) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "helloEXTRA";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    // Compute header length.
    u32 hdr_len = 0;
    for (u32 i = 0; i + 3 < resp_len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_len = i + 4;
            break;
        }
    }
    REQUIRE(hdr_len > 0);

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    loop.backend.clear_ops();
    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Body is complete (5 bytes <= initial body).
    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);
    CHECK_EQ(conn->resp_body_remaining, 0u);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // Send must be headers + 5 body bytes only (not the extra bytes).
    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->send_len, hdr_len + 5);
}

// Proxy request with Content-Length > buffer size: body streamed in multiple cycles.
TEST(streaming, request_body_content_length_multi_chunk) {
    SmallLoop loop;
    loop.setup();

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a POST request with Content-Length: 8000 and partial body.
    // Headers must fit in recv_buf with some body bytes.
    const char* req =
        "POST /upload HTTP/1.1\r\n"
        "Host: test\r\n"
        "Content-Length: 8000\r\n"
        "\r\n";
    u32 req_hdr_len = 0;
    while (req[req_hdr_len]) req_hdr_len++;

    // Fill rest of buffer with body bytes (partial body).
    u32 initial_body = SmallLoop::kBufSize - req_hdr_len;
    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_hdr_len; i++) dst[i] = static_cast<u8>(req[i]);
    for (u32 i = 0; i < initial_body; i++) dst[req_hdr_len + i] = static_cast<u8>(i & 0xFF);
    u32 total_in_buf = req_hdr_len + initial_body;
    conn->recv_buf.commit(total_in_buf);

    // Dispatch the recv event — this parses request and captures metadata.
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(total_in_buf));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should have detected Content-Length body mode.
    CHECK_EQ(conn->req_body_mode, BodyMode::ContentLength);
    // Body remaining = 8000 - initial_body.
    CHECK_GT(conn->req_body_remaining, 0u);

    // Switch to proxy mode.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Request sent to upstream (initial headers + partial body).
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->recv_buf.len())));

    // More body needed — should be waiting for client body.
    CHECK_EQ(conn->on_complete, &on_request_body_recvd<SmallLoop>);

    // Stream remaining body from client in chunks.
    u32 body_sent = initial_body;
    u32 total_body = 8000;
    while (body_sent < total_body) {
        u32 chunk = total_body - body_sent;
        if (chunk > SmallLoop::kBufSize) chunk = SmallLoop::kBufSize;

        // Inject client body data.
        u8 body_chunk[SmallLoop::kBufSize];
        for (u32 i = 0; i < chunk; i++) body_chunk[i] = static_cast<u8>(i & 0xFF);
        inject_custom_recv(loop, *conn, IoEventType::Recv, body_chunk, chunk);

        // Should have forwarded to upstream.
        CHECK_EQ(conn->on_complete, &on_request_body_sent<SmallLoop>);

        // Simulate upstream send completion.
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(chunk)));
        body_sent += chunk;

        if (body_sent < total_body) {
            // More body to stream.
            CHECK_EQ(conn->on_complete, &on_request_body_recvd<SmallLoop>);
        }
    }

    // Request body complete — should transition to waiting for upstream response.
    CHECK_EQ(conn->on_complete, &on_upstream_response<SmallLoop>);
}

// After a chunked request completes, keep-alive resets req_body_mode to None.
TEST(streaming, keep_alive_after_chunked_request_no_stale_state) {
    SmallLoop loop;
    loop.setup();

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // First request: chunked POST.
    const char* req =
        "POST / HTTP/1.1\r\n"
        "Host: test\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    u32 req_len = 0;
    while (req[req_len]) req_len++;

    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);

    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // First request parsed — body mode should be chunked and complete
    // (all chunk data was in the initial buffer).
    CHECK_EQ(conn->req_body_mode, BodyMode::Chunked);

    // The default handler sends a response directly (not proxy mode).
    // Simulate send completion to cycle back to keep-alive.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Should be back to reading next request.
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(conn->state, ConnState::ReadingHeader);

    // Second request: GET with no body.
    const char* req2 = "GET /page HTTP/1.1\r\nHost: test\r\n\r\n";
    u32 req2_len = 0;
    while (req2[req2_len]) req2_len++;

    conn->recv_buf.reset();
    dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req2_len; i++) dst[i] = static_cast<u8>(req2[i]);
    conn->recv_buf.commit(req2_len);

    IoEvent recv_ev2 = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req2_len));
    loop.backend.inject(recv_ev2);
    IoEvent events2[8];
    u32 n2 = loop.backend.wait(events2, 8);
    for (u32 i = 0; i < n2; i++) loop.dispatch(events2[i]);

    // After the second request, req_body_mode must be reset to None.
    CHECK_EQ(conn->req_body_mode, BodyMode::None);
}

// HTTP/1.0 upstream response with no Content-Length or chunked encoding.
// keep_alive defaults to false → BodyMode::UntilClose.
TEST(streaming, http10_until_close) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // HTTP/1.0 response: no CL, no chunked, no Connection header.
    // HTTP/1.0 defaults keep_alive=false → UntilClose.
    const char* resp =
        "HTTP/1.0 200 OK\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // HTTP/1.0 with no body-length indicator → UntilClose.
    CHECK_EQ(conn->resp_body_mode, BodyMode::UntilClose);
    // Should enter streaming mode (body end unknown).
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);

    // Send headers.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(resp_len)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Body chunk.
    u8 body[64];
    for (u32 i = 0; i < 64; i++) body[i] = static_cast<u8>('A');
    inject_custom_recv(loop, *conn, IoEventType::UpstreamRecv, body, 64);
    CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

    // Send completion.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 64));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // EOF from upstream → body complete, connection closed.
    IoEvent eof_ev = make_ev(conn->id, IoEventType::UpstreamRecv, 0);
    loop.backend.inject(eof_ev);
    IoEvent eof_events[8];
    u32 ne = loop.backend.wait(eof_events, 8);
    for (u32 i = 0; i < ne; i++) loop.dispatch(eof_events[i]);

    // UntilClose: client connection closed (EOF signals body end).
    CHECK_EQ(conn->fd, -1);
}

// HTTP/1.1 response with no CL, no chunked, and keep-alive (default).
// Body mode should be None (not UntilClose) since keep-alive means the
// server intends to reuse the connection.
TEST(streaming, keepalive_no_cl_no_body) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // HTTP/1.1 response with no body-length indicators.
    // keep_alive defaults to true for HTTP/1.1 → BodyMode::None.
    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // HTTP/1.1 with no CL/TE → UntilClose (RFC 7230: read until EOF).
    CHECK_EQ(conn->resp_body_mode, BodyMode::UntilClose);
    // UntilClose enters streaming path.
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);
}

// POST with Content-Length: 5 and body "helloEXTRA" — initial upstream send
// must cap at headers + 5 bytes, not forward the trailing "EXTRA" bytes.
TEST(streaming, request_cl_initial_send_capped) {
    SmallLoop loop;
    loop.setup();

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a POST request with Content-Length: 5 and 10 bytes of body area.
    const char* req =
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "helloEXTRA";
    u32 req_len = 0;
    while (req[req_len]) req_len++;

    // Compute header end offset.
    u32 hdr_end = 0;
    for (u32 i = 0; i + 3 < req_len; i++) {
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    REQUIRE(hdr_end > 0);

    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);

    // Dispatch the recv event — parses request headers and body metadata.
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->req_body_mode, BodyMode::ContentLength);
    CHECK_EQ(conn->req_content_length, 5u);

    // Switch to proxy mode and connect upstream.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    loop.backend.clear_ops();

    // Upstream connect succeeds — triggers send of request to upstream.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // The send to upstream must be capped at header_end + Content-Length (5).
    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->send_len, hdr_end + 5);
}

// GET request followed by pipelined bytes — upstream send must stop at
// the end of the request headers, not include trailing bytes.
TEST(streaming, request_no_body_get_caps_at_headers) {
    SmallLoop loop;
    loop.setup();

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a GET request with trailing pipelined data.
    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: x\r\n"
        "\r\n"
        "PIPELINED_DATA";
    u32 req_len = 0;
    while (req[req_len]) req_len++;

    // Compute header end offset.
    u32 hdr_end = 0;
    for (u32 i = 0; i + 3 < req_len; i++) {
        if (req[i] == '\r' && req[i + 1] == '\n' && req[i + 2] == '\r' && req[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    REQUIRE(hdr_end > 0);

    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);

    // Dispatch the recv event — parses request.
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->req_body_mode, BodyMode::None);

    // Switch to proxy mode and connect upstream.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    loop.backend.clear_ops();

    // Upstream connect succeeds — triggers send of request to upstream.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // The send to upstream must stop at the end of headers.
    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->send_len, hdr_end);
}

// After a streamed response completes (CL > 4KB), upstream_recv_armed must be
// cleared. A second request on the same keep-alive connection must work.
TEST(streaming, upstream_armed_cleared_after_body_complete) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream response: CL = 10000 (larger than 4KB buffer → streaming mode).
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10000\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    // Build initial recv_buf: headers + as much body as fits.
    u32 initial_body = SmallLoop::kBufSize - hdr_len;
    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    for (u32 i = 0; i < initial_body; i++) dst[hdr_len + i] = static_cast<u8>(i & 0xFF);
    conn->upstream_recv_buf.commit(hdr_len + initial_body);

    // Dispatch: parse upstream response headers + initial body.
    IoEvent ev =
        make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len + initial_body));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);

    // Send completion of initial headers+body.
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len + initial_body)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Stream remaining body in chunks.
    u32 body_sent = initial_body;
    u32 total_body = 10000;
    while (body_sent < total_body) {
        u32 chunk = total_body - body_sent;
        if (chunk > SmallLoop::kBufSize) chunk = SmallLoop::kBufSize;

        u8 body_chunk[SmallLoop::kBufSize];
        for (u32 i = 0; i < chunk; i++) body_chunk[i] = static_cast<u8>(i & 0xFF);
        inject_custom_recv(loop, *conn, IoEventType::UpstreamRecv, body_chunk, chunk);
        CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);

        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(chunk)));
        body_sent += chunk;
    }

    // Body complete — back to keep-alive.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    // Key assertion: upstream_recv_armed must be cleared after body completes.
    CHECK_EQ(conn->upstream_recv_armed, false);
    CHECK_EQ(conn->upstream_fd, -1);

    // --- Second request on the same keep-alive connection ---
    const char* req2 = "GET /second HTTP/1.1\r\nHost: test\r\n\r\n";
    u32 req2_len = 0;
    while (req2[req2_len]) req2_len++;

    conn->recv_buf.reset();
    dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req2_len; i++) dst[i] = static_cast<u8>(req2[i]);
    conn->recv_buf.commit(req2_len);

    IoEvent recv_ev2 = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req2_len));
    loop.backend.inject(recv_ev2);
    IoEvent events2[8];
    u32 n2 = loop.backend.wait(events2, 8);
    for (u32 i = 0; i < n2; i++) loop.dispatch(events2[i]);

    // Default handler sends a response — verify cycle works.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Now switch to proxy mode for the second request.
    conn->upstream_fd = 200;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Request sent to upstream — send completion.
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->recv_buf.len())));

    // Now waiting for upstream response — upstream_recv_armed should be set.
    CHECK_EQ(conn->on_complete, &on_upstream_response<SmallLoop>);
}

// A POST with Transfer-Encoding: chunked and malformed chunk size ("XY\r\n")
// must be rejected (connection closed) and never forwarded to upstream.
TEST(streaming, malformed_chunked_request_rejected) {
    SmallLoop loop;
    loop.setup();

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a POST request with chunked TE and malformed body.
    const char* req =
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "XY\r\n";
    u32 req_len = 0;
    while (req[req_len]) req_len++;

    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < req_len; i++) dst[i] = static_cast<u8>(req[i]);
    conn->recv_buf.commit(req_len);

    // Dispatch: parse request (will detect malformed chunked body).
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->req_body_mode, BodyMode::Chunked);
    CHECK_EQ(conn->req_malformed, true);

    // Switch to proxy mode and connect upstream.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Upstream connect succeeds — but on_upstream_connected should reject.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Connection must be closed (malformed request rejected, not forwarded).
    CHECK_EQ(conn->fd, -1);
}

// Response with CL:5 but upstream sends 10 body bytes in a streaming chunk.
// The callback should trim to CL boundary and transition to keep-alive, not close.
TEST(streaming, response_body_sent_trimmed_not_closed) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream response: CL = 5 with no body in headers buffer.
    // Use a response where headers alone fit in the buffer so we enter streaming.
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    conn->upstream_recv_buf.commit(hdr_len);

    // Dispatch: parse upstream response headers (no body yet).
    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);
    // Headers-only response goes through streaming path (send headers first).
    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);

    // Send completion of headers.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Upstream sends 10 bytes, but CL remaining is only 5.
    u8 body[10];
    for (u32 i = 0; i < 10; i++) body[i] = static_cast<u8>('A' + i);
    inject_custom_recv(loop, *conn, IoEventType::UpstreamRecv, body, 10);

    // on_response_body_recvd should trim to 5 bytes and set on_response_body_sent.
    CHECK_EQ(conn->on_complete, &on_response_body_sent<SmallLoop>);
    CHECK_EQ(conn->resp_body_remaining, 0u);

    // Send completion with the trimmed length (5 bytes).
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 5));

    // Body complete (CL satisfied) — should transition to keep-alive, NOT close.
    CHECK(conn->fd != -1);
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
}

// 1xx Continue is skipped; final 200 response is forwarded.
TEST(streaming, skip_1xx_continue_then_200) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream sends 100 Continue + 200 OK in a single buffer.
    const char* resp =
        "HTTP/1.1 100 Continue\r\n"
        "\r\n"
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "OK";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // The 100 Continue must be skipped; final response is 200.
    CHECK_EQ(conn->resp_status, static_cast<u16>(200));
    CHECK_EQ(conn->resp_body_mode, BodyMode::ContentLength);
    // Small body fits in initial recv — single-buffer path.
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
}

// 101 Switching Protocols is NOT skipped as an interim 1xx.
TEST(streaming, _101_not_skipped) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // 101 is terminal — must NOT be skipped.
    CHECK_EQ(conn->resp_status, static_cast<u16>(101));
    // No CL, no chunked, no keep-alive → UntilClose (streaming).
    CHECK_EQ(conn->resp_body_mode, BodyMode::UntilClose);
}

// 205 Reset Content has no body (same as 204/304).
TEST(streaming, status_205_no_body) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 205 Reset Content\r\n"
        "\r\n";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->resp_body_mode, BodyMode::None);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
}

// During response body streaming, a client Recv event should be ignored.
TEST(streaming, response_body_ignores_client_recv) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Upstream response with headers only (body streams later).
    const char* resp_hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 10000\r\n"
        "\r\n";
    u32 hdr_len = 0;
    while (resp_hdr[hdr_len]) hdr_len++;

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < hdr_len; i++) dst[i] = static_cast<u8>(resp_hdr[i]);
    conn->upstream_recv_buf.commit(hdr_len);

    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(hdr_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->on_complete, &on_response_header_sent<SmallLoop>);

    // Send headers completion → now waiting for body recv from upstream.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(hdr_len)));
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);

    // Inject a client Recv event (wrong type — should be UpstreamRecv).
    // With separate buffers, client data goes to recv_buf (harmless),
    // and on_response_body_recvd ignores it. No purge needed.
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));

    // Connection stays alive, still waiting for upstream body.
    CHECK(conn->fd >= 0);
    CHECK_EQ(conn->on_complete, &on_response_body_recvd<SmallLoop>);
}

// After streaming completes and connection returns to ReadingHeader,
// an UpstreamRecv event should be ignored (wrong event type → close).
TEST(streaming, stale_upstream_recv_ignored_in_header_reading) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Small response that completes immediately.
    inject_upstream_response(loop, *conn);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // Send completion → back to ReadingHeader (keep-alive).
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);

    // Inject a stale UpstreamRecv (wrong type for on_header_received).
    // on_header_received purges recv_buf and re-arms client recv (does not close).
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamRecv, 50));

    // Connection stays alive — stale UpstreamRecv is silently ignored.
    CHECK(conn->fd >= 0);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
}

// Chunked response that completes in initial buffer — excess bytes past
// terminal chunk must not be sent to the client.
TEST(streaming, chunked_response_initial_buffer_capped) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    const char* resp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n0\r\n\r\nEXTRA";
    u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;

    // Compute header length.
    u32 hdr_len = 0;
    for (u32 i = 0; i + 3 < resp_len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_len = i + 4;
            break;
        }
    }
    REQUIRE(hdr_len > 0);

    conn->upstream_recv_buf.reset();
    u8* dst = conn->upstream_recv_buf.write_ptr();
    for (u32 i = 0; i < resp_len; i++) dst[i] = static_cast<u8>(resp[i]);
    conn->upstream_recv_buf.commit(resp_len);

    loop.backend.clear_ops();
    IoEvent ev = make_ev(conn->id, IoEventType::UpstreamRecv, static_cast<i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->resp_body_mode, BodyMode::Chunked);
    // Entire chunked body done in initial buffer — single-buffer path.
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // The send must NOT include "EXTRA" — only headers + chunked body.
    // Chunked body: "5\r\nhello\r\n0\r\n\r\n" = 15 bytes.
    u32 chunked_body_len = 15;  // "5\r\nhello\r\n0\r\n\r\n"
    auto* send_op = loop.backend.last_op(MockOp::Send);
    REQUIRE(send_op != nullptr);
    CHECK_EQ(send_op->send_len, hdr_len + chunked_body_len);
}

// on_proxy_response_sent must close upstream_fd and clear armed flags
// before re-arming for the next keep-alive request.
TEST(streaming, proxy_response_sent_closes_upstream) {
    SmallLoop loop;
    loop.setup();
    auto* conn = setup_proxy_conn(loop);
    REQUIRE(conn != nullptr);

    // Inject a small response (fits in one buffer → on_proxy_response_sent).
    inject_upstream_response(loop, *conn);
    u32 resp_len = conn->upstream_recv_buf.len();

    // Complete the send → on_proxy_response_sent.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(resp_len)));

    // upstream_fd should be closed, armed flags cleared.
    CHECK_EQ(conn->upstream_fd, -1);
    CHECK_EQ(conn->upstream_recv_armed, false);
    CHECK_EQ(conn->upstream_send_armed, false);
    // Connection back to ReadingHeader (keep-alive).
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK(conn->fd >= 0);
}

// Two proxy requests on the same keep-alive connection.
// Without upstream_fd cleanup, the second request would hang.
TEST(streaming, proxy_keepalive_two_requests) {
    SmallLoop loop;
    loop.setup();

    // First request cycle.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));

    // Proxy setup.
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));

    // Upstream response.
    inject_upstream_response(loop, *conn);
    u32 resp_len = conn->upstream_recv_buf.len();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(resp_len)));

    // Should be back to ReadingHeader with upstream closed.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->upstream_fd, -1);

    // Second request cycle — should work.
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 100));
    conn->upstream_fd = 200;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));

    // Should successfully send to upstream (not hang).
    CHECK_GT(loop.backend.count_ops(MockOp::Send), 0u);
}

// === Pipeline ===

// Helper: write raw bytes into conn's recv_buf and dispatch as Recv event.
static void inject_raw_recv(SmallLoop& loop, Connection& conn, const char* data, u32 len) {
    conn.recv_buf.reset();
    u8* dst = conn.recv_buf.write_ptr();
    for (u32 i = 0; i < len; i++) dst[i] = static_cast<u8>(data[i]);
    conn.recv_buf.commit(len);
    IoEvent ev = make_ev(conn.id, IoEventType::Recv, static_cast<i32>(len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
}

// Two GET requests concatenated in one recv. Both should get responses.
TEST(pipeline, two_gets_direct_response) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.backend.clear_ops();

    // Two complete GET requests in one buffer.
    const char* two_gets =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 two_len = 27 + 28;  // first=27, second=28
    inject_raw_recv(loop, *conn, two_gets, two_len);

    // First request should be processed and response sent.
    // on_header_received fires, sends kResponse200, callback = on_response_sent.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Complete the first send.
    u32 send_len = conn->send_buf.len();
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline: second request should be dispatched immediately (no new recv).
    // After on_response_sent, pipeline_shift finds leftover bytes and re-enters
    // on_header_received. The second response is now being sent.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 1u);

    // Complete the second send.
    send_len = conn->send_buf.len();
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline drained — back to ReadingHeader, submit_recv issued.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 0u);
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
}

// POST with Content-Length:5 body "hello" + GET pipelined. Both processed.
TEST(pipeline, post_cl_then_get) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.backend.clear_ops();

    // POST with CL:5 body "hello" followed by a GET request.
    const char* data =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 total_len = 0;
    while (data[total_len]) total_len++;
    inject_raw_recv(loop, *conn, data, total_len);

    // First request (POST) should be processed.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Complete the first send.
    u32 send_len = conn->send_buf.len();
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline: GET should be dispatched immediately.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 1u);

    // Complete the second send.
    send_len = conn->send_buf.len();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline drained.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->pipeline_depth, 0u);
}

// 17 GETs pipelined. First 16 processed via recursion, 17th falls through to normal recv.
TEST(pipeline, depth_limit_respected) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Build 17 GET requests concatenated.
    // Each "GET / HTTP/1.1\r\nHost: x\r\n\r\n" = 27 bytes.
    // 17 * 27 = 459 bytes (fits in 4096 buffer).
    const char* one_get = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 one_len = 27;
    u32 total = one_len * 17;

    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 r = 0; r < 17; r++) {
        for (u32 i = 0; i < one_len; i++) dst[r * one_len + i] = static_cast<u8>(one_get[i]);
    }
    conn->recv_buf.commit(total);
    IoEvent ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(total));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // First request processed, waiting for send completion.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Process sends for requests 1-16 (each send triggers pipeline dispatch of the next).
    // Send 1 completes → pipeline dispatches request 2 (depth 0→1).
    // Send 2 completes → pipeline dispatches request 3 (depth 1→2).
    // ...
    // Send 16 completes → pipeline dispatches request 17 (depth 15→16).
    for (u32 r = 0; r < 16; r++) {
        u32 send_len = conn->send_buf.len();
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));
    }

    // After 16 sends, request 17 is being sent (depth = 16).
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 16u);

    // Complete request 17's send. pipeline_shift returns false (depth >= max).
    u32 send_len = conn->send_buf.len();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline drained — falls through to normal recv re-arm.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 0u);
}

// Complete GET + leftover bytes. First processes via pipeline, leftover is shifted
// and dispatched to on_header_received (which processes it as a new request).
TEST(pipeline, incomplete_second_request) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.backend.clear_ops();

    // Complete GET + partial second request (missing \r\n\r\n terminator).
    const char* data =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHos";
    u32 total_len = 27 + 20;
    inject_raw_recv(loop, *conn, data, total_len);

    // First request processed.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Complete the send.
    u32 send_len = conn->send_buf.len();
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // Pipeline shift moved the 20 leftover bytes to recv_buf start and
    // re-entered on_header_received. The partial request triggers Incomplete
    // parse, so the connection waits for more data instead of sending a
    // spurious response.
    // pipeline_depth stays > 0 so subsequent recvs continue the Incomplete check.
    CHECK_GT(conn->pipeline_depth, 0u);
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(conn->recv_buf.len(), 20u);  // leftover preserved
}

// Proxy request + pipelined GET. Verify stash in send_buf, then recover after proxy response.
TEST(pipeline, proxy_stash_and_recover) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a GET request + pipelined second GET into recv_buf.
    const char* data =
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 first_len = 27;
    u32 second_len = 28;
    u32 total_len = first_len + second_len;
    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < total_len; i++) dst[i] = static_cast<u8>(data[i]);
    conn->recv_buf.commit(total_len);

    // Dispatch the recv event manually (don't use inject_and_dispatch which
    // would overwrite recv_buf with garbage bytes).
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(total_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // The direct response path processed request 1; now switch to proxy for a
    // different test: simulate that on_header_received routed to proxy.
    // Reset to proxy flow: pretend the first request was a proxy request.
    // We need to test that on_upstream_request_sent stashes and
    // on_proxy_response_sent recovers.

    // Instead, let's set up from scratch in proxy mode with pipelined data.
    // Re-setup connection for proxy flow.
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write proxy request + pipelined GET.
    conn->recv_buf.reset();
    dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < total_len; i++) dst[i] = static_cast<u8>(data[i]);
    conn->recv_buf.commit(total_len);

    // Parse request metadata (sets req_initial_send_len = 27).
    recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(total_len));
    loop.backend.inject(recv_ev);
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // First request got a direct response. Complete its send.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);

    // Instead of completing normally, hijack to proxy mode.
    // This is complex; let's use a simpler approach: manually test stash/recover.

    // --- Test pipeline_stash and pipeline_recover directly ---
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write two requests into recv_buf.
    conn->recv_buf.reset();
    dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < total_len; i++) dst[i] = static_cast<u8>(data[i]);
    conn->recv_buf.commit(total_len);
    conn->req_initial_send_len = first_len;

    // Stash leftover bytes into send_buf.
    pipeline_stash(*conn);
    CHECK_EQ(conn->pipeline_stash_len, static_cast<u16>(second_len));
    CHECK_EQ(conn->send_buf.len(), second_len);

    // Verify stashed content matches the second request.
    const u8* stashed = conn->send_buf.data();
    const char* expected = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    bool match = true;
    for (u32 i = 0; i < second_len; i++) {
        if (stashed[i] != static_cast<u8>(expected[i])) {
            match = false;
            break;
        }
    }
    CHECK(match);

    // Simulate: recv_buf was reset for upstream response, now recover.
    conn->recv_buf.reset();
    bool recovered = pipeline_recover(*conn);
    CHECK(recovered);
    CHECK_EQ(conn->recv_buf.len(), second_len);
    CHECK_EQ(conn->pipeline_stash_len, 0u);
    CHECK_EQ(conn->pipeline_depth, 1u);

    // Verify recovered content matches the second request.
    const u8* recv_data = conn->recv_buf.data();
    match = true;
    for (u32 i = 0; i < second_len; i++) {
        if (recv_data[i] != static_cast<u8>(expected[i])) {
            match = false;
            break;
        }
    }
    CHECK(match);
}

// Single request (no pipelining). Verify pipeline_depth stays 0.
TEST(pipeline, no_pipeline_resets_depth) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    loop.backend.clear_ops();

    // Single GET with no trailing bytes.
    const char* single = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 single_len = 27;
    inject_raw_recv(loop, *conn, single, single_len);

    // Request processed.
    CHECK_EQ(conn->on_complete, &on_response_sent<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 0u);

    // Complete send — no leftover, pipeline_shift returns false.
    u32 send_len = conn->send_buf.len();
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(send_len)));

    // pipeline_depth reset to 0 in the submit_recv fallthrough path.
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
    CHECK_EQ(conn->on_complete, &on_header_received<SmallLoop>);
    CHECK_EQ(conn->pipeline_depth, 0u);
    CHECK_GE(loop.backend.count_ops(MockOp::Recv), 1u);
}

// Exact single request — pipeline_leftover returns 0.
TEST(pipeline, leftover_returns_zero_for_exact_request) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Write a single exact GET request into recv_buf.
    const char* exact = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    u32 exact_len = 27;
    conn->recv_buf.reset();
    u8* dst = conn->recv_buf.write_ptr();
    for (u32 i = 0; i < exact_len; i++) dst[i] = static_cast<u8>(exact[i]);
    conn->recv_buf.commit(exact_len);

    // Parse to set req_initial_send_len.
    capture_request_metadata(*conn);

    // req_initial_send_len should equal recv_buf.len() — no leftover bytes.
    CHECK_EQ(conn->req_initial_send_len, conn->recv_buf.len());
    CHECK_EQ(pipeline_leftover(*conn), 0u);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
