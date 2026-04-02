// Real-socket integration tests. Ported from libuv/libevent2 scenarios.
#include "rut/runtime/epoll_event_loop.h"
#include "rut/runtime/io_uring_backend.h"
#include "rut/runtime/shard.h"
#include "test.h"
#include "test_helpers.h"

// Helper: Connection with local buffer storage (for tests that use raw backends).
static constexpr u32 kTestBufSize = 4096;

struct TestConn {
    Connection conn;
    u8 recv_storage[kTestBufSize];
    u8 send_storage[kTestBufSize];

    void init(u32 id, i32 fd) {
        conn.reset();
        conn.id = id;
        conn.fd = fd;
        conn.recv_slice = recv_storage;
        conn.send_slice = send_storage;
        conn.recv_buf.bind(recv_storage, kTestBufSize);
        conn.send_buf.bind(send_storage, kTestBufSize);
    }
};

// === Socket Setup (libuv: test-tcp-bind-error, test-tcp-flags, test-tcp-reuseport) ===

TEST(socket, nonblocking) {
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    close(fd);
}

TEST(socket, reuseport) {
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    i32 val = 0;
    socklen_t len = sizeof(val);
    getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, &len);
    CHECK_EQ(val, 1);
    close(fd);
}

TEST(socket, nodelay) {
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    i32 val = 0;
    socklen_t len = sizeof(val);
    getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, &len);
    CHECK_EQ(val, 1);
    close(fd);
}

TEST(socket, two_listeners_same_port) {
    i32 fd1 = create_listen_socket(0).value_or(-1);
    REQUIRE(fd1 >= 0);
    i32 fd2 = create_listen_socket(get_port(fd1)).value_or(-1);
    CHECK(fd2 >= 0);
    if (fd2 >= 0) close(fd2);
    close(fd1);
}

TEST(socket, connect_refused) {
    i32 fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = __builtin_bswap16(19999);
    a.sin_addr.s_addr = __builtin_bswap32(0x7F000001);
    i32 rc = connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
    CHECK(rc < 0);
    CHECK(errno == EINPROGRESS || errno == ECONNREFUSED);
    close(fd);
}

TEST(socket, double_bind_error) {
    i32 fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = __builtin_bswap32(0x7F000001);
    CHECK_EQ(bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)), 0);
    a.sin_port = __builtin_bswap16(18888);
    CHECK(bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0);
    close(fd);
}

TEST(socket, double_listen_ok) {
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    CHECK_EQ(listen(fd, 4096), 0);
    close(fd);
}

// === Basic I/O (libuv: test-tcp-connect, libevent: test_simpleread/write) ===

TEST(io, simple_request_response) {
    TestServer srv;
    REQUIRE(srv.setup(10));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    char buf[4096];
    i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
    CHECK(n > 0);
    CHECK(has_200(buf, n));
    close(c);
    srv.teardown();
}

TEST(io, keepalive_3_requests) {
    TestServer srv;
    REQUIRE(srv.setup(30));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    for (int i = 0; i < 3; i++) {
        REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
        char buf[4096];
        i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
        CHECK(n > 0);
    }
    close(c);
    srv.teardown();
}

TEST(io, large_request_2kb) {
    TestServer srv;
    REQUIRE(srv.setup(20));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    char big[2048];
    memset(big, 'A', sizeof(big));
    memcpy(big, HTTP_REQ, HTTP_REQ_LEN);
    REQUIRE(send_all(c, big, sizeof(big)));
    char buf[4096];
    CHECK(recv_timeout(c, buf, sizeof(buf), 2000) > 0);
    close(c);
    srv.teardown();
}

TEST(io, zero_length_send) {
    TestServer srv;
    REQUIRE(srv.setup(10));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    send(c, "", 0, MSG_NOSIGNAL);
    REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    char buf[4096];
    CHECK(recv_timeout(c, buf, sizeof(buf), 2000) > 0);
    close(c);
    srv.teardown();
}

TEST(io, pipelining_3_requests) {
    TestServer srv;
    REQUIRE(srv.setup(30));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    for (int i = 0; i < 3; i++) REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    int responses = 0;
    for (int i = 0; i < 3; i++) {
        char buf[4096];
        if (recv_timeout(c, buf, sizeof(buf), 2000) > 0) responses++;
    }
    CHECK_GE(responses, 1);
    close(c);
    srv.teardown();
}

