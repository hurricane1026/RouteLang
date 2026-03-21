// Real-socket integration tests. Ported from libuv/libevent2 scenarios.
#include "test.h"
#include "test_helpers.h"

// === Socket Setup (libuv: test-tcp-bind-error, test-tcp-flags, test-tcp-reuseport) ===

TEST(socket, nonblocking) {
    i32 fd = create_listen_socket(0);
    REQUIRE(fd >= 0);
    CHECK(fcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    close(fd);
}

TEST(socket, reuseport) {
    i32 fd = create_listen_socket(0);
    REQUIRE(fd >= 0);
    i32 val = 0;
    socklen_t len = sizeof(val);
    getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &val, &len);
    CHECK_EQ(val, 1);
    close(fd);
}

TEST(socket, nodelay) {
    i32 fd = create_listen_socket(0);
    REQUIRE(fd >= 0);
    i32 val = 0;
    socklen_t len = sizeof(val);
    getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, &len);
    CHECK_EQ(val, 1);
    close(fd);
}

TEST(socket, two_listeners_same_port) {
    i32 fd1 = create_listen_socket(0);
    REQUIRE(fd1 >= 0);
    i32 fd2 = create_listen_socket(get_port(fd1));
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
    i32 fd = create_listen_socket(0);
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
    i32 fd = create_listen_socket(0);
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
    srv.loop->running = false;
    close(c);
    srv.teardown();
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
    i32 fd1 = create_listen_socket(0);
    REQUIRE(fd1 >= 0);
    u16 port = get_port(fd1);

    // Use the fd in a server, tear it down
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    REQUIRE_EQ(loop->init(0, fd1), 0);
    // Don't run the loop, just init + shutdown
    loop->shutdown();
    destroy_real_loop(loop);

    // fd1 should still be open (shutdown doesn't close listen_fd)
    // Close it explicitly — should succeed (not EBADF)
    CHECK_EQ(close(fd1), 0);

    // Now the port should be reusable
    i32 fd2 = create_listen_socket(port);
    CHECK(fd2 >= 0);
    if (fd2 >= 0) close(fd2);
}

// Regression: keepalive_timeout must be 60 in real EventLoop after mmap+init.
// Without explicit init, mmap zeroes → keepalive_timeout=0 → instant timeout.
TEST(copilot6, real_loop_keepalive_timeout) {
    auto* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    i32 fd = create_listen_socket(0);
    REQUIRE(fd >= 0);
    REQUIRE_EQ(loop->init(0, fd), 0);
    CHECK_EQ(loop->keepalive_timeout, 60u);
    loop->shutdown();
    close(fd);
    destroy_real_loop(loop);
}

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}
