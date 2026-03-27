#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/access_log.h"
#include "rut/runtime/callbacks.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/drain.h"
#include "rut/runtime/error.h"
#include "rut/runtime/io_backend.h"
#include "rut/runtime/io_event.h"
#include "rut/runtime/metrics.h"
#include "rut/runtime/timer_wheel.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timerfd.h>  // timerfd_settime
#include <unistd.h>       // close()

namespace rut {

// CRTP base — provides submit/close/alloc methods via static dispatch.
// No virtual functions, no vtable, no RTTI. The compiler inlines everything.
//
// Derived must implement:
//   void submit_recv_impl(Connection& c)
//   void submit_send_impl(Connection& c, const u8* buf, u32 len)
//   void submit_connect_impl(Connection& c, const void* addr, u32 addr_len)
//   void close_conn_impl(Connection& c)
//   Connection* alloc_conn_impl()
//   void free_conn_impl(Connection& c)

template <typename Derived>
class EventLoopCRTP {
    friend Derived;
    EventLoopCRTP() = default;

    Derived& self() { return static_cast<Derived&>(*this); }

public:
    void submit_recv(Connection& c) { self().submit_recv_impl(c); }
    void submit_send(Connection& c, const u8* buf, u32 len) {
        self().submit_send_impl(c, buf, len);
    }
    void submit_connect(Connection& c, const void* addr, u32 addr_len) {
        self().submit_connect_impl(c, addr, addr_len);
    }
    void close_conn(Connection& c) { self().close_conn_impl(c); }
    Connection* alloc_conn() { return self().alloc_conn_impl(); }
    void free_conn(Connection& c) { self().free_conn_impl(c); }

    // Upstream I/O: send/recv on upstream_fd instead of fd.
    void submit_send_upstream(Connection& c, const u8* buf, u32 len) {
        self().submit_send_upstream_impl(c, buf, len);
    }
    void submit_recv_upstream(Connection& c) { self().submit_recv_upstream_impl(c); }
};

template <typename Backend>
struct EventLoop : EventLoopCRTP<EventLoop<Backend>> {
    Backend backend;
    TimerWheel timer;
    u32 shard_id;

private:
    // Cross-thread state — main thread writes (stop/drain), shard thread reads.
    // All access via __atomic builtins for portability (ARM weak memory model).
    // draining_ is the release/acquire gate: drain_start_/drain_period_ are
    // written with relaxed ordering before draining_ is stored with release,
    // so the shard thread sees consistent values after acquiring draining_.
    bool running_;
    bool draining_;
    u64 drain_start_;   // monotonic seconds when drain began
    u32 drain_period_;  // seconds until force-close

public:
    static constexpr u32 kMaxConns = 16384;
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top;

    u32 keepalive_timeout = 60;
    i32 listen_fd = -1;  // stored for drain: close to stop kernel routing new connections

    // Per-shard access log ring. Set by Shard before run(). Null = no logging.
    AccessLogRing* access_log = nullptr;

    // Per-shard metrics. Set by Shard before run(). Null = no metrics.
    ShardMetrics* metrics = nullptr;

    core::Expected<void, Error> init(u32 id, i32 lfd) {
        shard_id = id;
        listen_fd = lfd;
        __atomic_store_n(&running_, true, __ATOMIC_RELAXED);
        __atomic_store_n(&draining_, false, __ATOMIC_RELAXED);
        __atomic_store_n(&drain_start_, static_cast<u64>(0), __ATOMIC_RELAXED);
        __atomic_store_n(&drain_period_, static_cast<u32>(0), __ATOMIC_RELAXED);
        keepalive_timeout = 60;
        free_top = kMaxConns;
        timer.init();
        for (u32 i = 0; i < kMaxConns; i++) {
            conns[i].reset();
            conns[i].id = i;
            conns[i].shard_id = static_cast<u8>(id);
            free_stack[i] = i;
        }
        TRY_VOID(backend.init(id, lfd));
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
            // Close listen fd after dispatching the current batch so any
            // already-queued accepts (epoll backlog) get a proper response
            // with Connection: close, rather than being dropped/reset.
            // Idempotent — only effective on the first drain iteration.
            if (__atomic_load_n(&draining_, __ATOMIC_ACQUIRE)) {
                close_listen();
                u64 start = __atomic_load_n(&drain_start_, __ATOMIC_RELAXED);
                u32 period = __atomic_load_n(&drain_period_, __ATOMIC_RELAXED);
                if (active_count() == 0) {
                    __atomic_store_n(&running_, false, __ATOMIC_RELAXED);
                } else if (monotonic_secs() >= start + period) {
                    force_close_all();
                    __atomic_store_n(&running_, false, __ATOMIC_RELAXED);
                }
            }
        }
    }

