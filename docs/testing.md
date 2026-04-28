# Testing Notes

## Connection State Invariants

`Connection::state` is debug and metrics state. Runtime dispatch is driven by
the four callback slots:

- `on_recv`
- `on_send`
- `on_upstream_recv`
- `on_upstream_send`

Tests should assert both the state and the slots after representative dispatch
steps. A response can be correct on the wire while still leaving stale proxy or
streaming state behind, so state-only or slot-only assertions are not enough.

The main invariants are:

- `Idle`: client and upstream fds are closed, and all callback slots are null.
- `ReadingHeader`: client fd is live, `on_recv` is `on_header_received`, and all
  send/upstream slots are null.
- `Proxying`: upstream fd is live, and active slots describe the exact proxy
  phase, such as upstream response wait, request body streaming, or early
  response deferral.
- `Sending`: client fd is live, `on_send` is active, and upstream slots are null.
  The explicit exception is streaming response body wait after response headers
  have been sent: state remains `Sending`, `on_send` is null, and
  `on_upstream_recv` is `on_response_body_recvd`.
- `ExecHandler`: a yielded JIT handler keeps all callback slots null while
  `pending_handler_fn` and `handler_state` identify the resume target.

When adding a callback path that sends a 500/502 response, include a focused
state-invariant assertion. The expected shape is `ConnState::Sending` with only
the client send slot active. This catches stale proxy/body-streaming callbacks
that would otherwise be easy to miss in wire-level tests.

## Fault Injection

Shared syscall fault injection lives in `testing/fault_injection.h` and
`testing/fault_injection.cc`. Test files should use the RAII scopes there
instead of defining local `extern "C"` syscall hooks.

Current scopes:

- `ScopedMemoryFault`: fail the Nth `mmap` call, fail `mprotect`, or both.
- `ScopedFakeSocket`: return a specific fd from the next AF_INET stream
  `socket` call.
- `ScopedRecvData`: make `recv` for one fd return optional EINTR failures and
  then deterministic bytes.
- `ScopedIoFault`: make `poll`, `read`, or `write` for one fd return
  configured transient errors, fatal errors, or short I/O. It also covers
  fd-scoped `send`, `connect`, `close`, and `fcntl` failures.
- `ScopedSyscallFault`: inject process-wide failures for initialization or
  path-based syscalls such as `epoll_create1`, `epoll_ctl`, `epoll_wait`,
  `timerfd_create`, `timerfd_settime`, `accept4`, `open`, `mkstemp`, and
  `unlink`.

Memory, socket, and `recv` state is thread-local and restored when the scope
exits. `ScopedIoFault` and `ScopedSyscallFault` use process-wide atomic
counters so they can fault I/O performed by helper threads. This keeps fault
setup local to a test case while allowing one shared interpose layer per test
binary.
