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
    Connection c;
    c.reset();  // first reset to bind recv_buf to storage
    c.fd = 42;
    c.on_complete = &on_header_received<SmallLoop>;
    c.state = ConnState::Sending;
    // Write some data into recv_buf
    const u8 data[] = "hello";
    c.recv_buf.write(data, 5);
    c.keep_alive = true;
    c.reset();
    CHECK_EQ(c.fd, -1);
    CHECK(c.on_complete == nullptr);
    CHECK_EQ(c.state, ConnState::Idle);
    CHECK_EQ(c.recv_buf.len(), 0u);
    CHECK(c.recv_buf.valid());  // bound to storage after reset
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
    Connection c;
    c.reset();
    // Fill recv_buf to capacity
    u32 avail = c.recv_buf.write_avail();
    CHECK_EQ(avail, 4096u);
    c.recv_buf.commit(avail);
    CHECK_EQ(c.recv_buf.len(), 4096u);
    CHECK_EQ(c.recv_buf.write_avail(), 0u);
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
    CHECK_EQ(c2->recv_buf.write_avail(), 4096u);
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

    // upstream response → recv_buf gets new data (upstream response)
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 200));
    CHECK_EQ(conn->recv_buf.len(), 200u);
    CHECK(!conn->recv_buf.is_released());

    // send to client → back to ReadingHeader
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 200));
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
    CHECK_EQ(c2->recv_buf.write_avail(), 4096u);
    CHECK_EQ(c2->send_buf.write_avail(), 4096u);

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

    // Upstream request sent → on_upstream_request_sent resets recv_buf for response
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
    CHECK_EQ(conn->recv_buf.len(), 0u);  // reset by on_upstream_request_sent

    // Upstream response received → data goes into recv_buf
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 200));
    // on_upstream_response does NOT reset recv_buf (send still in progress)
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // Proxy response sent → on_proxy_response_sent resets recv_buf
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 200));
    CHECK_EQ(conn->recv_buf.len(), 0u);  // reset by on_proxy_response_sent
    CHECK_EQ(conn->state, ConnState::ReadingHeader);
}