TEST(io, client_half_close) {
    TestServer srv;
    REQUIRE(srv.setup(20));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    shutdown(c, SHUT_WR);
    char buf[4096];
    CHECK(recv_timeout(c, buf, sizeof(buf), 2000) > 0);
    close(c);
    srv.teardown();
}

// === Concurrency (libevent: test_multiple, test-many-connections) ===

TEST(concurrent, five_clients) {
    TestServer srv;
    REQUIRE(srv.setup(50));
    i32 clients[5];
    for (int i = 0; i < 5; i++) {
        clients[i] = connect_to(srv.port);
        REQUIRE(clients[i] >= 0);
    }
    for (int i = 0; i < 5; i++) REQUIRE(send_all(clients[i], HTTP_REQ, HTTP_REQ_LEN));
    for (int i = 0; i < 5; i++) {
        char buf[4096];
        CHECK(recv_timeout(clients[i], buf, sizeof(buf), 2000) > 0);
    }
    for (int i = 0; i < 5; i++) close(clients[i]);
    srv.teardown();
}

TEST(concurrent, ten_clients_multi_req) {
    TestServer srv;
    REQUIRE(srv.setup(200));
    i32 clients[10];
    for (int i = 0; i < 10; i++) {
        clients[i] = connect_to(srv.port);
        REQUIRE(clients[i] >= 0);
    }
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 10; i++) REQUIRE(send_all(clients[i], HTTP_REQ, HTTP_REQ_LEN));
        for (int i = 0; i < 10; i++) {
            char buf[4096];
            CHECK(recv_timeout(clients[i], buf, sizeof(buf), 2000) > 0);
        }
    }
    for (int i = 0; i < 10; i++) close(clients[i]);
    srv.teardown();
}

TEST(concurrent, rapid_connect_disconnect_50) {
    TestServer srv;
    REQUIRE(srv.setup(500));
    for (int i = 0; i < 50; i++) {
        i32 c = connect_to(srv.port);
        if (c >= 0) close(c);
    }
    usleep(200000);
    srv.teardown();
}

TEST(concurrent, interleaved_io) {
    TestServer srv;
    REQUIRE(srv.setup(50));
    i32 c1 = connect_to(srv.port);
    i32 c2 = connect_to(srv.port);
    REQUIRE(c1 >= 0);
    REQUIRE(c2 >= 0);
    REQUIRE(send_all(c1, HTTP_REQ, HTTP_REQ_LEN));
    REQUIRE(send_all(c2, HTTP_REQ, HTTP_REQ_LEN));
    char buf[4096];
    CHECK(recv_timeout(c1, buf, sizeof(buf), 2000) > 0);
    CHECK(recv_timeout(c2, buf, sizeof(buf), 2000) > 0);
    close(c1);
    close(c2);
    srv.teardown();
}

// === Error Handling (libuv: test-tcp-close, test-tcp-write-fail) ===

TEST(error, client_eof) {
    TestServer srv;
    REQUIRE(srv.setup(20));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    close(c);
    usleep(100000);
    srv.teardown();
}

TEST(error, client_rst) {
    TestServer srv;
    REQUIRE(srv.setup(20));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    send_all(c, "GET", 3);
    struct linger lg = {1, 0};
    setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(c);
    usleep(100000);
    srv.teardown();
}

