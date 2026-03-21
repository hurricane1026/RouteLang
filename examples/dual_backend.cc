// Dual-backend example: same callbacks, io_uring or epoll selected at compile time.
//
// Shows:
//   1. Callback layer is 100% backend-agnostic
//   2. Backend difference is inside wait() only
//   3. Zero virtual dispatch (template)

#include <stdint.h>
#include <unistd.h>

using u8 = uint8_t; using u16 = uint16_t;
using u32 = uint32_t; using i32 = int32_t; using u64 = uint64_t;

// ---- Unified completion event ----

struct IoEvent {
    u32 conn_id;
    i32 result;
    u16 buf_id;    // io_uring: provided buffer id. epoll: 0
};

// ---- Forward decls ----

template<typename Backend> struct EventLoop;
struct Connection;

using Callback = void (*)(void* loop, Connection&, IoEvent);

struct Connection {
    Callback on_complete;
    i32  fd;
    u32  id;
    i32  upstream_fd;
    bool keep_alive;
    u8   recv_buf[4096];
    u32  recv_len;
    u8   send_buf[4096];
    u32  send_len;
};

// ---- io_uring backend (sketch) ----

struct IoUringBackend {
    i32 ring_fd = -1;
    i32 listen_fd = -1;
    Connection* conns = nullptr;

    i32 init(u32, i32 lfd, Connection* c) {
        listen_fd = lfd; conns = c;
        // real: io_uring_setup, mmap rings, setup provided buffer ring
        return 0;
    }

    void add_accept() {
        // real: IORING_OP_ACCEPT + IORING_ACCEPT_MULTISHOT
    }

    void add_recv(Connection& c) {
        // real: IORING_OP_RECV + IORING_RECV_MULTISHOT + IOSQE_BUFFER_SELECT
        // No buffer needed — kernel picks from provided buffer ring
        (void)c;
    }

    void add_send(Connection& c) {
        // real: IORING_OP_SEND (or SEND_ZC)
        (void)c;
    }

    void add_connect(Connection& c, const void*, u32) {
        // real: IORING_OP_CONNECT
        (void)c;
    }

    u32 wait(IoEvent* events, u32 max) {
        // real: io_uring_enter → harvest CQEs
        //
        // CQE arrives with:
        //   - user_data → conn_id
        //   - res → bytes transferred (or -errno)
        //   - flags → IORING_CQE_F_BUFFER → buf_id
        //
        // Just copy into IoEvent. I/O is ALREADY DONE.
        // No recv()/send() call here — kernel did it.
        (void)events; (void)max;
        return 0;
    }

    void shutdown() {}
};

// ---- epoll backend (sketch) ----

struct EpollBackend {
    i32 epoll_fd = -1;
    i32 listen_fd = -1;
    Connection* conns = nullptr;

    i32 init(u32, i32 lfd, Connection* c) {
        listen_fd = lfd; conns = c;
        // real: epoll_create1, timerfd_create
        return 0;
    }

    void add_accept() {
        // real: epoll_ctl(EPOLL_CTL_ADD, listen_fd, EPOLLIN)
    }

    void add_recv(Connection& c) {
        // real: epoll_ctl(EPOLL_CTL_ADD, c.fd, EPOLLIN)
        // NOTE: no buffer here. recv() happens inside wait().
        (void)c;
    }

    void add_send(Connection& c) {
        // real: try send() immediately.
        //       if complete → queue synthetic IoEvent.
        //       if EAGAIN  → epoll_ctl(EPOLL_CTL_MOD, EPOLLOUT)
        (void)c;
    }

    void add_connect(Connection& c, const void*, u32) {
        // real: connect() + epoll_ctl(EPOLLOUT) if EINPROGRESS
        (void)c;
    }

    u32 wait(IoEvent* events, u32 max) {
        // real:
        //   epoll_wait → get ready fds
        //   for each ready fd:
        //     if EPOLLIN on listen_fd → accept4() loop → emit Accept events
        //     if EPOLLIN on conn_fd  → recv() HERE → emit Recv events
        //     if EPOLLOUT            → emit Send/Connect events
        //
        // KEY DIFFERENCE: epoll does recv/send INSIDE wait().
        // io_uring's wait() just harvests completions — I/O already done.
        //
        // But both produce the same IoEvent output.
        (void)events; (void)max;
        return 0;
    }

    void shutdown() {}
};

// ---- EventLoop: template on backend, same for both ----

template<typename Backend>
struct EventLoop {
    Backend backend;
    Connection conns[1024];
    bool running = true;

    i32 init(u32 shard_id, i32 listen_fd) {
        return backend.init(shard_id, listen_fd, conns);
    }

    // The entire event loop
    void run() {
        backend.add_accept();
        IoEvent events[256];

        while (running) {
            u32 n = backend.wait(events, 256);
            for (u32 i = 0; i < n; i++) {
                auto& conn = conns[events[i].conn_id];
                // One indirect call. Callback doesn't know which backend.
                conn.on_complete(this, conn, events[i]);
            }
        }
    }

    // Helpers called by callbacks (thin wrappers over backend)
    void submit_recv(Connection& c)    { backend.add_recv(c); }
    void submit_send(Connection& c)    { backend.add_send(c); }
    void close_conn(Connection& c)     { c.fd = -1; c.on_complete = nullptr; }
    void submit_connect(Connection& c, const void* a, u32 l) { backend.add_connect(c, a, l); }
};

// ---- Callbacks: 100% backend-agnostic ----
// They call loop->submit_recv/send, which forwards to whichever backend.
// They never touch io_uring or epoll directly.

static void on_response_sent(void* lp, Connection& conn, IoEvent ev);

static void on_header_received(void* lp, Connection& conn, IoEvent ev) {
    // Works identically whether ev came from io_uring CQE or epoll recv()
    if (ev.result <= 0) return;
    conn.recv_len = static_cast<u32>(ev.result);
    // parse, route, decide upstream...

    conn.on_complete = on_response_sent;
    // loop type erased through void* — in real code use template or static dispatch
    (void)lp;
}

static void on_response_sent(void* lp, Connection& conn, IoEvent ev) {
    if (ev.result < 0 || !conn.keep_alive) return;
    conn.on_complete = on_header_received;
    (void)lp; (void)ev;
}

// ---- Startup: pick backend once ----

static void write_str(const char* s) {
    int len = 0; while (s[len]) len++;
    (void)write(1, s, len);
}

static bool detect_io_uring() {
    // real: try io_uring_setup syscall. Success = use io_uring.
    return false;  // simulate no io_uring
}

int main() {
    // Backend selected once at startup.
    // Two code paths only HERE — everywhere else is generic.
    if (detect_io_uring()) {
        write_str("Using io_uring backend\n");
        EventLoop<IoUringBackend> loop;
        loop.init(0, -1);
        // loop.run();
    } else {
        write_str("Using epoll backend\n");
        EventLoop<EpollBackend> loop;
        loop.init(0, -1);
        // loop.run();
    }

    write_str("Callbacks are identical for both backends.\n");
    return 0;
}
