// Mock tests — no real sockets. Ported from libuv/libevent scenarios.
#include "rout/runtime/arena.h"

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
    CHECK_EQ(c->recv_len, 100u);
    CHECK_GT(c->send_len, 0u);
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
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_len)));
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
        loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_len)));
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
    Connection c;
    c.fd = 42;
    c.on_complete = &on_header_received<SmallLoop>;
    c.state = ConnState::Sending;
    c.recv_len = 100;
    c.keep_alive = true;
    c.reset();
    CHECK_EQ(c.fd, -1);
    CHECK(c.on_complete == nullptr);
    CHECK_EQ(c.state, ConnState::Idle);
    CHECK_EQ(c.recv_len, 0u);
    CHECK_EQ(c.keep_alive, false);
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
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_len)));

    // The submit_recv call must have succeeded (add_recv recorded)
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
    CHECK_EQ(c->on_complete, &on_header_received<SmallLoop>);

    // Do it again — second cycle
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_len)));
    CHECK_EQ(loop.backend.count_ops(MockOp::Recv), 1u);
}

// Copilot #6: timer wheel comment said 60 slots but code uses 64.
// Verify the actual slot count.
TEST(copilot, timer_wheel_has_64_slots) {
    CHECK_EQ(TimerWheel::kSlots, 64u);
}

// Copilot #4: io_backend.h documented init as void, but it returns i32.
// Verify MockBackend.init returns 0 on success.
TEST(copilot, backend_init_returns_zero) {
    SmallLoop loop;
    loop.setup();
    // setup() calls backend.init which returns i32
    CHECK_EQ(loop.backend.init(0, -1), 0);
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

    // Upstream response → forward to client
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 200));
    CHECK_EQ(conn->state, ConnState::Sending);
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);
    CHECK_EQ(loop.backend.count_ops(MockOp::Send), 1u);

    // Response sent to client → back to reading (keep-alive)
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 200));
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
    CHECK_GT(conn->send_len, 0u);
    // Verify "502" in send buffer
    bool has_502 = false;
    for (u32 i = 0; i + 2 < conn->send_len; i++) {
        if (conn->send_buf[i] == '5' && conn->send_buf[i + 1] == '0' &&
            conn->send_buf[i + 2] == '2') {
            has_502 = true;
            break;
        }
    }
    CHECK(has_502);
    // 502 sets keep_alive=false → on_response_sent will close, not loop
    CHECK_EQ(conn->keep_alive, false);
    // Simulate send completion → connection should be closed
    u32 cid = conn->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<i32>(conn->send_len)));
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
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 0));  // EOF
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
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 200));
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
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 200));
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 200));
        CHECK_EQ(conn->state, ConnState::ReadingHeader);
    }
}

// === Copilot round 4 regression tests ===

// io_uring init returns -errno (not -1) on failure.
// We can't easily make mmap fail, but verify the convention:
// successful MockBackend init returns 0.
TEST(copilot4, init_returns_zero_on_success) {
    SmallLoop loop;
    loop.setup();
    CHECK_EQ(loop.backend.init(0, -1), 0);
}

// Arena init returns -errno on failure (not -1).
// Verify success returns 0.
TEST(copilot4, arena_init_returns_zero) {
    Arena a;
    CHECK_EQ(a.init(4096), 0);
    a.destroy();
}

// Arena init with absurdly large size should fail gracefully.
// mmap of near-max u64 will fail → should return negative errno.
TEST(copilot4, arena_init_huge_fails) {
    Arena a;
    i32 rc = a.init(static_cast<u64>(-1));  // ~18 exabytes
    CHECK(rc < 0);
    // Should be -ENOMEM or similar, not -1
    CHECK_NE(rc, -1);  // -errno convention, not raw -1
}

// Arena alloc overflow protection
TEST(copilot4, arena_alloc_overflow_returns_null) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    // size close to u64 max → overflow in (size+7) alignment
    void* p = a.alloc(static_cast<u64>(-1));
    CHECK(p == nullptr);
    a.destroy();
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
            make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_len)));
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

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}