TEST(error, send_no_read_close) {
    TestServer srv;
    REQUIRE(srv.setup(20));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    close(c);
    usleep(100000);
    i32 c2 = connect_to(srv.port);
    REQUIRE(c2 >= 0);
    send_all(c2, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    CHECK(recv_timeout(c2, buf, sizeof(buf), 2000) > 0);
    close(c2);
    srv.teardown();
}

TEST(error, server_shutdown_with_clients) {
    TestServer srv;
    REQUIRE(srv.setup(10));
    i32 clients[3];
    for (int i = 0; i < 3; i++) {
        clients[i] = connect_to(srv.port);
        REQUIRE(clients[i] >= 0);
    }
    srv.teardown();
    for (int i = 0; i < 3; i++) {
        char buf[64];
        recv_timeout(clients[i], buf, sizeof(buf), 500);
        close(clients[i]);
    }
}

TEST(error, double_close_no_crash) {
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    close(fd);
    CHECK(close(fd) < 0);
}

TEST(error, fd_recycle_no_stale) {
    TestServer srv;
    REQUIRE(srv.setup(30));
    i32 c1 = connect_to(srv.port);
    REQUIRE(c1 >= 0);
    send_all(c1, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    recv_timeout(c1, buf, sizeof(buf), 1000);
    close(c1);
    usleep(50000);
    i32 c2 = connect_to(srv.port);
    REQUIRE(c2 >= 0);
    send_all(c2, HTTP_REQ, HTTP_REQ_LEN);
    CHECK(recv_timeout(c2, buf, sizeof(buf), 2000) > 0);
    close(c2);
    srv.teardown();
}

TEST(error, fd_leak_100_cycles) {
    TestServer srv;
    REQUIRE(srv.setup(2000));
    for (int i = 0; i < 100; i++) {
        i32 c = connect_to(srv.port);
        if (c < 0) continue;
        send_all(c, HTTP_REQ, HTTP_REQ_LEN);
        char buf[4096];
        recv_timeout(c, buf, sizeof(buf), 500);
        close(c);
    }
    srv.teardown();
}

// === Loop Control (libevent: test_loopexit) ===

TEST(loop, stop_from_outside) {
    TestServer srv;
    REQUIRE(srv.setup(1000));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    recv_timeout(c, buf, sizeof(buf), 1000);
    srv.loop->stop();
    close(c);
    srv.teardown();
}

// === Partial send (real socket) ===

// Verify EpollBackend::send_state is initialized to zero.
// After init, no connection should have outstanding send state.
TEST(partial_send, state_initialized) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    REQUIRE(loop->init(0, fd).has_value());
    // EpollBackend send_state should be zero-initialized
    for (u32 i = 0; i < EpollBackend::kMaxFdMap; i++) {
        CHECK_EQ(loop->backend.send_state[i].remaining, 0u);
    }
    loop->shutdown();
    close(fd);
    destroy_real_loop(loop);
}

// Real partial send test: fill socket buffer to force EAGAIN, then drain.
// Uses socketpair to control both ends.
// Force actual partial send via backpressure: fill the socket send buffer
// by not reading on the peer side, then drain and verify completion.
TEST(partial_send, real_epollout_completion) {
    i32 fds[2];
    REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    // Set minimal send buffer to force partial sends / EAGAIN quickly.
    // Linux doubles the value, so minimum effective is ~2*sndbuf.
    i32 sndbuf = 2048;
    REQUIRE_EQ(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);
    // Also limit recv buffer on peer to create backpressure faster
    i32 rcvbuf = 2048;
    REQUIRE_EQ(setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)), 0);

    // Use init() so timerfd is created — gives wait() a bounded 1-second wakeup
    // to prevent indefinite hangs if EPOLLOUT doesn't fire immediately.
    EpollBackend backend;
    REQUIRE(backend.init(0, -1).has_value());
    backend.downstream_fd_map[0] = fds[0];

    TestConn tc;
    tc.init(0, fds[0]);
    Connection& conn = tc.conn;

    // Fill send_buf with 4096 bytes (larger than the tiny socket buffer)
    u8 fill_data[4096];
    for (u32 j = 0; j < sizeof(fill_data); j++) fill_data[j] = static_cast<u8>(j & 0xFF);
    conn.send_buf.write(fill_data, sizeof(fill_data));

    // add_send tries immediate send — may be partial or EAGAIN
    backend.add_send(fds[0], 0, conn.send_buf.data(), conn.send_buf.len());

    // Check for immediate completion via synthetic pending events (non-blocking).
    // If add_send succeeded fully, pending_count > 0 with the completion.
    IoEvent events[16];
    bool got_full_send = false;
    if (backend.pending_count > 0) {
        u32 n = backend.wait(events, 16, &conn, 1);
        for (u32 i = 0; i < n; i++) {
            if (events[i].type == IoEventType::Send && events[i].result == 4096) {
                got_full_send = true;
            }
        }
    }

    if (!got_full_send) {
        // Partial send or EAGAIN path — drain peer to make socket writable,
        // then wait for EPOLLOUT completion. Drain fully before wait() to
        // ensure EPOLLOUT is ready (wait uses blocking epoll_wait).
        char drain[8192];
        for (int attempt = 0; attempt < 20; attempt++) {
            // Drain all available data from peer
            for (;;) {
                ssize_t nr = recv(fds[1], drain, sizeof(drain), MSG_DONTWAIT);
                if (nr <= 0) break;
            }
            usleep(1000);  // 1ms — let kernel update socket writability

            u32 n = backend.wait(events, 16, &conn, 1);
            for (u32 i = 0; i < n; i++) {
                if (events[i].type == IoEventType::Send) {
                    CHECK_EQ(events[i].result, 4096);
                    got_full_send = true;
                }
            }
            if (got_full_send) break;
        }
    }
    CHECK(got_full_send);

    close(fds[0]);
    close(fds[1]);
    backend.shutdown();
}

