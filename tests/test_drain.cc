// Graceful shutdown + connection draining tests.
#include "rut/runtime/drain.h"
#include "rut/runtime/event_loop.h"
#include "test.h"
#include "test_helpers.h"

// === DrainConfig defaults ===

TEST(drain_config, defaults) {
    rut::DrainConfig cfg;
    CHECK_EQ(cfg.period_secs, 30u);
}

// === should_drain_close ===

TEST(drain_prob, always_true_at_deadline) {
    // At elapsed == period, every connection should close.
    for (rut::u32 id = 0; id < 100; id++) {
        CHECK(rut::should_drain_close(id, 1000, 1030, 30));
    }
}

TEST(drain_prob, always_true_past_deadline) {
    for (rut::u32 id = 0; id < 100; id++) {
        CHECK(rut::should_drain_close(id, 1000, 1100, 30));
    }
}

TEST(drain_prob, always_true_zero_period) {
    for (rut::u32 id = 0; id < 100; id++) {
        CHECK(rut::should_drain_close(id, 1000, 1000, 0));
    }
}

TEST(drain_prob, none_at_start) {
    // At elapsed == 0, threshold < 0 is never true → no closes.
    rut::u32 closed = 0;
    for (rut::u32 id = 0; id < 1000; id++) {
        if (rut::should_drain_close(id, 1000, 1000, 30)) closed++;
    }
    CHECK_EQ(closed, 0u);
}

TEST(drain_prob, increases_over_time) {
    // At 10% through, ~10% should close. At 50%, ~50%.
    // Use large sample for statistical stability.
    auto count_closes = [](rut::u64 elapsed, rut::u32 period) {
        rut::u32 closed = 0;
        for (rut::u32 id = 0; id < 10000; id++) {
            if (rut::should_drain_close(id, 0, elapsed, period)) closed++;
        }
        return closed;
    };

    rut::u32 at_10pct = count_closes(3, 30);   // 10%
    rut::u32 at_50pct = count_closes(15, 30);  // 50%
    rut::u32 at_90pct = count_closes(27, 30);  // 90%

    // Allow ±15% tolerance due to hash distribution.
    CHECK(at_10pct > 0);
    CHECK(at_10pct < 2500);  // < 25%
    CHECK(at_50pct > 3500);  // > 35%
    CHECK(at_50pct < 6500);  // < 65%
    CHECK(at_90pct > 7500);  // > 75%
}

// === monotonic_secs ===

TEST(monotonic, returns_nonzero) {
    rut::u64 t = rut::monotonic_secs();
    CHECK(t > 0);
}

TEST(monotonic, non_decreasing) {
    rut::u64 a = rut::monotonic_secs();
    rut::u64 b = rut::monotonic_secs();
    CHECK(b >= a);
}

// === EventLoop drain mode ===

TEST(event_loop_drain, initial_state_not_draining) {
    SmallLoop loop;
    loop.setup();
    CHECK(!loop.is_draining());
}

TEST(event_loop_drain, active_count_empty) {
    using rut::EpollBackend;
    using rut::EventLoop;
    // Use SmallLoop which tracks free_top
    SmallLoop loop;
    loop.setup();
    CHECK_EQ(loop.free_top, SmallLoop::kMaxConns);
}

// === Drain accepts: served gracefully, not RST'd ===

TEST(drain_accept, accepts_during_drain_get_response) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    // Accept during drain — should still be processed, not closed.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    // Connection is accepted and keep_alive should be false.
    CHECK(!c->keep_alive);
    CHECK_EQ(c->state, ConnState::ReadingHeader);
}

TEST(drain_accept, drain_accept_full_cycle) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;
    ShardMetrics m;
    m.init();
    loop.metrics = &m;

    // Accept → recv → send → closed
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;

    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));
    rut::u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(send_len)));

    // After response with Connection: close, connection should be freed.
    CHECK_EQ(loop.conns[cid].fd, -1);
    CHECK_EQ(m.requests_total, 1u);
    CHECK_EQ(m.requests_active, 0u);
}

// === Callbacks respect drain ===

