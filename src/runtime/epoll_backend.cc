#include "rut/runtime/epoll_backend.h"

#include "core/expected.h"
#include "rut/runtime/error.h"

#include <errno.h>
#include <string.h>  // memset
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace rut {

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

core::Expected<void, Error> EpollBackend::init(u32 /*shard_id*/, i32 lfd) {
    listen_fd = lfd;
    epoll_fd = -1;
    timer_fd = -1;
    pending_count = 0;
    for (u32 i = 0; i < kMaxFdMap; i++) {
        fd_map[i] = -1;
        send_state[i] = {nullptr, -1, 0, 0};
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) return core::make_unexpected(Error::from_errno(Error::Source::Epoll));

    // Create timerfd for 1-second ticks (drives timer wheel)
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        i32 err = errno;
        close(epoll_fd);
        epoll_fd = -1;
        return core::make_unexpected(Error::make(err, Error::Source::Timerfd));
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
        i32 err = errno;
        shutdown();
        return core::make_unexpected(Error::make(err, Error::Source::Epoll));
    }

    return {};
}

// --- Operations ---

void EpollBackend::add_accept() {
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(kListenConnId, IoEventType::Accept);
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);
}

bool EpollBackend::add_recv(i32 fd, u32 conn_id) {
    if (conn_id < kMaxFdMap) fd_map[conn_id] = fd;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(conn_id, IoEventType::Recv);
    // MOD first (fd may already be registered for EPOLLOUT after send);
    // fall back to ADD if not yet registered.
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
    return true;
}

bool EpollBackend::add_send_upstream(i32 fd, u32 conn_id, const u8* buf, u32 len) {
    return add_send(fd, conn_id, buf, len);
}

bool EpollBackend::add_recv_upstream(i32 fd, u32 conn_id) {
    if (conn_id < kMaxFdMap) fd_map[conn_id] = fd;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = encode_data(conn_id, IoEventType::UpstreamRecv);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
    return true;
}

bool EpollBackend::add_send(i32 fd, u32 conn_id, const u8* buf, u32 len) {
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
        return true;
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
        return true;
    }

    // Partial send or EAGAIN: track remaining bytes, register for EPOLLOUT.
    // wait() will complete the send on writability and emit the real byte count.
    // Store the original source pointer and fd — the source may be recv_buf (proxy),
    // not always send_buf, and fd may be upstream_fd.
    u32 sent = (nw > 0) ? static_cast<u32>(nw) : 0;
    if (conn_id < kMaxFdMap) {
        send_state[conn_id].src = buf;
        send_state[conn_id].fd = fd;
        send_state[conn_id].offset = sent;
        send_state[conn_id].remaining = len - sent;
    }
    struct epoll_event ev;
    ev.events = EPOLLOUT;
    ev.data.u64 = encode_data(conn_id, IoEventType::Send);
    i32 rc = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    if (rc < 0 && errno == ENOENT) {
        rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
    if (rc < 0) {
        // epoll registration failed — clear send_state and emit error completion.
        // Error must always be delivered (no silent drop) to prevent stuck connections.
        i32 err = errno;
        if (conn_id < kMaxFdMap) {
            send_state[conn_id] = {nullptr, -1, 0, 0};
        }
        if (pending_count >= 64) pending_count = 63;  // evict oldest to guarantee space
        pending_completions[pending_count].conn_id = conn_id;
        pending_completions[pending_count].type = IoEventType::Send;
        pending_completions[pending_count].result = -err;
        pending_completions[pending_count].buf_id = 0;
        pending_completions[pending_count].has_buf = 0;
        pending_count++;
    }
    return true;
}

bool EpollBackend::add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len) {
    if (conn_id < kMaxFdMap) fd_map[conn_id] = fd;
    // Initiate non-blocking connect
    i32 rc = connect(fd, static_cast<const struct sockaddr*>(addr), addr_len);
    if (rc == 0) {
        // Immediate connect (loopback) — register upstream fd for recv.
        // Use UpstreamRecv so on_upstream_response accepts the event.
        struct epoll_event reg_ev;
        reg_ev.events = EPOLLIN;
        reg_ev.data.u64 = encode_data(conn_id, IoEventType::UpstreamRecv);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &reg_ev);
        if (pending_count < 64) {
            pending_completions[pending_count].conn_id = conn_id;
            pending_completions[pending_count].type = IoEventType::UpstreamConnect;
            pending_completions[pending_count].result = 0;
            pending_completions[pending_count].buf_id = 0;
            pending_completions[pending_count].has_buf = 0;
            pending_count++;
        }
        return true;
    }

    if (errno == EINPROGRESS) {
        // Register for EPOLLOUT — connect completion
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.u64 = encode_data(conn_id, IoEventType::UpstreamConnect);
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        return true;
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
    return true;
}

u32 EpollBackend::cancel(i32 fd,
                         u32 /*conn_id*/,
                         bool /*recv_armed*/,
                         bool /*send_armed*/,
                         bool /*upstream_recv_armed*/,
                         bool /*upstream_send_armed*/,
                         bool /*has_upstream*/) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    return 0;  // sync backend: no cancel CQEs to track
}