// === Partial send: real socket edge cases ===

// Verify EpollBackend correctly handles immediate full send on socketpair
TEST(partial_send, socketpair_full_send) {
    i32 fds[2];
    REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    EpollBackend backend;
    REQUIRE(backend.init(0, -1).has_value());
    backend.downstream_fd_map[0] = fds[0];

    TestConn tc;
    tc.init(0, fds[0]);
    Connection& conn = tc.conn;

    // Small payload — immediate full send
    conn.send_buf.write(reinterpret_cast<const u8*>("OK"), 2);
    backend.add_send(fds[0], 0, conn.send_buf.data(), conn.send_buf.len());

    IoEvent events[8];
    u32 n = backend.wait(events, 8, &conn, 1);
    bool found = false;
    for (u32 i = 0; i < n; i++) {
        if (events[i].type == IoEventType::Send) {
            CHECK_EQ(events[i].result, 2);
            found = true;
        }
    }
    CHECK(found);

    // Verify receiver got the data
    char buf[8];
    ssize_t nr = recv(fds[1], buf, sizeof(buf), 0);
    CHECK_EQ(nr, 2);
    CHECK_EQ(buf[0], 'O');
    CHECK_EQ(buf[1], 'K');

    close(fds[0]);
    close(fds[1]);
    backend.shutdown();
}

// Verify add_send error path: send to closed peer
TEST(partial_send, send_to_closed_peer) {
    i32 fds[2];
    REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    // Close receiver immediately
    close(fds[1]);

    EpollBackend backend;
    REQUIRE(backend.init(0, -1).has_value());

    TestConn tc;
    tc.init(0, fds[0]);
    Connection& conn = tc.conn;
    conn.send_buf.write(reinterpret_cast<const u8*>("data"), 4);

    // send to closed peer — may succeed (kernel buffers) or fail with EPIPE/ECONNRESET
    backend.add_send(fds[0], 0, conn.send_buf.data(), conn.send_buf.len());

    IoEvent events[8];
    u32 n = backend.wait(events, 8, &conn, 1);
    // Should get a completion (either success or error)
    bool found = false;
    for (u32 i = 0; i < n; i++) {
        if (events[i].type == IoEventType::Send) {
            found = true;
            // result is either positive (buffered) or negative (-errno)
        }
    }
    CHECK(found);

    close(fds[0]);
    backend.shutdown();
}

// Verify send_state is zeroed per connection after init
TEST(partial_send, state_zeroed_per_conn) {
    EpollBackend backend;
    REQUIRE(backend.init(0, -1).has_value());
    // Spot check several entries
    CHECK_EQ(backend.send_state[0].offset, 0u);
    CHECK_EQ(backend.send_state[0].remaining, 0u);
    CHECK_EQ(backend.send_state[100].remaining, 0u);
    CHECK_EQ(backend.send_state[EpollBackend::kMaxFdMap - 1].remaining, 0u);
    backend.shutdown();
}

// Verify EPOLLOUT with no pending send switches back to EPOLLIN (no busy loop).
// After a successful immediate send, send_state.remaining == 0.
// If a spurious EPOLLOUT fires, wait() should switch to EPOLLIN and not spin.
TEST(partial_send, epollout_no_pending_switches_to_epollin) {
    i32 fds[2];
    REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    EpollBackend backend;
    REQUIRE(backend.init(0, -1).has_value());
    backend.downstream_fd_map[0] = fds[0];

    TestConn tc;
    tc.init(0, fds[0]);
    Connection& conn = tc.conn;

    // Do a small immediate send — completes fully, send_state.remaining stays 0
    conn.send_buf.write(reinterpret_cast<const u8*>("hi"), 2);
    backend.add_send(fds[0], 0, conn.send_buf.data(), conn.send_buf.len());

    // Drain the synthetic completion
    IoEvent events[8];
    if (backend.pending_count > 0) {
        backend.wait(events, 8, &conn, 1);
    }

    // send_state should have remaining == 0 after full immediate send
    CHECK_EQ(backend.send_state[0].remaining, 0u);

    // Ensure fd is registered with epoll (add_recv registers for EPOLLIN),
    // then switch to EPOLLOUT to simulate a spurious wakeup.
    backend.add_recv(fds[0], 0);
    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.u64 = (static_cast<u64>(0) << 8) | static_cast<u64>(IoEventType::Send);
    REQUIRE_EQ(epoll_ctl(backend.epoll_fd, EPOLL_CTL_MOD, fds[0], &ev), 0);

    // wait() should see EPOLLOUT with no pending send → switch to EPOLLIN, no event
    // The timerfd will wake wait() within 1 second (bounded, no hang)
    u32 n = backend.wait(events, 8, &conn, 1);
    // Should get a Timeout event (from timerfd), not a Send event
    bool got_send = false;
    for (u32 i = 0; i < n; i++) {
        if (events[i].type == IoEventType::Send) got_send = true;
    }
    CHECK(!got_send);  // no spurious Send completion

    // Drain peer
    char buf[8];
    (void)recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);

    close(fds[0]);
    close(fds[1]);
    backend.shutdown();
}

