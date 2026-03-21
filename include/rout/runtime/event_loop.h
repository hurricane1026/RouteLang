#pragma once

#include "rout/common/types.h"
#include "rout/runtime/callbacks.h"
#include "rout/runtime/connection.h"
#include "rout/runtime/error.h"
#include "rout/runtime/io_backend.h"
#include "rout/runtime/io_event.h"
#include "rout/runtime/timer_wheel.h"

#include "core/expected.h"

#include <unistd.h>  // close()

namespace rout {

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
    bool running_;  // access only via is_running()/stop() with atomics

public:
    static constexpr u32 kMaxConns = 16384;
    Connection conns[kMaxConns];
    u32 free_stack[kMaxConns];
    u32 free_top;

    u32 keepalive_timeout = 60;

    core::Expected<void, Error> init(u32 id, i32 listen_fd) {
        shard_id = id;
        __atomic_store_n(&running_, true, __ATOMIC_RELAXED);
        keepalive_timeout = 60;  // explicit: mmap zeroes memory, skipping default member init
        free_top = kMaxConns;
        timer.init();
        for (u32 i = 0; i < kMaxConns; i++) {
            conns[i].reset();
            conns[i].id = i;
            conns[i].shard_id = static_cast<u8>(id);
            free_stack[i] = i;
        }
        TRY_VOID(backend.init(id, listen_fd));
        return {};
    }

    void run() {
        backend.add_accept();
        IoEvent events[kMaxEventsPerWait];

        while (__atomic_load_n(&running_, __ATOMIC_RELAXED)) {
            u32 n = backend.wait(events, kMaxEventsPerWait, conns, kMaxConns);
            for (u32 i = 0; i < n; i++) {
                dispatch(events[i]);
            }
        }
    }

    void stop() { __atomic_store_n(&running_, false, __ATOMIC_RELAXED); }
    bool is_running() const { return __atomic_load_n(&running_, __ATOMIC_RELAXED); }
    void shutdown() { backend.shutdown(); }

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
        this->free_conn(c);
    }

    // --- Dispatch ---

    void dispatch(const IoEvent& ev) {
        if (ev.type == IoEventType::Accept) {
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
        c->state = ConnState::ReadingHeader;
        c->on_complete = &on_header_received<Self>;
        timer.add(c, keepalive_timeout);
        this->submit_recv(*c);
    }
};

}  // namespace rout