void EpollBackend::cancel_accept() {
    if (listen_fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, listen_fd, nullptr);
    }
}

// --- Wait ---

u32 EpollBackend::wait(IoEvent* events, u32 max_events, Connection* conns, u32 max_conns) {
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
            u64 ticks = 0;
            ssize_t rr = read(timer_fd, &ticks, sizeof(ticks));
            if (rr != static_cast<ssize_t>(sizeof(ticks))) continue;  // short/error read
            if (out < max_events) {
                events[out].conn_id = 0;
                events[out].type = IoEventType::Timeout;
                // Clamp ticks to avoid i32 overflow on long stalls
                events[out].result = (ticks > 0x7FFFFFFF) ? 0x7FFFFFFF : static_cast<i32>(ticks);
                events[out].buf_id = 0;
                events[out].has_buf = 0;
                out++;
            }
        } else if (ep_events[i].events & EPOLLIN) {
            // Readiness → recv directly into Connection::recv_buf
            i32 fd = (conn_id < kMaxFdMap) ? fd_map[conn_id] : -1;
            if (fd < 0) continue;
            // Append recv data into the appropriate recv buffer.
            // UpstreamRecv → upstream_recv_buf; Recv → recv_buf.
            // Buffer is NOT reset here — callback resets when it consumes data.
            // This allows multi-packet requests to accumulate.
            i32 result = 0;
            if (conn_id < max_conns) {
                auto& buf = (type == IoEventType::UpstreamRecv) ? conns[conn_id].upstream_recv_buf
                                                                : conns[conn_id].recv_buf;
                u32 avail = buf.write_avail();
                if (avail > 0) {
                    ssize_t nr;
                    do {
                        nr = recv(fd, buf.write_ptr(), avail, 0);
                    } while (nr < 0 && errno == EINTR);
                    if (nr > 0) {
                        buf.commit(static_cast<u32>(nr));
                        result = static_cast<i32>(nr);
                    } else if (nr == 0) {
                        result = 0;  // EOF
                    } else {
                        result = -errno;
                    }
                } else {
                    result = -ENOBUFS;  // recv buffer full
                }
            } else {
                result = -EINVAL;  // invalid conn_id
            }
            events[out].conn_id = conn_id;
            events[out].type = type;  // Recv or UpstreamRecv
            events[out].result = result;
            events[out].buf_id = 0;
            events[out].has_buf = 0;
            events[out].more = 0;
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
                // EPOLLOUT: complete the outstanding partial send.
                // Source pointer and fd are stored in send_state (set by add_send),
                // not read from Connection — the source may be recv_buf (proxy).
                if (conn_id >= kMaxFdMap) continue;
                auto& ss = send_state[conn_id];
                if (ss.remaining == 0 || !ss.src || ss.fd < 0) {
                    // No outstanding send — switch the fd that was registered
                    // for EPOLLOUT back to EPOLLIN. Use fd_map (the client fd
                    // for this conn_id) since ss.fd may be stale/cleared.
                    i32 rfd = (conn_id < kMaxFdMap) ? fd_map[conn_id] : -1;
                    if (rfd >= 0) {
                        struct epoll_event rev;
                        rev.events = EPOLLIN;
                        rev.data.u64 = encode_data(conn_id, IoEventType::Recv);
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, rfd, &rev);
                    }
                    continue;
                }
                const u8* src = ss.src;
                bool done = false;
                bool eagain = false;
                // Loop to send remaining bytes (socket may accept partial again)
                while (ss.remaining > 0) {
                    ssize_t nw;
                    do {
                        nw = send(ss.fd, src + ss.offset, ss.remaining, MSG_NOSIGNAL);
                    } while (nw < 0 && errno == EINTR);
                    if (nw > 0) {
                        ss.offset += static_cast<u32>(nw);
                        ss.remaining -= static_cast<u32>(nw);
                    } else if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // Still can't send — stay registered for EPOLLOUT, don't emit event
                        eagain = true;
                        break;
                    } else {
                        // Error or nw==0 — emit error completion (nw==0 treated as EPIPE)
                        events[out].conn_id = conn_id;
                        events[out].type = IoEventType::Send;
                        events[out].result = (nw < 0) ? -errno : -EPIPE;
                        events[out].buf_id = 0;
                        events[out].has_buf = 0;
                        ss.remaining = 0;
                        done = true;
                        break;
                    }
                }
                if (eagain) continue;  // no event, retry on next EPOLLOUT
                if (!done) {
                    // All bytes sent — emit completion with total byte count
                    events[out].conn_id = conn_id;
                    events[out].type = IoEventType::Send;
                    events[out].result = static_cast<i32>(ss.offset);
                    events[out].buf_id = 0;
                    events[out].has_buf = 0;
                }
                // Send finished (success or error) — clear state and switch
                // the fd back to EPOLLIN immediately (don't wait for spurious EPOLLOUT).
                i32 send_fd = ss.fd;
                ss = {nullptr, -1, 0, 0};
                if (send_fd >= 0) {
                    struct epoll_event rev;
                    rev.events = EPOLLIN;
                    rev.data.u64 = encode_data(conn_id, IoEventType::Recv);
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, send_fd, &rev);
                }
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

}  // namespace rut