// === io_uring backend (TODO #3 + #4) ===

// Verify IoUringBackend::init creates a timerfd
TEST(uring, init_creates_timerfd) {
    // io_uring requires Linux 6.0+. If init fails, skip gracefully.
    IoUringBackend backend;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    auto rc = backend.init(0, lfd);
    if (!rc) {
        // io_uring not available — skip
        close(lfd);
        CHECK(true);
        return;
    }
    CHECK(backend.timer_fd >= 0);
    backend.shutdown();
    close(lfd);
}

// Verify return_buffer doesn't crash (ring structure test)
TEST(uring, return_buffer_no_crash) {
    IoUringBackend backend;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    auto rc = backend.init(0, lfd);
    if (!rc) {
        close(lfd);
        CHECK(true);
        return;
    }
    // Return buffer 0 — should not crash
    backend.return_buffer(0);
    // Return buffer at boundary
    backend.return_buffer(static_cast<u16>(kProvidedBufCount - 1));
    backend.shutdown();
    close(lfd);
}

// === Shard lifecycle ===

// Shard init + shutdown without spawning a thread
TEST(shard, init_shutdown) {
    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(shard.init(0, lfd).has_value());
    CHECK(shard.loop != nullptr);
    CHECK_EQ(shard.id, 0u);
    CHECK_EQ(shard.listen_fd, lfd);
    shard.shutdown();
    CHECK(shard.loop == nullptr);
    close(lfd);
}

// Shard spawn + stop + join
TEST(shard, spawn_stop_join) {
    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn(-1).has_value());  // no CPU pinning
    CHECK(shard.thread_spawned);

    // Let the shard run briefly, then stop
    usleep(50000);  // 50ms
    shard.stop();
    shard.join();
    CHECK(!shard.thread_spawned);
    shard.shutdown();
    close(lfd);
}

// Shard handles requests while running
TEST(shard, serves_requests) {
    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    u16 port = get_port(lfd);
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn(-1).has_value());

    usleep(50000);  // let shard start
    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    char buf[4096];
    i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
    CHECK(n > 0);
    CHECK(has_200(buf, n));
    close(c);

    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
}

// Two shards on same port (SO_REUSEPORT)
TEST(shard, two_shards_same_port) {
    i32 lfd1 = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd1 >= 0);
    u16 port = get_port(lfd1);
    i32 lfd2 = create_listen_socket(port).value_or(-1);
    REQUIRE(lfd2 >= 0);

    Shard<EpollEventLoop> s1, s2;
    REQUIRE(s1.init(0, lfd1).has_value());
    REQUIRE(s2.init(1, lfd2).has_value());
    REQUIRE(s1.spawn(-1).has_value());
    REQUIRE(s2.spawn(-1).has_value());

    usleep(50000);

    // Send requests — kernel distributes across shards
    for (int i = 0; i < 5; i++) {
        i32 c = connect_to(port);
        REQUIRE(c >= 0);
        REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
        char buf[4096];
        i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
        CHECK(n > 0);
        close(c);
    }

    s1.stop();
    s2.stop();
    s1.join();
    s2.join();
    s1.shutdown();
    s2.shutdown();
    close(lfd1);
    close(lfd2);
}

// detect_cpu_count returns > 0
TEST(shard, detect_cpu_count) {
    u32 cpus = detect_cpu_count();
    CHECK(cpus > 0);
    CHECK(cpus <= 1024);  // sanity upper bound
}

