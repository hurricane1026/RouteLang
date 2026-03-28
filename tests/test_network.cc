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

    // Upstream response → forward to client
    loop.backend.clear_ops();
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamRecv, 200));
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
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamRecv, 200));
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
        loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamRecv, 200));
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

    // upstream response → recv_buf gets new data (upstream response)
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamRecv, 200));
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

    // Upstream request sent → on_upstream_request_sent resets recv_buf for response
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 100));
    CHECK_EQ(conn->recv_buf.len(), 0u);  // reset by on_upstream_request_sent

    // Upstream response received → data goes into recv_buf
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamRecv, 200));
    // on_upstream_response does NOT reset recv_buf (send still in progress)
    CHECK_EQ(conn->on_complete, &on_proxy_response_sent<SmallLoop>);

    // Proxy response sent → on_proxy_response_sent resets recv_buf
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, 200));
    CHECK_EQ(conn->recv_buf.len(), 0u);  // reset by on_proxy_response_sent
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

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
