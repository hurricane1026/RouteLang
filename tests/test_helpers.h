#pragma once

// Shared test infrastructure — mock event loop, real socket helpers.

#include "rout/runtime/callbacks.h"
#include "rout/runtime/connection.h"
#include "rout/runtime/epoll_backend.h"
#include "rout/runtime/event_loop.h"
#include "rout/runtime/io_event.h"
#include "rout/runtime/socket.h"
#include "rout/runtime/timer_wheel.h"

#include "mock_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace rout;

// ---- Mock event loop (64 conns, for unit tests) ----

struct SmallLoop : EventLoopCRTP<SmallLoop> {
    MockBackend backend;
    TimerWheel timer;
    u32 shard_id = 0;
    bool running = true;

    static constexpr u32 kMaxConns = 64;
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top = 0;
    u32 keepalive_timeout = 60;

    void setup() {
        running = true;
        keepalive_timeout = 60;
        free_top = kMaxConns;
        timer.init();
        for (u32 i = 0; i < kMaxConns; i++) {
            conns[i].reset();
            conns[i].id = i;
            free_stack[i] = i;
        }
        backend.init(0, -1);
    }

    Connection* alloc_conn_impl() {
        if (free_top == 0) return nullptr;
        u32 id = free_stack[--free_top];
        conns[id].reset();
        conns[id].id = id;
        return &conns[id];
    }
    void free_conn_impl(Connection& c) {
        u32 cid = c.id;
        timer.remove(&c);
        c.reset();
        free_stack[free_top++] = cid;
    }
    void submit_recv_impl(Connection& c) { backend.add_recv(c.fd, c.id); }
    void submit_send_impl(Connection& c, const u8* buf, u32 len) {
        backend.add_send(c.fd, c.id, buf, len);
    }
    void submit_send_upstream_impl(Connection& c, const u8* buf, u32 len) {
        backend.add_send(c.upstream_fd, c.id, buf, len);
    }
    void submit_recv_upstream_impl(Connection& c) { backend.add_recv(c.upstream_fd, c.id); }
    void submit_connect_impl(Connection& c, const void* addr, u32 addr_len) {
        backend.add_connect(c.upstream_fd, c.id, addr, addr_len);
    }
    void close_conn_impl(Connection& c) {
        c.fd = -1;
        c.upstream_fd = -1;
        this->free_conn(c);
    }

    // Inject + dispatch convenience.
    // For Recv events with result>0, simulates what a real backend does:
    // append mock data into the connection's recv_buf (backend no longer resets).
    // Syncs ev.result to actual committed bytes, or -ENOBUFS if buffer full.
    void inject_and_dispatch(IoEvent ev) {
        if (ev.type == IoEventType::Recv && ev.result > 0 && ev.conn_id < kMaxConns) {
            auto& buf = conns[ev.conn_id].recv_buf;
            u32 n = static_cast<u32>(ev.result);
            u32 avail = buf.write_avail();
            if (avail == 0) {
                ev.result = -ENOBUFS;
            } else {
                if (n > avail) n = avail;
                // Write deterministic mock bytes before commit so data()-based
                // tests see meaningful content (repeating 0x00..0xFF pattern).
                u8* dst = buf.write_ptr();
                for (u32 j = 0; j < n; j++) dst[j] = static_cast<u8>(j & 0xFF);
                buf.commit(n);
                ev.result = static_cast<i32>(n);
            }
        }
        backend.inject(ev);
        IoEvent events[8];
        u32 n = backend.wait(events, 8);
        for (u32 i = 0; i < n; i++) dispatch(events[i]);
    }

    void dispatch(const IoEvent& ev) {
        if (ev.type == IoEventType::Accept) {
            if (ev.result < 0) return;
            Connection* c = this->alloc_conn();
            if (!c) return;
            c->fd = ev.result;
            c->state = ConnState::ReadingHeader;
            c->on_complete = &on_header_received<SmallLoop>;
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

    // Find connection by fd
    Connection* find_fd(i32 fd) {
        for (u32 i = 0; i < kMaxConns; i++)
            if (conns[i].fd == fd) return &conns[i];
        return nullptr;
    }
};

inline IoEvent make_ev(u32 conn_id, IoEventType type, i32 result, u16 buf_id = 0, u8 has_buf = 0) {
    return {conn_id, result, buf_id, has_buf, type};
}

// ---- Real socket helpers ----

using RealLoop = EventLoop<EpollBackend>;

inline RealLoop* create_real_loop() {
    void* p =
        mmap(nullptr, sizeof(RealLoop), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<RealLoop*>(p);
}

inline void destroy_real_loop(RealLoop* l) {
    if (l) munmap(l, sizeof(RealLoop));
}

inline u16 get_port(i32 fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &l);
    return __builtin_bswap16(a.sin_port);
}

inline i32 connect_to(u16 port) {
    i32 fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = __builtin_bswap16(port);
    a.sin_addr.s_addr = __builtin_bswap32(0x7F000001);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline bool send_all(i32 fd, const char* d, u32 len) {
    u32 sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, d + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<u32>(n);
    }
    return true;
}

inline i32 recv_timeout(i32 fd, char* buf, u32 len, i32 ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = static_cast<long>(ms % 1000) * 1000L;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(fd, buf, len, 0);
    return static_cast<i32>(n < 0 ? -errno : n);
}

inline bool has_200(const char* buf, i32 n) {
    for (i32 i = 0; i < n - 2; i++)
        if (buf[i] == '2' && buf[i + 1] == '0' && buf[i + 2] == '0') return true;
    return false;
}

struct LoopThread {
    RealLoop* loop;
    pthread_t thread;
    i32 max_iters;
    static void* run(void* arg) {
        auto* lt = static_cast<LoopThread*>(arg);
        auto* lp = lt->loop;
        lp->backend.add_accept();
        IoEvent events[256];
        i32 iters = 0;
        while (lp->running) {
            u32 n = lp->backend.wait(events, 256, lp->conns, RealLoop::kMaxConns);
            for (u32 i = 0; i < n; i++) lp->dispatch(events[i]);
            if (++iters >= lt->max_iters) break;
        }
        return nullptr;
    }
    void start() { pthread_create(&thread, nullptr, run, this); }
    void stop() {
        loop->running = false;
        pthread_join(thread, nullptr);
    }
};

struct TestServer {
    RealLoop* loop = nullptr;
    i32 listen_fd = -1;
    u16 port = 0;
    LoopThread lt{};

    bool setup(i32 iters) {
        loop = create_real_loop();
        if (!loop) return false;
        listen_fd = create_listen_socket(0);
        if (listen_fd < 0) {
            destroy_real_loop(loop);
            return false;
        }
        port = get_port(listen_fd);
        if (loop->init(0, listen_fd) < 0) {
            close(listen_fd);
            destroy_real_loop(loop);
            return false;
        }
        lt = {loop, {}, iters};
        lt.start();
        return true;
    }
    void teardown() {
        lt.stop();
        loop->shutdown();
        close(listen_fd);
        destroy_real_loop(loop);
    }
};

#define HTTP_REQ "GET / HTTP/1.1\r\nHost: x\r\n\r\n"
#define HTTP_REQ_LEN 27
