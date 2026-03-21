#include "rout/runtime/epoll_backend.h"

#include <errno.h>
#include <string.h>  // memset
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace rout {

// --- Encode/decode epoll_event.data.u64 ---
// Same layout as io_uring: [63:8] = conn_id, [7:0] = IoEventType
// Plus special sentinel for listen fd and timer fd.

static constexpr u32 kListenConnId = 0xFFFFFF;
static constexpr u32 kTimerConnId = 0xFFFFFE;

u64 EpollBackend::encode_data(u32 conn_id, IoEventType type) {
    return (static_cast<u64>(conn_id) << 8) | static_cast<u64>(type);
}

void EpollBackend::decode_data(u64 data, u32& conn_id, IoEventType& type) {
    type = static_cast<IoEventType>(data & 0xFF);
    conn_id = static_cast<u32>(data >> 8);
}

// --- Init ---

i32 EpollBackend::init(u32 /*shard_id*/, i32 lfd) {
    listen_fd = lfd;
    epoll_fd = -1;
    timer_fd = -1;
    pending_count = 0;
    for (u32 i = 0; i < kMaxFdMap; i++) fd_map[i] = -1;

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return -errno;

    // Create timerfd for 1-second ticks (drives timer wheel)
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        i32 err = errno;
        close(epoll_fd);
        epoll_fd = -1;
        return -err;
    }

    struct itimerspec ts;
    memset(&ts, 0, sizeof(ts));
    ts.it_interval.tv_sec = 1;
    ts.it_value.tv_sec = 1;
    timerfd_settime(timer_fd, 0, &ts, nullptr);

    // Register timer_fd with epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(kTimerConnId, IoEventType::Timeout);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        shutdown();
        return -errno;
    }

    return 0;
}

// --- Operations ---

void EpollBackend::add_accept() {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(kListenConnId, IoEventType::Accept);
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
}

void EpollBackend::add_recv(i32 fd, u32 conn_id) {
    if (conn_id < kMaxFdMap) fd_map[conn_id] = fd;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(conn_id, IoEventType::Recv);
    // MOD first (fd may already be registered for EPOLLOUT after send);
    // fall back to ADD if not yet registered.
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
}

void EpollBackend::add_send(i32 fd, u32 conn_id, const u8* buf, u32 len) {
    // Try immediate send first (common case: socket buffer not full)
    ssize_t nw = send(fd, buf, len, MSG_NOSIGNAL);

    if (nw == static_cast<ssize_t>(len)) {
        // Sent everything — queue synthetic completion
        if (pending_count < 64) {
            pending_completions[pending_count].conn_id = conn_id;
            pending_completions[pending_count].type = IoEventType::Send;
            pending_completions[pending_count].result = static_cast<i32>(nw);
            pending_completions[pending_count].buf_id = 0;
            pending_completions[pending_count].has_buf = 0;
            pending_count++;
        }
        return;
    }

    if (nw < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Real error — queue error completion
        if (pending_count < 64) {
            pending_completions[pending_count].conn_id = conn_id;
            pending_completions[pending_count].type = IoEventType::Send;
            pending_completions[pending_count].result = -errno;
            pending_completions[pending_count].buf_id = 0;
            pending_completions[pending_count].has_buf = 0;
            pending_count++;
        }
        return;
    }

    // Partial send or EAGAIN: register for EPOLLOUT
    // TODO: store outstanding send state (buf/len/offset) keyed by conn_id,
    // complete the send inside wait() on EPOLLOUT, then emit Send completion.
    // Current behavior emits result=0 (readiness), which callbacks treat as success.
    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.u64 = encode_data(conn_id, IoEventType::Send);
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

void EpollBackend::add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len) {
    if (conn_id < kMaxFdMap) fd_map[conn_id] = fd;
    // Initiate non-blocking connect
    i32 rc = connect(fd, static_cast<const struct sockaddr*>(addr), addr_len);
    if (rc == 0) {
        // Immediate connect (loopback) — register fd for future I/O
        struct epoll_event reg_ev;
        reg_ev.events = EPOLLIN;
        reg_ev.data.u64 = encode_data(conn_id, IoEventType::Recv);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &reg_ev);
        if (pending_count < 64) {
            pending_completions[pending_count].conn_id = conn_id;
            pending_completions[pending_count].type = IoEventType::UpstreamConnect;
            pending_completions[pending_count].result = 0;
            pending_completions[pending_count].buf_id = 0;
            pending_completions[pending_count].has_buf = 0;
            pending_count++;
        }
        return;
    }

    if (errno == EINPROGRESS) {
        // Register for EPOLLOUT — connect completion
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.u64 = encode_data(conn_id, IoEventType::UpstreamConnect);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        return;
    }

    // Connect failed immediately
    if (pending_count < 64) {
        pending_completions[pending_count].conn_id = conn_id;
        pending_completions[pending_count].type = IoEventType::UpstreamConnect;
        pending_completions[pending_count].result = -errno;
        pending_completions[pending_count].buf_id = 0;
        pending_completions[pending_count].has_buf = 0;
        pending_count++;
    }
}

