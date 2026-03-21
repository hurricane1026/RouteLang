# TODO

Outstanding work items, tracked from code TODOs and Copilot review findings.

## Phase 1 (current)

### epoll backend
- [ ] **Partial send proactor semantics** — `add_send()` on EAGAIN/partial registers EPOLLOUT but `wait()` emits `result=0` (readiness). Callbacks treat non-negative as success → truncated responses under backpressure. Fix: track per-connection send state (buf/offset/remaining), complete the send inside `wait()`, emit real byte count. (`src/runtime/epoll_backend.cc:118`)
- [ ] **recv into Connection buffer** — `wait()` recv's into stack `tmp_buf` and discards data. Callbacks (especially proxy) need data in `Connection::recv_buf`. Fix: pass connection table to `wait()`, or extend IoEvent with buffer pointer. (`src/runtime/epoll_backend.cc:223`)

### io_uring backend
- [ ] **Timeout events** — io_uring backend does not emit `IoEventType::Timeout`. Timer wheel only driven by epoll's timerfd. Fix: add timerfd + `IORING_OP_READ` or `IORING_OP_TIMEOUT` for periodic ticks. (`include/rout/runtime/event_loop.h:135`)
- [ ] **Provided buffer return** — recv completions transfer buffer ownership to userspace but `EventLoop::dispatch` never calls `return_buffer()`. Will exhaust the ring after ~2048 recvs. Fix: return buffer after callback processes data (copy out, then return).

## Phase 2 (shard integration)
- [ ] **Shard: memory pools** — Arena, SlabPool, SlicePool per shard (`include/rout/runtime/shard.h:14`)
- [ ] **Shard: connection table** — SlabPool<Connection> (`shard.h:15`)
- [ ] **Shard: timer wheel integration** — wire into shard lifecycle (`shard.h:16`)
- [ ] **Shard: upstream connection pools** — per-upstream, per-shard (`shard.h:17`)
- [ ] **Shard: route table** — atomically swappable for hot reload (`shard.h:18`)