    void stop() { __atomic_store_n(&running_, false, __ATOMIC_RELEASE); }
    bool is_running() const { return __atomic_load_n(&running_, __ATOMIC_ACQUIRE); }
    bool is_draining() const { return __atomic_load_n(&draining_, __ATOMIC_ACQUIRE); }
    void shutdown() { backend.shutdown(); }

    // Begin graceful drain. New requests get Connection: close.
    // Idle connections are probabilistically closed on each timer tick.
    // After period_secs, force-close all remaining.
    //
    // Called from main thread. Relaxed stores for period/start, then
    // release store on draining_ — shard thread's acquire load on
    // draining_ guarantees it sees consistent period/start values.
    void drain(u32 period_secs) {
        __atomic_store_n(&drain_period_, period_secs, __ATOMIC_RELAXED);
        __atomic_store_n(&drain_start_, monotonic_secs(), __ATOMIC_RELAXED);
        __atomic_store_n(&draining_, true, __ATOMIC_RELEASE);

        // Wake the shard thread if it's blocked in backend.wait().
        // timerfd_settime is async-signal-safe and thread-safe (POSIX).
        // Setting a 1ns expiry fires immediately, causing wait() to return
        // a Timeout event so the run loop can observe draining_ and close_listen().
        if (backend.timer_fd >= 0) {
            struct itimerspec wake = {};
            wake.it_value.tv_nsec = 1;    // fire immediately
            wake.it_interval.tv_sec = 1;  // preserve 1-second periodic tick
            timerfd_settime(backend.timer_fd, 0, &wake, nullptr);
        }
    }

    // Number of allocated (in-use) connections.
    u32 active_count() const { return kMaxConns - free_top; }

    // --- CRTP implementations ---

    Connection* alloc_conn_impl() {
        if (free_top == 0) return nullptr;
        u32 id = free_stack[--free_top];
        conns[id].reset();
        conns[id].id = id;
        conns[id].shard_id = static_cast<u8>(shard_id);
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
    void submit_connect_impl(Connection& c, const void* addr, u32 addr_len) {
        backend.add_connect(c.upstream_fd, c.id, addr, addr_len);
    }
    void submit_send_upstream_impl(Connection& c, const u8* buf, u32 len) {
        backend.add_send(c.upstream_fd, c.id, buf, len);
    }
    void submit_recv_upstream_impl(Connection& c) { backend.add_recv(c.upstream_fd, c.id); }

    void close_conn_impl(Connection& c) {
        if (c.fd >= 0) {
            ::close(c.fd);
            c.fd = -1;
        }
        if (c.upstream_fd >= 0) {
            ::close(c.upstream_fd);
            c.upstream_fd = -1;
        }
        if (metrics) {
            // If a request was in flight (started but not completed),
            // decrement requests_active to avoid a permanent leak.
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
            // During drain: still accept queued connections so they get a
            // proper response with Connection: close, rather than a TCP RST.
            // on_accept() sets keep_alive=false when draining_ is true.
            on_accept(ev);
            return;
        }
        if (ev.type == IoEventType::Timeout) {
            // ev.result carries timerfd tick count — may be >1 if loop stalled.
            // Advance timer wheel once per accumulated tick to avoid skipping expirations.
            i32 ticks = ev.result > 0 ? ev.result : 1;
            const i32 max_ticks = static_cast<i32>(TimerWheel::kSlots);
            if (ticks > max_ticks) ticks = max_ticks;  // clamp to wheel size
            for (i32 t = 0; t < ticks; t++) {
                timer.tick([this](Connection* c) { this->close_conn(*c); });
            }

            // During drain: probabilistically close idle connections.
            // ReadingHeader = waiting for next request on keep-alive (effectively idle).
            if (__atomic_load_n(&draining_, __ATOMIC_ACQUIRE)) {
                u64 start = __atomic_load_n(&drain_start_, __ATOMIC_RELAXED);
                u32 period = __atomic_load_n(&drain_period_, __ATOMIC_RELAXED);
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
    using Self = EventLoop<Backend>;

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
        // During drain: mark new connections for close after first response.
        c->keep_alive = !__atomic_load_n(&draining_, __ATOMIC_RELAXED);
        c->on_complete = &on_header_received<Self>;
        timer.add(c, keepalive_timeout);
        if (metrics) metrics->on_accept();
        this->submit_recv(*c);
    }

    // Stop accepting new connections: cancel the backend's accept request
    // (required for io_uring multishot) then close the listen socket.
    void close_listen() {
        if (listen_fd >= 0) {
            backend.cancel_accept();
            ::close(listen_fd);
            listen_fd = -1;
        }
    }

    // Force-close all active connections (drain deadline exceeded).
    void force_close_all() {
        for (u32 i = 0; i < kMaxConns; i++) {
            if (conns[i].fd >= 0) {
                this->close_conn(conns[i]);
            }
        }
    }
};

}  // namespace rut
