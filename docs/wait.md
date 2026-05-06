# `wait(...)` Routes

`wait(...)` pauses a JIT route handler for a timer duration, then resumes the
same handler state machine.

## Syntax

```rut
route GET "/sleep" { wait(1000) return 204 }
```

The bare integer form is milliseconds. Duration suffixes are also supported:

```rut
route GET "/a" { wait(500ms) return 204 }
route GET "/b" { wait(2s) return 204 }
route GET "/c" { wait(5m) return 204 }
route GET "/d" { wait(1h) return 204 }
```

The duration is stored as an unsigned 32-bit millisecond payload. `wait(0)` is
rejected.

## Supported Route Shape

Current wait lowering supports this top-level route body shape:

```rut
route GET "/x" {
    let code = 201
    wait(100)
    wait(200)
    if code == 201 { return 201 } else { return 500 }
}
```

In other words:

```text
let* ; wait* ; guard* ; terminal_control
```

`let` bindings may appear before the first `wait`. Additional `wait(...)`
statements must be contiguous. `let` after a `wait`, or a `wait` after a
top-level `guard` / `if` / `match` / `return` / `forward`, is rejected today.

The current codegen re-materializes pure pre-wait local initializers when the
handler reaches the terminal state. Dedicated `HandlerCtx` slot storage for
values that truly live across yield boundaries is reserved for a later
state-machine lowering pass.

## Runtime Behavior

Each `wait(...)` compiles to a timer yield:

1. Initial call with `state = 0` returns `Yield(Timer, ms, next_state = 1)`.
2. The event loop schedules a timer for the raw millisecond payload.
3. When the timer fires, the loop resumes the same handler with `state =
   next_state`.
4. After the final wait, the handler runs the terminal control and returns or
   forwards.

Both production loops have millisecond-resolution timer paths:

- epoll uses a one-shot `timerfd` plus a min-heap of yield deadlines.
- io_uring uses `IORING_OP_TIMEOUT`.

If a precise timer path is unavailable under pressure, the loops may use the
1-second wheel only when the requested wait can still be represented safely.
Otherwise the request fails instead of resuming early.

## Decorators

Decorated wait routes are supported for the direct terminal subset:

```rut
func auth(_ ignored: i32) -> i32 => 0

route {
    @auth "*"
    GET "/sleep" { wait(1000) return 204 }
}
```

Decorator guards run before the first timer yield is armed. Decorated wait
routes currently reject user `let` bindings, user top-level guards, for-loops,
and non-direct terminal control such as `if` / `match`.

## Current Gaps

The following are future work rather than current behavior:

- Full source-ordered state-machine lowering for arbitrary route control around
  waits.
- `HandlerCtx` frame slots for impure local initializers or values that must be
  stored instead of re-materialized across a yield.
- Waits inside nested blocks, loops, branches, or decorator bodies.
- Non-timer yield kinds such as upstream async operations.
