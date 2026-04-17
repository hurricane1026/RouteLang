#pragma once

#include "rut/common/types.h"
#include "rut/jit/handler_abi.h"

namespace rut {

// ── Outcome of a JIT handler invocation ────────────────────────────
//
// Decouples the handler ABI (packed u64) from the event-loop wiring.
// Callers (epoll_event_loop, iouring_event_loop, simulate_engine) invoke
// the handler via `invoke_jit_handler()` and then dispatch on `kind`:
//
//   - ReturnStatus: send HTTP response with `status_code` and finish.
//   - Forward:     proxy to upstream #`upstream_id`.
//   - TimerYield:  schedule `timer_seconds` later and resume the same
//                   handler with `ctx.state = next_state`. The handler
//                   was paused at a `wait(ms)` (or later `any(wait(h, ms))`)
//                   boundary; the event loop decides which timer
//                   mechanism to use (timerfd wheel / IORING_OP_TIMEOUT).
//   - Error:       handler returned an unsupported action — event loop
//                   should close the connection with 500.

struct JitDispatchOutcome {
    enum class Kind : u8 {
        ReturnStatus,
        Forward,
        TimerYield,
        Error,
    };

    Kind kind = Kind::Error;
    u16 status_code = 0;
    u16 upstream_id = 0;
    u16 next_state = 0;
    u16 timer_seconds = 0;  // rounded up from the handler's ms payload
};

// Round-up conversion from ms to seconds. Exposed so callers and tests
// agree on the clamping semantics. TimerWheel has 1s resolution today;
// if a finer wheel / io_uring timeout is added later, callers can switch
// to the raw ms payload stored in HandlerResult::status_code directly.
inline u16 timer_seconds_from_ms(u16 ms) {
    if (ms == 0) return 0;
    const u32 secs = (static_cast<u32>(ms) + 999u) / 1000u;
    return secs > 0xFFFFu ? static_cast<u16>(0xFFFFu) : static_cast<u16>(secs);
}

// Invoke a JIT-compiled handler once and translate the packed result
// into an event-loop-facing outcome. Does NOT loop — each Yield returns
// a TimerYield outcome, and the caller is expected to resume by setting
// `ctx.state = out.next_state` and calling this function again after the
// timer fires.
//
// Caller owns storage for `ctx`. For Layer 0 (no live-across-yield
// locals) `ctx.slot_count == 0` and the same `ctx` is reused across
// resumes; the entry call must set `ctx.state = 0`.
inline JitDispatchOutcome invoke_jit_handler(jit::HandlerFn fn,
                                             void* conn,
                                             jit::HandlerCtx& ctx,
                                             const u8* req_data,
                                             u32 req_len,
                                             void* arena) {
    JitDispatchOutcome out{};
    if (fn == nullptr) return out;  // Kind::Error by default

    const u64 packed = fn(conn, &ctx, req_data, req_len, arena);
    const auto r = jit::HandlerResult::unpack(packed);

    switch (r.action) {
        case jit::HandlerAction::ReturnStatus:
            out.kind = JitDispatchOutcome::Kind::ReturnStatus;
            out.status_code = r.status_code;
            return out;
        case jit::HandlerAction::Forward:
            out.kind = JitDispatchOutcome::Kind::Forward;
            out.upstream_id = r.upstream_id;
            return out;
        case jit::HandlerAction::Yield:
            if (r.yield_kind == jit::YieldKind::Timer) {
                out.kind = JitDispatchOutcome::Kind::TimerYield;
                out.next_state = r.next_state;
                // status_code slot carries the ms payload for Timer yields.
                out.timer_seconds = timer_seconds_from_ms(r.status_code);
                return out;
            }
            // HttpGet/HttpPost/Forward yields — future slices.
            return out;  // Kind::Error
    }
    return out;  // unreachable; leaves Kind::Error
}

}  // namespace rut