// Two shards with ephemeral port (port=0) bind the same port
TEST(shard, ephemeral_port_two_shards) {
    // First shard gets ephemeral port
    i32 lfd1 = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd1 >= 0);
    u16 port = get_port(lfd1);
    CHECK(port > 0);

    // Second shard should bind the same port (SO_REUSEPORT)
    i32 lfd2 = create_listen_socket(port).value_or(-1);
    REQUIRE(lfd2 >= 0);
    CHECK_EQ(get_port(lfd2), port);

    Shard<EpollEventLoop> s1, s2;
    REQUIRE(s1.init(0, lfd1).has_value());
    REQUIRE(s2.init(1, lfd2).has_value());
    REQUIRE(s1.spawn(-1).has_value());
    REQUIRE(s2.spawn(-1).has_value());

    usleep(50000);
    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
    char buf[4096];
    CHECK(recv_timeout(c, buf, sizeof(buf), 2000) > 0);
    close(c);

    s1.stop();
    s2.stop();
    s1.join();
    s2.join();
    s1.shutdown();
    s2.shutdown();
    close(lfd1);
    close(lfd2);
}

// Shard with owns_listen_fd closes it on shutdown
TEST(shard, owns_listen_fd) {
    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(shard.init(0, lfd).has_value());
    shard.owns_listen_fd = true;
    shard.shutdown();
    // lfd should be closed now — verify by trying to close again
    CHECK(close(lfd) < 0);
}

// Shard has upstream pool after init
TEST(shard, upstream_pool_initialized) {
    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(shard.init(0, lfd).has_value());
    CHECK(shard.upstream != nullptr);
    // Pool should be fresh (all free)
    auto* c = shard.upstream->alloc();
    CHECK(c != nullptr);
    shard.upstream->free(c);
    shard.shutdown();
    close(lfd);
}

// Shard can hold a route config
TEST(shard, route_config_attached) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);
    cfg.add_upstream("api", 0x7F000001, 8080);
    cfg.add_proxy("/api/", 0, 0);

    Shard<EpollEventLoop> shard;
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(shard.init(0, lfd).has_value());
    shard.route_config = &cfg;
    CHECK(shard.route_config != nullptr);
    CHECK_EQ(shard.route_config->route_count, 2u);
    CHECK_EQ(shard.route_config->upstream_count, 1u);
    shard.shutdown();
    close(lfd);
}

// === Copilot-found: epoll add_recv EEXIST after send ===
// Real socket test: multiple keep-alive cycles exercise the
// EPOLL_CTL_MOD fallback in add_recv (fd already registered).
TEST(copilot, keepalive_5_cycles_no_eexist) {
    TestServer srv;
    REQUIRE(srv.setup(100));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    for (int i = 0; i < 5; i++) {
        REQUIRE(send_all(c, HTTP_REQ, HTTP_REQ_LEN));
        char buf[4096];
        i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
        CHECK(n > 0);
    }
    close(c);
    srv.teardown();
}

// Regression: 20 keep-alive cycles — proves MOD+ADD fallback works.
// Without it, cycle 2+ fails with EEXIST when fd is already registered.
TEST(copilot, keepalive_20_cycles_eexist_regression) {
    TestServer srv;
    REQUIRE(srv.setup(200));
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    int success = 0;
    for (int i = 0; i < 20; i++) {
        if (!send_all(c, HTTP_REQ, HTTP_REQ_LEN)) break;
        char buf[4096];
        i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
        if (n <= 0) break;
        success++;
    }
    CHECK_EQ(success, 20);
    close(c);
    srv.teardown();
}

// Regression: listen_fd must be closeable after server teardown.
// Proves the fd isn't leaked or double-closed.
TEST(copilot6, listen_fd_not_leaked) {
    i32 fd1 = create_listen_socket(0).value_or(-1);
    REQUIRE(fd1 >= 0);
    u16 port = get_port(fd1);

    // Use the fd in a server, tear it down
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    REQUIRE(loop->init(0, fd1).has_value());
    // Don't run the loop, just init + shutdown
    loop->shutdown();
    destroy_real_loop(loop);

    // fd1 should still be open (shutdown doesn't close listen_fd)
    // Close it explicitly — should succeed (not EBADF)
    CHECK_EQ(close(fd1), 0);

    // Now the port should be reusable
    i32 fd2 = create_listen_socket(port).value_or(-1);
    CHECK(fd2 >= 0);
    if (fd2 >= 0) close(fd2);
}

// Regression: keepalive_timeout must be 60 in real EventLoop after mmap+init.
// Without explicit init, mmap zeroes → keepalive_timeout=0 → instant timeout.
TEST(copilot6, real_loop_keepalive_timeout) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 fd = create_listen_socket(0).value_or(-1);
    REQUIRE(fd >= 0);
    REQUIRE(loop->init(0, fd).has_value());
    CHECK_EQ(loop->keepalive_timeout, 60u);
    loop->shutdown();
    close(fd);
    destroy_real_loop(loop);
}

