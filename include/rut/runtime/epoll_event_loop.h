#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/access_log.h"
#include "rut/runtime/callbacks.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/drain.h"
#include "rut/runtime/epoll_backend.h"
#include "rut/runtime/error.h"
#include "rut/runtime/event_loop.h"
#include "rut/runtime/io_backend.h"
#include "rut/runtime/io_event.h"
#include "rut/runtime/metrics.h"
#include "rut/runtime/shard_control.h"
#include "rut/runtime/slice_pool.h"
#include "rut/runtime/timer_wheel.h"
#include <atomic>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace rut {

// EpollEventLoop — concrete, non-template event loop for epoll backend.
//
// Epoll is synchronous: the kernel is done with user buffers when
// recv/send returns. No deferred reclamation, no pending_ops tracking,
// no armed flags, no cancel SQEs.
struct EpollEventLoop : EventLoopCRTP<EpollEventLoop> {
    EpollBackend backend;
    TimerWheel timer;
    u32 shard_id;

private:
    std::atomic<bool> running_;
    std::atomic<bool> draining_;
    std::atomic<u64> drain_start_;
    std::atomic<u32> drain_period_;

public:
    static constexpr u32 kMaxConns = 16384;
    static constexpr u32 kDefaultKeepaliveTimeout = 60;
    SlicePool pool;
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top;

    u32 keepalive_timeout = kDefaultKeepaliveTimeout;
    i32 listen_fd = -1;

    AccessLogRing* access_log = nullptr;
    ShardMetrics* metrics = nullptr;

    const RouteConfig** config_ptr = nullptr;
    ShardControlBlock* control = nullptr;
    ShardEpoch* epoch = nullptr;
    void** jit_code_ptr = nullptr;

    core::Expected<void, Error> init(u32 id, i32 lfd, u32 pool_prealloc = 0) {
        shard_id = id;
        listen_fd = lfd;
        running_.store(true, std::memory_order_relaxed);
        draining_.store(false, std::memory_order_relaxed);
        drain_start_.store(0, std::memory_order_relaxed);
        drain_period_.store(0, std::memory_order_relaxed);
        keepalive_timeout = kDefaultKeepaliveTimeout;
        config_ptr = nullptr;
        control = nullptr;
        epoch = nullptr;
        jit_code_ptr = nullptr;
        free_top = kMaxConns;
        timer.init();
        for (u32 i = 0; i < kMaxConns; i++) {
            conns[i].reset();
            conns[i].id = i;
            conns[i].shard_id = static_cast<u8>(id);
            free_stack[i] = i;
        }
        TRY_VOID(pool.init(kMaxConns * 2, pool_prealloc));
        auto be = backend.init(id, lfd);
        if (!be) {
            pool.destroy();
            return core::make_unexpected(be.error());
        }
        return {};
    }

    void run() {
        backend.add_accept();
        IoEvent events[kMaxEventsPerWait];

        while (is_running()) {
            u32 n = backend.wait(events, kMaxEventsPerWait, conns, kMaxConns);
            for (u32 i = 0; i < n; i++) {
                dispatch(events[i]);
            }
            poll_command();
            if (draining_.load(std::memory_order_acquire)) {
                close_listen();
                u64 start = drain_start_.load(std::memory_order_relaxed);
                u32 period = drain_period_.load(std::memory_order_relaxed);
                if (active_count() == 0) {
                    running_.store(false, std::memory_order_relaxed);
                } else if (monotonic_secs() >= start + period) {
                    force_close_all();
                    running_.store(false, std::memory_order_relaxed);
                }
            }
        }
    }

    void stop() { running_.store(false, std::memory_order_release); }
    bool is_running() const { return running_.load(std::memory_order_acquire); }
    bool is_draining() const { return draining_.load(std::memory_order_acquire); }

    void poll_command() {
        if (!control) return;
        auto* cfg = control->pending_config.exchange(nullptr, std::memory_order_acq_rel);
        if (cfg && config_ptr) *config_ptr = cfg;
        auto* jit = control->pending_jit.exchange(nullptr, std::memory_order_acq_rel);
        if (jit && jit_code_ptr) *jit_code_ptr = jit;
    }

    void epoch_enter() {
        if (epoch)
            epoch->epoch.store(epoch->epoch.load(std::memory_order_relaxed) + 1,
                               std::memory_order_release);
    }
    void epoch_leave() {
        if (epoch)
            epoch->epoch.store(epoch->epoch.load(std::memory_order_relaxed) + 1,
                               std::memory_order_release);
    }

    void shutdown() {
        backend.shutdown();
        pool.destroy();
    }

    void drain(u32 period_secs) {
        drain_period_.store(period_secs, std::memory_order_relaxed);
        drain_start_.store(monotonic_secs(), std::memory_order_relaxed);
        draining_.store(true, std::memory_order_release);
        if (backend.timer_fd >= 0) {
            struct itimerspec wake = {};
            wake.it_value.tv_nsec = 1;
            wake.it_interval.tv_sec = 1;
            timerfd_settime(backend.timer_fd, 0, &wake, nullptr);
        }
    }

