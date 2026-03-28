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
#include "rut/runtime/shard_control.h"
#include "rut/runtime/slice_pool.h"
#include "rut/runtime/timer_wheel.h"
#include <atomic>

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
    // All access via std::atomic for portability (ARM weak memory model).
    // draining_ is the release/acquire gate: drain_start_/drain_period_ are
    // written with relaxed ordering before draining_ is stored with release,
    // so the shard thread sees consistent values after acquiring draining_.
    std::atomic<bool> running_;
    std::atomic<bool> draining_;
    std::atomic<u64> drain_start_;   // monotonic seconds when drain began
    std::atomic<u32> drain_period_;  // seconds until force-close

public:
    static constexpr u32 kMaxConns = 16384;
    static constexpr u32 kDefaultKeepaliveTimeout = 60;
    SlicePool pool;  // per-shard buffer pool (2 slices per active connection)
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top;

    // Pending-free list: slots closed during the current dispatch batch.
    // Moved to free_stack (and deferred slices returned to pool) after the
    // next wait() cycle, which submits cancel SQEs and harvests CQEs so
    // the kernel no longer references the buffers.
    u32 pending_free[kMaxConns];
    u32 pending_free_count;

    // Deferred accepts: accepted fds that couldn't be allocated during
    // dispatch because all slots were in pending_free. Retried after the
    // batch finishes and reclaim_pending() frees slots.
    static constexpr u32 kMaxDeferredAccepts = 64;
    i32 deferred_accepts[kMaxDeferredAccepts];
    u32 deferred_accept_count;

    u32 keepalive_timeout = kDefaultKeepaliveTimeout;
    i32 listen_fd = -1;  // stored for drain: close to stop kernel routing new connections

    // Per-shard access log ring. Set by Shard before run(). Null = no logging.
    AccessLogRing* access_log = nullptr;

    // Per-shard metrics. Set by Shard before run(). Null = no metrics.
    ShardMetrics* metrics = nullptr;

    // Per-shard control plane pointers. Set by Shard::init(), read by
    // poll_command() / epoch_enter() / epoch_leave() on the shard thread.
    // Null = no control plane (standalone EventLoop in tests).
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
        pending_free_count = 0;
        deferred_accept_count = 0;
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
            // Async backend: reclaim closed slots whose CQEs have all been
            // harvested. Runs AFTER dispatch so stale CQEs decrement
            // pending_ops before we check. Then retry any deferred accepts.
            if constexpr (Backend::kAsyncIo) {
                reclaim_pending();
                retry_deferred_accepts();
            }
            poll_command();
            // Close listen fd after dispatching the current batch so any
            // already-queued accepts (epoll backlog) get a proper response
            // with Connection: close, rather than being dropped/reset.
            // Idempotent — only effective on the first drain iteration.
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

    // Poll the per-shard control block. Independent config + JIT slots.
    // Zero-cost when neither flag is set (two atomic loads, both predicted
    // not-taken).
    void poll_command() {
        if (!control) return;
        // Single atomic exchange per slot: read + clear in one op.
        // nullptr = no update; non-null = apply.
        auto* cfg = control->pending_config.exchange(nullptr, std::memory_order_acq_rel);
        if (cfg && config_ptr) *config_ptr = cfg;

        auto* jit = control->pending_jit.exchange(nullptr, std::memory_order_acq_rel);
        if (jit && jit_code_ptr) *jit_code_ptr = jit;
    }

    // RCU monotonic epoch. Both enter and leave increment, so the control
    // plane can snapshot before a swap and wait for advancement.
    // Zero-cost when epoch pointer is null (no control plane wired).
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
        if constexpr (Backend::kAsyncIo) reclaim_pending();
        backend.shutdown();
        pool.destroy();
    }

    // Reclaim a single slot from pending_free by conn_id. Frees slices,
    // pushes to free_stack, and removes from the pending_free array.
    // Called inline from dispatch() when a stale CQE completes reclamation,
    // so a later Accept in the same batch can reuse the slot immediately.
    void reclaim_slot(u32 cid) {
        if (conns[cid].recv_slice) {
            pool.free(conns[cid].recv_slice);
            conns[cid].recv_slice = nullptr;
        }
        if (conns[cid].send_slice) {
            pool.free(conns[cid].send_slice);
            conns[cid].send_slice = nullptr;
        }
        free_stack[free_top++] = cid;
        // Remove from pending_free (swap with last element).
        for (u32 i = 0; i < pending_free_count; i++) {
            if (pending_free[i] == cid) {
                pending_free[i] = pending_free[--pending_free_count];
                break;
            }
        }
    }

    // CQE-driven reclamation: only reclaim slots whose in-flight I/O has
    // fully completed (pending_ops == 0). Slots still waiting for CQEs
    // remain in pending_free until a future dispatch() decrements their
    // pending_ops to 0.
    void reclaim_pending() {
        u32 remaining = 0;
        for (u32 i = 0; i < pending_free_count; i++) {
            u32 cid = pending_free[i];
            if (conns[cid].pending_ops == 0) {
                if (conns[cid].recv_slice) {
                    pool.free(conns[cid].recv_slice);
                    conns[cid].recv_slice = nullptr;
                }
                if (conns[cid].send_slice) {
                    pool.free(conns[cid].send_slice);
                    conns[cid].send_slice = nullptr;
                }
                free_stack[free_top++] = cid;
            } else {
                pending_free[remaining++] = cid;
            }
        }
        pending_free_count = remaining;
    }

    // Begin graceful drain. New requests get Connection: close.
    // Idle connections are probabilistically closed on each timer tick.
    // After period_secs, force-close all remaining.
    //
    // Called from main thread. Relaxed stores for period/start, then
    // release store on draining_ — shard thread's acquire load on
    // draining_ guarantees it sees consistent period/start values.
    void drain(u32 period_secs) {
        drain_period_.store(period_secs, std::memory_order_relaxed);
        drain_start_.store(monotonic_secs(), std::memory_order_relaxed);
        draining_.store(true, std::memory_order_release);

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

    // Number of connections not yet fully reclaimed.
    // Includes pending_free slots: they're closed but still waiting for
    // CQEs, so drain must keep running until they're reclaimed too.
    u32 active_count() const { return kMaxConns - free_top; }

    // --- CRTP implementations ---

    Connection* alloc_conn_impl() {
        if (free_top == 0) return nullptr;
        // Allocate buffer slices from pool before committing the conn slot.
        u8* rs = pool.alloc();
        u8* ss = pool.alloc();
        if (!rs || !ss) {
            if (rs) pool.free(rs);
            if (ss) pool.free(ss);
            return nullptr;  // pool exhausted — back-pressure
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
        if constexpr (Backend::kAsyncIo) {
            // Async backend (io_uring): if no ops are in flight (the close
            // was triggered by the final CQE), reclaim immediately — no
            // need to defer. This avoids blocking alloc_conn at saturation
            // when a close and accept arrive in the same dispatch batch.
            if (c.pending_ops == 0) {
                if (c.recv_slice) pool.free(c.recv_slice);
                if (c.send_slice) pool.free(c.send_slice);
                c.reset();
                free_stack[free_top++] = cid;
                return;
            }
            // Ops still in flight: defer until CQEs arrive and pending_ops
            // reaches 0 in reclaim_pending().
            u8* rs = c.recv_slice;
            u8* ss = c.send_slice;
            u32 ops = c.pending_ops;
            c.reset();
            conns[cid].recv_slice = rs;
            conns[cid].send_slice = ss;
            conns[cid].pending_ops = ops;
            pending_free[pending_free_count++] = cid;
        } else {
            // Sync backend (epoll): kernel is done with buffers when
            // read/write returns. Return slices to pool immediately.
            if (c.recv_slice) pool.free(c.recv_slice);
            if (c.send_slice) pool.free(c.send_slice);
            c.reset();
            free_stack[free_top++] = cid;
        }
    }

    void submit_recv_impl(Connection& c) {
        if constexpr (Backend::kAsyncIo) {
            // Multishot recv stays armed across keep-alive cycles.
            // Skip re-submit to avoid inflating pending_ops.
            if (c.recv_armed) return;
        }
        if (backend.add_recv(c.fd, c.id)) {
            if constexpr (Backend::kAsyncIo) {
                c.pending_ops++;
                c.recv_armed = true;
            }
        }
    }
    void submit_send_impl(Connection& c, const u8* buf, u32 len) {
        if (backend.add_send(c.fd, c.id, buf, len)) {
            if constexpr (Backend::kAsyncIo) {
                c.pending_ops++;
                c.send_armed = true;
            }
        }
    }
    void submit_connect_impl(Connection& c, const void* addr, u32 addr_len) {
        if (backend.add_connect(c.upstream_fd, c.id, addr, addr_len)) {
            if constexpr (Backend::kAsyncIo) c.pending_ops++;
        }
    }
    void submit_send_upstream_impl(Connection& c, const u8* buf, u32 len) {
        if (backend.add_send_upstream(c.upstream_fd, c.id, buf, len)) {
            if constexpr (Backend::kAsyncIo) {
                c.pending_ops++;
                c.upstream_send_armed = true;
            }
        }
    }
    void submit_recv_upstream_impl(Connection& c) {
        if constexpr (Backend::kAsyncIo) {
            if (c.upstream_recv_armed) return;
        }
        if (backend.add_recv_upstream(c.upstream_fd, c.id)) {
            if constexpr (Backend::kAsyncIo) {
                c.pending_ops++;
                c.upstream_recv_armed = true;
            }
        }
    }

    void close_conn_impl(Connection& c) {
        // If a request was in flight (epoch_enter called), leave the epoch
        // before closing. This covers timer wheel timeouts, force_close_all
        // during drain, and any other path that bypasses normal callbacks.
        if (c.req_start_us != 0) epoch_leave();
        if constexpr (Backend::kAsyncIo) {
            // Only cancel when ops are in flight. If pending_ops == 0,
            // the slot is freed immediately — no cancels needed.
            // Add cancel count to pending_ops so the slot isn't reclaimed
            // until all cancel CQEs have been processed.
            if (c.pending_ops > 0) {
                c.pending_ops += backend.cancel(c.fd,
                                                c.id,
                                                c.recv_armed,
                                                c.send_armed,
                                                c.upstream_recv_armed,
                                                c.upstream_send_armed,
                                                c.upstream_fd >= 0);
            }
        }
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
            if constexpr (Backend::kAsyncIo) {
                // Decrement pending_ops only on the final CQE for this op.
                // Multishot recv (IORING_RECV_MULTISHOT) sets ev.more on
                // intermediate CQEs — the SQE stays armed, so the op is
                // still in-flight and must not be counted as complete.
                if (!ev.more) {
                    if (conn.pending_ops > 0) conn.pending_ops--;
                    // Multishot recv ended — clear the armed flag using
                    // event type (not state) to distinguish client vs upstream.
                    if (ev.type == IoEventType::Recv) conn.recv_armed = false;
                    if (ev.type == IoEventType::Send) conn.send_armed = false;
                    if (ev.type == IoEventType::UpstreamSend) conn.upstream_send_armed = false;
                    if (ev.type == IoEventType::UpstreamRecv) conn.upstream_recv_armed = false;
                }
            }
            if (conn.on_complete) {
                timer.refresh(&conn, keepalive_timeout);
                conn.on_complete(this, conn, ev);
            } else if constexpr (Backend::kAsyncIo) {
                // Stale CQE for a closed connection. If all ops are now
                // complete, reclaim the slot immediately so a later Accept
                // in the same batch can reuse it (avoids dropping connections
                // at saturation).
                if (conn.pending_ops == 0) {
                    reclaim_slot(ev.conn_id);
                }
            }
        }
    }

private:
    using Self = EventLoop<Backend>;

    void on_accept(const IoEvent& ev) {
        if (ev.result < 0) return;
        Connection* c = this->alloc_conn();
        if (!c) {
            if constexpr (Backend::kAsyncIo) {
                // Try reclaiming slots from stale CQEs earlier in this batch.
                reclaim_pending();
                c = this->alloc_conn();
                if (!c) {
                    // Still no slot — a later CQE in this batch may free one.
                    // Defer the accept fd and retry after the batch finishes.
                    if (deferred_accept_count < kMaxDeferredAccepts) {
                        deferred_accepts[deferred_accept_count++] = ev.result;
                    } else {
                        ::close(ev.result);
                    }
                    return;
                }
            } else {
                ::close(ev.result);
                return;
            }
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
        c->keep_alive = !draining_.load(std::memory_order_relaxed);
        c->on_complete = &on_header_received<Self>;
        timer.add(c, keepalive_timeout);
        if (metrics) metrics->on_accept();
        this->submit_recv(*c);
    }

    // Retry accepts that were deferred because no slot was available
    // during the dispatch batch. Now that reclaim_pending() has run,
    // slots may have become available.
    void retry_deferred_accepts() {
        for (u32 i = 0; i < deferred_accept_count; i++) {
            i32 fd = deferred_accepts[i];
            Connection* c = this->alloc_conn();
            if (!c) {
                ::close(fd);
                continue;
            }
            c->fd = fd;
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
        deferred_accept_count = 0;
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
