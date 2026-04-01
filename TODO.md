# TODO

Outstanding work items, tracked from code TODOs and Copilot review findings.

## Phase 1 (current)

### epoll backend
- [x] **Partial send proactor semantics** — `add_send()` tracks offset/remaining in `SendState` per conn_id. `wait()` completes the send on EPOLLOUT via loop, emits real byte count. send_buf converted to Buffer.
- [x] **recv into Connection buffer** — `wait()` now recv's into `Connection::recv_buf` (Buffer) via `write_ptr()`/`commit()`. Connection table passed to `wait()`. Callbacks use `recv_buf.data()`/`recv_buf.len()`.

### io_uring backend
- [x] **Timeout events** — timerfd created in `init()`, `IORING_OP_READ` submitted for 1-second ticks. `wait()` emits `Timeout` events and re-submits the read.
- [x] **Provided buffer return** — `wait()` copies recv data from provided buffer into `Connection::recv_buf` via `write_ptr()`/`commit()`, then immediately calls `return_buffer(buf_id)`. Events reach dispatch with `has_buf=0`.

## Phase 2 (shard integration)
- [x] **Shard: per-core runtime** — Shard<Backend> wraps EventLoop (mmap'd), Arena (per-request scratch), listen_fd (SO_REUSEPORT), pthread with CPU affinity. Multi-shard main with signal-based graceful shutdown.
- [x] **Shard: connection table** — EventLoop owns Connection[16384] with free-stack pool (inherited from Phase 1). SlabPool deferred to when Connection moves off fixed array.
- [x] **Shard: timer wheel integration** — EventLoop owns TimerWheel, driven by timerfd. Shard lifecycle: shutdown drains via EventLoop::shutdown().
- [x] **Shard: upstream connection pools** — UpstreamPool per shard (mmap'd, 4096 slots), alloc/free/find_idle/return_idle/shutdown. UpstreamConn tracks fd + upstream_id + idle state.
- [x] **Shard: route table** — RouteConfig with RouteEntry[] (prefix match) + UpstreamTarget[] (addr:port). Supports Static/Proxy actions, method filter, first-match-wins. Shard holds const RouteConfig* (swappable for hot reload).
- [x] **SlicePool** — 16KB slice allocator, mmap-backed, O(1) alloc/free via free-stack. Per-shard, for on-demand network I/O buffers (idle connections hold 0 slices).
- [x] **SlabPool\<T, Cap\>** — generic fixed-size object pool, mmap-backed. alloc/free by pointer or index, index_of, capacity/available/in_use stats. Generalizes EventLoop's Connection free-stack.
- [ ] **Integration**: replace Connection inline storage with SlicePool slices (idle connections → zero buffer memory)

## Testing methodology gaps

### Fault injection for rare-condition bugs
Bugs like `elapsed_us` nsec underflow (#1), EINTR non-retry (#2-3), and mmap failure silent degradation (#7) all share a pattern: they involve OS-level conditions (clock boundary crossing, signal interruption, memory pressure) that normal unit tests never trigger.

**TODO**: Build a fault injection framework:
- **EINTR simulation**: wrap `write()`/`read()` in test builds to probabilistically return EINTR before the real call. Verifies all I/O loops retry correctly.
- **mmap failure injection**: allow tests to force `mmap()` to return MAP_FAILED on demand. Verifies all allocation paths propagate failure instead of silently degrading.
- **Boundary value clock**: provide a `clock_gettime()` shim that returns specific `timespec` values (e.g., `{tv_sec=1, tv_nsec=999999000}` → `{tv_sec=2, tv_nsec=1000}`) to exercise arithmetic edge cases.
- **Goal**: every new I/O function or time calculation automatically gets tested against these failure modes, so review doesn't have to catch them manually.

### API misuse pattern testing
Bugs like double-open fd leak (#4) and bodyless status codes (#5) involve using APIs in ways the happy path doesn't exercise.

**TODO**: Establish a testing checklist for new APIs:
- For any resource-owning struct (fd, mmap pointer): test double-open, use-after-close, double-close.
- For any function that takes a status/error code: test all semantic categories (1xx, 2xx, 3xx, 4xx, 5xx) and specifically codes with special HTTP semantics (204, 304, 101).
- For any function with a size parameter: test 0, 1, boundary-1, boundary, boundary+1.