// === Epoll event loop: drain, metrics, epoch ===

TEST(epoll_drain, drain_closes_idle_connections) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(loop->init(0, lfd).has_value());
    CHECK(!loop->is_draining());
    loop->drain(1);  // 1-second drain period
    CHECK(loop->is_draining());
    loop->stop();
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(epoll_drain, drain_with_active_connections) {
    TestServer srv;
    REQUIRE(srv.setup(50));
    // Connect and send request
    i32 c = connect_to(srv.port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    i32 n = recv_timeout(c, buf, sizeof(buf), 2000);
    CHECK(n > 0);
    // After response, connection is keep-alive. Now drain.
    srv.loop->drain(1);
    CHECK(srv.loop->is_draining());
    close(c);
    srv.teardown();
}

TEST(epoll_metrics, accept_and_request_counted) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(loop->init(0, lfd).has_value());
    ShardMetrics metrics{};
    loop->metrics = &metrics;
    u16 port = get_port(lfd);
    LoopThread lt = {loop, {}, 20};
    lt.start();
    usleep(10000);
    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    recv_timeout(c, buf, sizeof(buf), 2000);
    close(c);
    lt.stop();
    CHECK_GT(metrics.connections_total, 0u);
    CHECK_GT(metrics.requests_total, 0u);
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(epoll_epoch, epoch_increments_on_request) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(loop->init(0, lfd).has_value());
    ShardEpoch epoch{};
    epoch.epoch.store(0, std::memory_order_relaxed);
    loop->epoch = &epoch;
    u16 port = get_port(lfd);
    LoopThread lt = {loop, {}, 20};
    lt.start();
    usleep(10000);
    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    recv_timeout(c, buf, sizeof(buf), 2000);
    close(c);
    lt.stop();
    // Epoch should have advanced (enter + leave = +2 per request)
    CHECK_GT(epoch.epoch.load(std::memory_order_relaxed), 0u);
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(epoll_command, config_swap_via_control) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(loop->init(0, lfd).has_value());
    ShardControlBlock ctrl{};
    RouteConfig cfg{};
    const RouteConfig* cfg_ptr = nullptr;
    loop->control = &ctrl;
    loop->config_ptr = &cfg_ptr;
    ctrl.pending_config.store(&cfg, std::memory_order_relaxed);
    loop->poll_command();
    CHECK_EQ(cfg_ptr, &cfg);
    // JIT swap
    void* fake_jit = reinterpret_cast<void*>(0x1234);
    void* jit_ptr = nullptr;
    loop->jit_code_ptr = &jit_ptr;
    ctrl.pending_jit.store(fake_jit, std::memory_order_relaxed);
    loop->poll_command();
    CHECK_EQ(jit_ptr, fake_jit);
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

// === Shard lifecycle ===

TEST(shard, init_and_shutdown) {
    // Test Shard init with real event loop type
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    Shard<EpollEventLoop> shard;
    auto res = shard.init(0, lfd);
    CHECK(res.has_value());
    shard.shutdown();
    close(lfd);
}

TEST(shard, metrics_wired) {
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    Shard<EpollEventLoop> shard;
    auto res = shard.init(0, lfd);
    REQUIRE(res.has_value());
    CHECK(shard.loop->metrics != nullptr);
    CHECK_EQ(shard.loop->metrics->connections_total, 0u);
    shard.shutdown();
    close(lfd);
}

TEST(epoll_drain, drain_completes_naturally) {
    // Start loop, connect a client, drain, let it complete
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    REQUIRE(loop->init(0, lfd).has_value());
    u16 port = get_port(lfd);

    // Start loop in thread with enough iterations to complete drain
    LoopThread lt = {loop, {}, 100};
    lt.start();
    usleep(10000);

    // Connect and send a request (creates active connection)
    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, HTTP_REQ, HTTP_REQ_LEN);
    char buf[4096];
    recv_timeout(c, buf, sizeof(buf), 2000);

    // Start drain with 1-second period
    loop->drain(1);
    CHECK(loop->is_draining());

    // Close client to allow drain to complete (no active connections)
    close(c);
    usleep(500000);  // wait for drain

    lt.stop();
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard, access_log_init) {
    i32 lfd = create_listen_socket(0).value_or(-1);
    REQUIRE(lfd >= 0);
    Shard<EpollEventLoop> shard;
    auto res = shard.init(0, lfd);
    REQUIRE(res.has_value());
    // Access log is lazily allocated
    CHECK(shard.loop->access_log == nullptr);
    auto log_res = shard.init_access_log();
    CHECK(log_res.has_value());
    CHECK(shard.loop->access_log != nullptr);
    shard.shutdown();
    close(lfd);
}