TEST(drain_callback, response_has_connection_close) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    // Accept a connection
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // The accept should have set keep_alive = false since draining
    // Note: SmallLoop::dispatch doesn't check draining for accept (it's manual),
    // but callbacks check loop->is_draining() on header received.
    loop.backend.clear_ops();

    // Simulate recv (header received)
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));

    // Connection should be marked for close
    CHECK(!c->keep_alive);
    CHECK_EQ(c->state, ConnState::Sending);

    // Verify send_buf contains "close" (from "Connection: close")
    const char* data = reinterpret_cast<const char*>(c->send_buf.data());
    rut::u32 len = c->send_buf.len();
    bool found_close = false;
    for (rut::u32 i = 0; i + 5 <= len; i++) {
        if (data[i] == 'c' && data[i + 1] == 'l' && data[i + 2] == 'o' && data[i + 3] == 's' &&
            data[i + 4] == 'e') {
            found_close = true;
            break;
        }
    }
    CHECK(found_close);
}

TEST(drain_callback, non_drain_response_has_keep_alive) {
    SmallLoop loop;
    loop.setup();
    // Not draining — default

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.backend.clear_ops();

    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));

    CHECK(c->keep_alive);

    // Verify send_buf contains "keep-alive"
    const char* data = reinterpret_cast<const char*>(c->send_buf.data());
    rut::u32 len = c->send_buf.len();
    bool found_ka = false;
    for (rut::u32 i = 0; i + 10 <= len; i++) {
        if (data[i] == 'k' && data[i + 1] == 'e' && data[i + 2] == 'e' && data[i + 3] == 'p') {
            found_ka = true;
            break;
        }
    }
    CHECK(found_ka);
}

TEST(drain_callback, close_after_drain_response_sent) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    // Accept + recv
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));

    // Send response — since keep_alive=false, connection should be closed after send
    rut::u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(send_len)));

    // Connection should have been freed (fd == -1 after close_conn)
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Proxy drain: upstream response rewrite ===

