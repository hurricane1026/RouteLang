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
- [ ] **Shard: memory pools** — Arena, SlabPool, SlicePool per shard (`include/rout/runtime/shard.h:14`)
- [ ] **Shard: connection table** — SlabPool<Connection> (`shard.h:15`)
- [ ] **Shard: timer wheel integration** — wire into shard lifecycle (`shard.h:16`)
- [ ] **Shard: upstream connection pools** — per-upstream, per-shard (`shard.h:17`)
- [ ] **Shard: route table** — atomically swappable for hot reload (`shard.h:18`)