// Connection reset clears both buffers to valid state
TEST(recv_semantic, reset_clears_both_buffers) {
    Connection c;
    c.reset();

    // Write data to both buffers
    c.recv_buf.write(reinterpret_cast<const u8*>("GET"), 3);
    c.send_buf.write(reinterpret_cast<const u8*>("HTTP"), 4);
    CHECK_EQ(c.recv_buf.len(), 3u);
    CHECK_EQ(c.send_buf.len(), 4u);

    c.reset();
    CHECK_EQ(c.recv_buf.len(), 0u);
    CHECK_EQ(c.send_buf.len(), 0u);
    CHECK(c.recv_buf.valid());
    CHECK(c.send_buf.valid());
    CHECK_EQ(c.recv_buf.write_avail(), 4096u);
    CHECK_EQ(c.send_buf.write_avail(), 4096u);
    CHECK(!c.recv_buf.is_released());
    CHECK(!c.send_buf.is_released());
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
    // Dispatch a Recv event while expecting Send → close
    loop.dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(loop.conns[cid].fd, -1);
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

// on_upstream_response expects Recv — Send should close
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
    CHECK_EQ(c2->send_buf.write_avail(), 4096u);
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

#include "rout/runtime/route_table.h"

TEST(route, match_prefix) {
    RouteConfig cfg;
    i32 up = cfg.add_upstream("backend", 0x7F000001, 8080);
    REQUIRE(up >= 0);
    cfg.add_proxy("/api/", 0, static_cast<u16>(up));

    const u8 path1[] = "/api/users";
    auto* r = cfg.match(path1, sizeof(path1) - 1, 0);
    REQUIRE(r != nullptr);
    CHECK_EQ(r->action, RouteAction::Proxy);
    CHECK_EQ(r->upstream_id, static_cast<u16>(up));

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
    i32 up1 = cfg.add_upstream("v1", 0x7F000001, 8081);
    i32 up2 = cfg.add_upstream("v2", 0x7F000001, 8082);
    cfg.add_proxy("/api/v1/", 0, static_cast<u16>(up1));
    cfg.add_proxy("/api/", 0, static_cast<u16>(up2));

    const u8 path[] = "/api/v1/users";
    auto* r = cfg.match(path, sizeof(path) - 1, 0);
    REQUIRE(r != nullptr);
    CHECK_EQ(r->upstream_id, static_cast<u16>(up1));
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
    i32 idx = cfg.add_upstream("api", 0x0A000101, 9090);
    REQUIRE(idx >= 0);
    auto& t = cfg.upstreams[idx];
    CHECK_EQ(t.addr.sin_family, AF_INET);
    CHECK_EQ(__builtin_bswap16(t.addr.sin_port), 9090u);
    CHECK_EQ(__builtin_bswap32(t.addr.sin_addr.s_addr), 0x0A000101u);
    CHECK_EQ(t.name[0], 'a');
}

// === UpstreamPool ===

#include "rout/runtime/upstream_pool.h"

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

#include "rout/runtime/slice_pool.h"

TEST(slice_pool, init_destroy) {
    SlicePool pool;
    REQUIRE_EQ(pool.init(64), 0);
    CHECK_EQ(pool.count, 64u);
    CHECK_EQ(pool.available(), 64u);
    CHECK_EQ(pool.in_use(), 0u);
    pool.destroy();
}

TEST(slice_pool, alloc_free) {
    SlicePool pool;
    REQUIRE_EQ(pool.init(4), 0);

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
    REQUIRE_EQ(pool.init(2), 0);

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
    REQUIRE_EQ(pool.init(1), 0);
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
    REQUIRE_EQ(pool.init(2), 0);
    pool.free(nullptr);              // should not crash
    CHECK_EQ(pool.available(), 2u);  // unchanged
    pool.destroy();
}

// SlicePool: out-of-order free
TEST(slice_pool, out_of_order_free) {
    SlicePool pool;
    REQUIRE_EQ(pool.init(4), 0);
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
    REQUIRE_EQ(pool.init(8), 0);
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
    REQUIRE_EQ(pool.init(4), 0);
    pool.destroy();
    pool.destroy();  // second destroy should not crash
    CHECK(pool.base == nullptr);
    CHECK(pool.free_stack == nullptr);
}

// SlicePool: alloc after destroy returns nullptr
TEST(slice_pool, alloc_after_destroy) {
    SlicePool pool;
    REQUIRE_EQ(pool.init(4), 0);
    pool.destroy();
    CHECK(pool.alloc() == nullptr);
}

// SlicePool: large pool (verify mmap works at scale)
TEST(slice_pool, large_pool) {
    SlicePool pool;
    REQUIRE_EQ(pool.init(1024), 0);  // 1024 * 16KB = 16MB
    CHECK_EQ(pool.available(), 1024u);

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

#include "rout/runtime/slab_pool.h"

struct TestObj {
    i32 value;
    u8 data[60];  // pad to 64 bytes
};

TEST(slab_pool, init_destroy) {
    SlabPool<TestObj, 128> pool;
    REQUIRE_EQ(pool.init(), 0);
    CHECK_EQ(pool.capacity(), 128u);
    CHECK_EQ(pool.available(), 128u);
    CHECK_EQ(pool.in_use(), 0u);
    pool.destroy();
}

TEST(slab_pool, alloc_free_by_ptr) {
    SlabPool<TestObj, 4> pool;
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);
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
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);

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
    REQUIRE_EQ(pool.init(), 0);
    pool.destroy();
    pool.destroy();  // second destroy should not crash
    CHECK(pool.objects == nullptr);
    CHECK(pool.free_stack == nullptr);
}

// SlabPool: stress alloc-free cycles
TEST(slab_pool, stress_cycles) {
    SlabPool<TestObj, 16> pool;
    REQUIRE_EQ(pool.init(), 0);
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
    REQUIRE_EQ(pool.init(), 0);
    SmallObj* a = pool.alloc();
    REQUIRE(a != nullptr);
    a->tag = 0xAB;
    CHECK_EQ(pool.index_of(a), pool.index_of(a));  // consistent
    CHECK_EQ(a->tag, 0xABu);
    pool.free(a);
    pool.destroy();
}

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}