TEST(drain_proxy, upstream_response_rewrites_connection_header) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    // Accept + recv + set up proxy path
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));

    // Wire to proxy: upstream connect
    c->upstream_fd = 99;
    c->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    // Forward request to upstream
    rut::u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(req_len)));

    // Now inject upstream response with Connection: keep-alive header
    // Manually write HTTP response into upstream_recv_buf
    c->upstream_recv_buf.reset();
    const char* resp = "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nOK";
    rut::u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;
    rut::u8* dst = c->upstream_recv_buf.write_ptr();
    for (rut::u32 i = 0; i < resp_len; i++) dst[i] = static_cast<rut::u8>(resp[i]);
    c->upstream_recv_buf.commit(resp_len);

    // Inject the recv event manually (bypass inject_and_dispatch mock data fill)
    IoEvent ev = make_ev(cid, IoEventType::UpstreamRecv, static_cast<rut::i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    rut::u32 n = loop.backend.wait(events, 8);
    for (rut::u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // The Connection header should have been rewritten to "close" in upstream_recv_buf
    const char* data = reinterpret_cast<const char*>(c->upstream_recv_buf.data());
    rut::u32 len = c->upstream_recv_buf.len();
    bool found_close = false;
    for (rut::u32 i = 0; i + 15 <= len; i++) {
        if (data[i] == 'C' && data[i + 1] == 'o' && data[i + 2] == 'n' && data[i + 3] == 'n' &&
            data[i + 4] == 'e' && data[i + 5] == 'c' && data[i + 6] == 't' && data[i + 7] == 'i' &&
            data[i + 8] == 'o' && data[i + 9] == 'n' && data[i + 10] == ':' &&
            data[i + 11] == ' ' && data[i + 12] == 'c' && data[i + 13] == 'l' &&
            data[i + 14] == 'o') {
            found_close = true;
            break;
        }
    }
    CHECK(found_close);
}

TEST(drain_proxy, upstream_response_rewrites_lowercase_connection_header) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));

    c->upstream_fd = 99;
    c->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    rut::u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(req_len)));

    c->upstream_recv_buf.reset();
    const char* resp = "HTTP/1.1 200 OK\r\nconnection: keep-alive\r\nContent-Length: 2\r\n\r\nOK";
    rut::u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;
    rut::u8* dst = c->upstream_recv_buf.write_ptr();
    for (rut::u32 i = 0; i < resp_len; i++) dst[i] = static_cast<rut::u8>(resp[i]);
    c->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(cid, IoEventType::UpstreamRecv, static_cast<rut::i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    rut::u32 n = loop.backend.wait(events, 8);
    for (rut::u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    const char* data = reinterpret_cast<const char*>(c->upstream_recv_buf.data());
    bool found_close = false;
    for (rut::u32 i = 0; i + 18 <= c->upstream_recv_buf.len(); i++) {
        if (data[i] == 'c' && data[i + 1] == 'o' && data[i + 2] == 'n' && data[i + 3] == 'n' &&
            data[i + 4] == 'e' && data[i + 5] == 'c' && data[i + 6] == 't' && data[i + 7] == 'i' &&
            data[i + 8] == 'o' && data[i + 9] == 'n' && data[i + 10] == ':' &&
            data[i + 11] == ' ' && data[i + 12] == 'c' && data[i + 13] == 'l' &&
            data[i + 14] == 'o' && data[i + 15] == 's' && data[i + 16] == 'e') {
            found_close = true;
            break;
        }
    }
    CHECK(found_close);
}

TEST(drain_proxy, upstream_response_injects_close_when_missing) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));

    c->upstream_fd = 99;
    c->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    rut::u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(req_len)));

    // Upstream response WITHOUT Connection header
    c->upstream_recv_buf.reset();
    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    rut::u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;
    rut::u8* dst = c->upstream_recv_buf.write_ptr();
    for (rut::u32 i = 0; i < resp_len; i++) dst[i] = static_cast<rut::u8>(resp[i]);
    c->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(cid, IoEventType::UpstreamRecv, static_cast<rut::i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    rut::u32 n = loop.backend.wait(events, 8);
    for (rut::u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Should have been routed through send_buf with Connection: close injected
    // The on_complete should now be on_response_sent (rebuilt in send_buf path)
    CHECK(!c->keep_alive);
}

TEST(drain_proxy, upstream_status_parsed) {
    SmallLoop loop;
    loop.setup();

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    rut::u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 100));

    c->upstream_fd = 99;
    c->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::UpstreamConnect, 0));
    rut::u32 req_len = c->recv_buf.len();
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, static_cast<rut::i32>(req_len)));

    // Upstream 404 response
    c->upstream_recv_buf.reset();
    const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
    rut::u32 resp_len = 0;
    while (resp[resp_len]) resp_len++;
    rut::u8* dst = c->upstream_recv_buf.write_ptr();
    for (rut::u32 i = 0; i < resp_len; i++) dst[i] = static_cast<rut::u8>(resp[i]);
    c->upstream_recv_buf.commit(resp_len);

    IoEvent ev = make_ev(cid, IoEventType::UpstreamRecv, static_cast<rut::i32>(resp_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    rut::u32 n = loop.backend.wait(events, 8);
    for (rut::u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(c->resp_status, static_cast<rut::u16>(404));
}

// === Drain: slice pool state ===

TEST(drain_pool, all_slices_returned_after_drain) {
    SmallLoop loop;
    loop.setup();
    loop.draining = true;

    // Accept 5 connections, complete their request cycles
    rut::u32 cids[5];
    for (rut::u32 i = 0; i < 5; i++) {
        loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, static_cast<rut::i32>(100 + i)));
        auto* c = loop.find_fd(static_cast<rut::i32>(100 + i));
        REQUIRE(c != nullptr);
        cids[i] = c->id;
        loop.inject_and_dispatch(make_ev(cids[i], IoEventType::Recv, 50));
        rut::u32 send_len = loop.conns[cids[i]].send_buf.len();
        loop.inject_and_dispatch(
            make_ev(cids[i], IoEventType::Send, static_cast<rut::i32>(send_len)));
        // Connection closed by drain (keep_alive=false)
        CHECK_EQ(loop.conns[cids[i]].fd, -1);
    }

    // Sync backend: all connections freed immediately during dispatch.
    CHECK_EQ(loop.free_top, SmallLoop::kMaxConns);
    // Slice pointers cleared by reset.
    for (rut::u32 i = 0; i < 5; i++) {
        CHECK_EQ(loop.conns[cids[i]].recv_slice, nullptr);
        CHECK_EQ(loop.conns[cids[i]].send_slice, nullptr);
    }
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
