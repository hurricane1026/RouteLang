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
- [x] **Integration**: replace Connection inline storage with SlicePool slices (idle connections → zero buffer memory). Production loops allocate recv/send slices on accept, lazily allocate upstream recv for proxying, and release all slices when the connection returns idle.

## Testing methodology gaps

Recurring patterns found during code review that automated tests consistently miss. Each needs a systematic prevention mechanism.

### 1. Fault injection for OS-level edge cases
**Pattern**: EINTR non-retry, mmap failure silent degradation, clock boundary arithmetic overflow.
**Why tests miss it**: Tests run on local loopback without signals, with abundant memory, and requests complete in < 1 second.

**TODO**: Build fault injection shims for test builds:
- **EINTR simulation**: Wrap `write()`/`read()`/`send()`/`recv()` to probabilistically return EINTR before the real call. Verifies all I/O loops retry correctly.
- **mmap failure injection**: Force `mmap()` to return MAP_FAILED on demand. Verifies all allocation paths propagate failure.
- **Boundary clock**: Provide a `clock_gettime()` shim that returns specific `timespec` values (e.g., `{tv_sec=1, tv_nsec=999999000}` → `{tv_sec=2, tv_nsec=1000}`) to exercise arithmetic edge cases.
- [x] Capture file read/write tests inject one-shot EINTR and verify `capture_read_entry` / `capture_write_entry` retry.

### 2. Untrusted/malicious input testing
**Pattern**: Status code parsing assumes digits, response parsing assumes well-formed HTTP.
**Why tests miss it**: Both client and server are our own code — always produce valid output.

**TODO**: For every function that parses external input, add a "malicious input" test suite:
- Garbage bytes, truncated data, oversized fields, non-ASCII characters.
- For sim_one: test with a raw TCP server that returns malformed responses (e.g., `"HTTP/1.1 XYZ Bad\r\n"`, empty response, partial response).
- For capture_read_entry: test with corrupted capture files (wrong magic, truncated entry, zeroed entry).
- [x] Response parser rejects malformed status codes (`XYZ`, non-digit, `<100`, `>599`).
- [x] Capture file tests cover invalid header metadata plus truncated and zeroed entries.

### 3. Unused data region hygiene
**Pattern**: CaptureEntry raw_headers tail contains uninitialized stack data after memcpy of valid bytes.
**Why tests miss it**: Tests only read `raw_headers[0..raw_header_len]`, never inspect the tail. But the full struct gets persisted to disk.

**TODO**: For any struct that is serialized/persisted:
- Zero-initialize the entire struct, not just the used portion.
- Or add a post-serialization check in debug builds that validates `memcmp(buf + used_len, zeros, total_len - used_len) == 0`.
- [x] Add a test that writes an entry to file, reads it back, and verifies bytes beyond `raw_header_len` are zero.

### 4. Internal state consistency testing
**Pattern**: conn.state left as Proxying while sending a 502 static response.
**Why tests miss it**: `conn.state` is only used for debugging/metrics, never checked in callback logic. Tests verify behavior (correct response), not internal state fields.

**TODO**: For debug-only fields that track state machine position:
- Add `CHECK_EQ(conn.state, ConnState::Sending)` assertions in tests that verify response paths.
- Or add a debug-mode invariant checker that validates `state` matches the active callback slot configuration after each dispatch.
- [x] Add `ConnState::Sending` assertions to proxy connect failure and malformed upstream 502 response paths.

### 5. Cross-path replay coverage
**Pattern**: replay_one only tested with static routes. Proxy routes through replay_one produce send_len==0 and bogus results.
**Why tests miss it**: Tests were written incrementally — static routes first, proxy routes tested separately via manual setup. Nobody ran a proxy route through the full `replay_one` → `replay_file` path.

**TODO**: For any function that dispatches to multiple code paths (static vs proxy vs default):
- Maintain an explicit coverage matrix: `[function × path × test]`.
- Every time a new path is added to a function, require a test that drives it through ALL callers (not just direct unit tests).
- [x] For replay: add a test that calls `replay_one` with a proxy route config and verifies `replayed == false` (the current correct behavior).