void EpollBackend::cancel(i32 fd, u32 /*conn_id*/) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

// --- Wait ---

u32 EpollBackend::wait(IoEvent* events, u32 max_events) {
    u32 out = 0;

    // First, drain pending synthetic completions
    while (pending_count > 0 && out < max_events) {
        events[out] = pending_completions[--pending_count];
        out++;
    }

    // If we already have events, don't block
    i32 timeout_ms = (out > 0) ? 0 : -1;

    struct epoll_event ep_events[kMaxEventsPerWait];
    u32 max_ep = max_events - out;
    if (max_ep > kMaxEventsPerWait) max_ep = kMaxEventsPerWait;

    i32 n;
    for (;;) {
        n = epoll_wait(epoll_fd, ep_events, static_cast<i32>(max_ep), timeout_ms);
        if (n >= 0) break;
        if (errno == EINTR) continue;
        return out;  // real error, return what we have
    }

    for (i32 i = 0; i < n && out < max_events; i++) {
        u32 conn_id;
        IoEventType type;
        decode_data(ep_events[i].data.u64, conn_id, type);

        if (conn_id == kListenConnId) {
            // Accept loop (no multishot in epoll)
            for (;;) {
                i32 fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                if (out >= max_events) {
                    close(fd);  // drop if buffer full
                    break;
                }
                events[out].conn_id = 0;
                events[out].type = IoEventType::Accept;
                events[out].result = fd;
                events[out].buf_id = 0;
                events[out].has_buf = 0;
                out++;
            }
        } else if (conn_id == kTimerConnId) {
            // Timer tick — read timerfd to acknowledge
            u64 ticks;
            (void)read(timer_fd, &ticks, sizeof(ticks));
            if (out < max_events) {
                events[out].conn_id = 0;
                events[out].type = IoEventType::Timeout;
                events[out].result = static_cast<i32>(ticks);
                events[out].buf_id = 0;
                events[out].has_buf = 0;
                out++;
            }
        } else if (ep_events[i].events & EPOLLIN) {
            // Readiness → do recv now, emit completion
            // TODO: recv into Connection::recv_buf instead of stack tmp_buf so
            // callbacks (especially proxy) can access the received data.
            i32 fd = (conn_id < kMaxFdMap) ? fd_map[conn_id] : -1;
            if (fd < 0) continue;
            u8 tmp_buf[4096];
            ssize_t nr = recv(fd, tmp_buf, sizeof(tmp_buf), 0);
            events[out].conn_id = conn_id;
            events[out].type = IoEventType::Recv;
            events[out].result = static_cast<i32>(nr < 0 ? -errno : nr);
            events[out].buf_id = 0;
            events[out].has_buf = 0;
            out++;
        } else if (ep_events[i].events & EPOLLOUT) {
            if (type == IoEventType::UpstreamConnect) {
                // Connect completed — check for errors
                i32 err = 0;
                socklen_t errlen = sizeof(err);
                i32 cfd = (conn_id < kMaxFdMap) ? fd_map[conn_id] : -1;
                if (cfd >= 0) getsockopt(cfd, SOL_SOCKET, SO_ERROR, &err, &errlen);
                events[out].conn_id = conn_id;
                events[out].type = IoEventType::UpstreamConnect;
                events[out].result = err ? -err : 0;
                events[out].buf_id = 0;
                events[out].has_buf = 0;
            } else {
                // Send readiness — the upper layer should retry send
                events[out].conn_id = conn_id;
                events[out].type = IoEventType::Send;
                events[out].result = 0;  // signal: ready to send
                events[out].buf_id = 0;
                events[out].has_buf = 0;
            }
            out++;
        }
    }

    return out;
}

// --- Shutdown ---

void EpollBackend::shutdown() {
    if (timer_fd >= 0) {
        close(timer_fd);
        timer_fd = -1;
    }
    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

}  // namespace rout
