# `wait` Routes

`wait` is the route-handler suspension primitive. The intended model is not
"sleep several times"; it is "yield to the runtime, let the event loop drive one
or more operations, then resume this handler when an event completes."

## Event Model

The handler can suspend for three classes of events:

- Any current-connection event.
- A specific IO operation such as downstream recv/send or upstream
  connect/send/recv.
- A timer deadline.

Timer waits are currently wired end-to-end. Wait-any and wait-IO are the target
surface for the next slices; the runtime already has event types for recv,
send, upstream connect, upstream send, upstream recv, and timeout completions,
but the DSL/JIT ABI still needs to expose those as resumable wait results.

## Intended Syntax

Wait for any event on the current connection:

```rut
route GET "/stream" {
    let ev = wait()
    if ev.kind == recv { return 200 } else { return 500 }
}
```

Wait for request body data:

```rut
route POST "/upload" {
    let chunk = wait(io.recv(req.body))
    guard chunk.ok else { return 400 }
    return 204
}
```

Wait until a response write completes:

```rut
route GET "/flush" {
    let sent = wait(io.send(response(200, body: "ok")))
    guard sent.ok else { return 500 }
    return 204
}
```

Wait for an upstream connection:

```rut
upstream api at "127.0.0.1:9000"

route GET "/proxy" {
    let conn = wait(io.connect(api))
    guard conn.ok else { return 502 }
    return 200
}
```

Wait for upstream request send and response recv:

```rut
upstream api at "127.0.0.1:9000"

route POST "/proxy" {
    let upstream = wait(io.connect(api))
    guard upstream.ok else { return 502 }

    let sent = wait(io.upstream_send(upstream, req.body))
    guard sent.ok else { return 502 }

    let response = wait(io.upstream_recv(upstream))
    guard response.ok else { return 502 }
    return response.status
}
```

Race an IO operation with a timeout:

```rut
route POST "/upload" {
    let ev = wait(any(io.recv(req.body), timer(2s)))
    if ev.kind == timeout { return 408 } else { return 204 }
}
```

Wait for downstream disconnect while an upstream operation is in flight:

```rut
upstream api at "127.0.0.1:9000"

route GET "/long" {
    let ev = wait(any(io.recv(req), io.connect(api), timer(5s)))
    if ev.kind == recv && ev.eof { return 499 }
    if ev.kind == timeout { return 504 }
    return 200
}
```

## Timer Waits Today

Timer waits are the currently implemented subset:

```rut
route GET "/sleep" {
    wait(250)
    return 204
}
```

The bare integer form is milliseconds. Duration suffixes are also supported:

```rut
route GET "/short" { wait(500ms) return 204 }
route GET "/seconds" { wait(2s) return 204 }
route GET "/minutes" { wait(5m) return 204 }
route GET "/hours" { wait(1h) return 204 }
```

The duration is stored as an unsigned 32-bit millisecond payload. `wait(0)` is
rejected.

## Supported Timer Shape

Current timer-wait lowering supports source-ordered top-level guards and waits:

```rut
route GET "/x" {
    let code = 201
    guard req.path == "/x" else { return 404 }
    wait(100)
    if code == 201 { return 201 } else { return 500 }
}
```

In other words:

```text
let* ; (guard | wait)* ; terminal_control
```

`let` bindings may appear before the first `wait`. Top-level guards may appear
before, between, or after waits, and a failing guard returns immediately without
arming later timers. `let` after a `wait`, or `wait` after terminal control
(`if` / `match` / `return` / `forward`), is rejected today.

## Runtime Behavior

Timer `wait(...)` compiles to a timer yield:

1. Initial call with `state = 0` enters the first route state. It may return,
   forward, branch through guards, or return `Yield(Timer, ms, next_state = 1)`.
2. The event loop schedules a timer for the raw millisecond payload.
3. When the timer fires, the loop resumes the same handler with `state =
   next_state`.
4. The resumed state continues after the corresponding `wait(...)`.

Both production loops have millisecond-resolution timer paths:

- epoll uses a one-shot `timerfd` plus a min-heap of yield deadlines.
- io_uring uses `IORING_OP_TIMEOUT`.

## Decorators

Decorator guards run before user route logic and before any wait is armed:

```rut
func auth(_ ignored: i32) -> i32 => 0

route {
    @auth "*"
    GET "/sleep" { wait(1000) return 204 }
}
```

## Current Gaps

The following are future work rather than current behavior:

- Wait-any result values.
- Wait-IO operations for downstream recv/send and upstream connect/send/recv.
- `wait(any(...))` races across IO and timer events.
- `HandlerCtx` frame slots for values that must be stored across yield.
- `let` bindings after a `wait(...)`.
- Waits inside nested blocks, loops, branches, or decorator bodies.
- `match` terminal control on wait routes.