// === Route matching via real sockets (covers EpollEventLoop instantiation) ===

TEST(route, static_200_real_socket) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);
    cfg.add_static("/", 0, 404);
    const RouteConfig* active = &cfg;

    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    u16 port = get_port(lfd);
    REQUIRE(loop->init(0, lfd).has_value());
    loop->config_ptr = &active;
    LoopThread lt = {loop, {}, 20};
    lt.start();

    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 33);
    char buf[1024];
    i32 n = recv_timeout(c, buf, sizeof(buf), 500);
    CHECK_GT(n, 0);
    CHECK(has_200(buf, n));
    close(c);
    lt.stop();
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(route, static_404_real_socket) {
    RouteConfig cfg;
    cfg.add_static("/", 0, 404);
    const RouteConfig* active = &cfg;

    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    u16 port = get_port(lfd);
    REQUIRE(loop->init(0, lfd).has_value());
    loop->config_ptr = &active;
    LoopThread lt = {loop, {}, 20};
    lt.start();

    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 35);
    char buf[1024];
    i32 n = recv_timeout(c, buf, sizeof(buf), 500);
    CHECK_GT(n, 0);
    // Should contain "404"
    bool found_404 = false;
    for (i32 i = 0; i < n - 2; i++) {
        if (buf[i] == '4' && buf[i + 1] == '0' && buf[i + 2] == '4') {
            found_404 = true;
            break;
        }
    }
    CHECK(found_404);
    close(c);
    lt.stop();
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(route, multiple_status_codes_real_socket) {
    RouteConfig cfg;
    cfg.add_static("/e201", 0, 201);
    cfg.add_static("/e204", 0, 204);
    cfg.add_static("/e301", 0, 301);
    cfg.add_static("/e403", 0, 403);
    cfg.add_static("/e500", 0, 500);
    const RouteConfig* active = &cfg;

    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    u16 port = get_port(lfd);
    REQUIRE(loop->init(0, lfd).has_value());
    loop->config_ptr = &active;
    LoopThread lt = {loop, {}, 50};
    lt.start();

    struct {
        const char* req;
        u32 req_len;
        const char* expect;
    } cases[] = {
        {"GET /e201 HTTP/1.1\r\nHost: x\r\n\r\n", 31, "201"},
        {"GET /e204 HTTP/1.1\r\nHost: x\r\n\r\n", 31, "204"},
        {"GET /e301 HTTP/1.1\r\nHost: x\r\n\r\n", 31, "301"},
        {"GET /e403 HTTP/1.1\r\nHost: x\r\n\r\n", 31, "403"},
        {"GET /e500 HTTP/1.1\r\nHost: x\r\n\r\n", 31, "500"},
    };
    for (auto& tc : cases) {
        i32 c = connect_to(port);
        REQUIRE(c >= 0);
        send_all(c, tc.req, tc.req_len);
        char buf[1024];
        i32 n = recv_timeout(c, buf, sizeof(buf), 500);
        CHECK_GT(n, 0);
        bool found = false;
        for (i32 i = 0; i < n - 2; i++) {
            if (buf[i] == tc.expect[0] && buf[i + 1] == tc.expect[1] &&
                buf[i + 2] == tc.expect[2]) {
                found = true;
                break;
            }
        }
        CHECK(found);
        close(c);
    }
    lt.stop();
    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(route, capture_real_socket) {
    RouteConfig cfg;
    cfg.add_static("/", 0, 200);
    const RouteConfig* active = &cfg;
    CaptureRing ring;
    ring.init();

    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    u16 port = get_port(lfd);
    REQUIRE(loop->init(0, lfd).has_value());
    loop->config_ptr = &active;
    loop->set_capture(&ring);
    LoopThread lt = {loop, {}, 20};
    lt.start();

    i32 c = connect_to(port);
    REQUIRE(c >= 0);
    send_all(c, "GET /cap HTTP/1.1\r\nHost: x\r\n\r\n", 31);
    char buf[1024];
    i32 n = recv_timeout(c, buf, sizeof(buf), 500);
    CHECK_GT(n, 0);
    close(c);

    lt.stop();
    // Small delay for request completion
    usleep(5000);
    CHECK_EQ(ring.available(), 1u);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