    u32 active_count() const { return kMaxConns - free_top; }

    // --- CRTP implementations (no if constexpr — epoll only) ---

    Connection* alloc_conn_impl() {
        if (free_top == 0) return nullptr;
        u8* rs = pool.alloc();
        u8* ss = pool.alloc();
        if (!rs || !ss) {
            if (rs) pool.free(rs);
            if (ss) pool.free(ss);
            return nullptr;
        }
        u32 id = free_stack[--free_top];
        conns[id].reset();
        conns[id].id = id;
        conns[id].shard_id = static_cast<u8>(shard_id);
        conns[id].recv_slice = rs;
        conns[id].send_slice = ss;
        conns[id].recv_buf.bind(rs, SlicePool::kSliceSize);
        conns[id].send_buf.bind(ss, SlicePool::kSliceSize);
        return &conns[id];
    }

    void free_conn_impl(Connection& c) {
        u32 cid = c.id;
        timer.remove(&c);
        // Sync backend: kernel is done with buffers. Free immediately.
        if (c.recv_slice) pool.free(c.recv_slice);
        if (c.send_slice) pool.free(c.send_slice);
        c.reset();
        free_stack[free_top++] = cid;
    }

    void submit_recv_impl(Connection& c) { backend.add_recv(c.fd, c.id); }

    void submit_send_impl(Connection& c, const u8* buf, u32 len) {
        backend.add_send(c.fd, c.id, buf, len);
    }

    void submit_connect_impl(Connection& c, const void* addr, u32 addr_len) {
        backend.add_connect(c.upstream_fd, c.id, addr, addr_len);
    }

    void submit_send_upstream_impl(Connection& c, const u8* buf, u32 len) {
        backend.add_send_upstream(c.upstream_fd, c.id, buf, len);
    }

    void submit_recv_upstream_impl(Connection& c) {
        backend.add_recv_upstream(c.upstream_fd, c.id);
    }

    void close_conn_impl(Connection& c) {
        if (c.req_start_us != 0) epoch_leave();
        if (c.fd >= 0) {
            ::close(c.fd);
            c.fd = -1;
        }
        if (c.upstream_fd >= 0) {
            ::close(c.upstream_fd);
            c.upstream_fd = -1;
        }
        if (metrics) {
            if (c.req_start_us != 0) {
                if (metrics->requests_active > 0) metrics->requests_active--;
            }
            metrics->on_close();
        }
        this->free_conn(c);
    }

    // --- Dispatch ---

    void dispatch(const IoEvent& ev) {
        if (ev.type == IoEventType::Accept) {
            on_accept(ev);
            return;
        }
        if (ev.type == IoEventType::Timeout) {
            i32 ticks = ev.result > 0 ? ev.result : 1;
            const i32 max_ticks = static_cast<i32>(TimerWheel::kSlots);
            if (ticks > max_ticks) ticks = max_ticks;
            for (i32 t = 0; t < ticks; t++) {
                timer.tick([this](Connection* c) { this->close_conn(*c); });
            }
            if (draining_.load(std::memory_order_acquire)) {
                u64 start = drain_start_.load(std::memory_order_relaxed);
                u32 period = drain_period_.load(std::memory_order_relaxed);
                u64 now = monotonic_secs();
                for (u32 i = 0; i < kMaxConns; i++) {
                    if (conns[i].fd >= 0 && conns[i].state == ConnState::ReadingHeader &&
                        should_drain_close(i, start, now, period)) {
                        this->close_conn(conns[i]);
                    }
                }
            }
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

private:
    using Self = EpollEventLoop;

    void on_accept(const IoEvent& ev) {
        if (ev.result < 0) return;
        Connection* c = this->alloc_conn();
        if (!c) {
            ::close(ev.result);
            return;
        }
        c->fd = ev.result;
        struct sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        if (::getpeername(c->fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0 &&
            peer.sin_family == AF_INET) {
            c->peer_addr = peer.sin_addr.s_addr;
        }
        c->state = ConnState::ReadingHeader;
        c->keep_alive = !draining_.load(std::memory_order_relaxed);
        c->on_complete = &on_header_received<Self>;
        timer.add(c, keepalive_timeout);
        if (metrics) metrics->on_accept();
        this->submit_recv(*c);
    }

    void close_listen() {
        if (listen_fd >= 0) {
            backend.cancel_accept();
            ::close(listen_fd);
            listen_fd = -1;
        }
    }

    void force_close_all() {
        for (u32 i = 0; i < kMaxConns; i++) {
            if (conns[i].fd >= 0) {
                this->close_conn(conns[i]);
            }
        }
    }
};

}  // namespace rut
