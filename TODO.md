# TODO

Outstanding work items, prioritized for the next implementation passes.

## Recently Completed

- [x] epoll partial-send proactor semantics and recv-buffer integration.
- [x] io_uring timerfd timeout events and provided-buffer return path.
- [x] Shard runtime integration: per-core EventLoop, TimerWheel, route table, upstream pool, SlicePool, and SlabPool.
- [x] Connection buffers moved from inline storage to SlicePool-backed slices; idle/free connections hold zero buffer slices.
- [x] Traffic replay now covers static/default paths and explicitly skips proxy routes through `replay_one`.
- [x] Capture persistence now covers raw-header tail zeroing, corrupted/truncated entries, zeroed entries, and EINTR retry for capture read/write.
- [x] Response parser rejects malformed status codes (`XYZ`, non-digit, `<100`, `>599`).
- [x] Simulate engine rejects malformed captured request headers and counts malformed capture entries as failed.
- [x] Proxy 502 paths assert `ConnState::Sending`; upstream connect failure now sets that state in production.
- [x] State invariant tests cover representative static, proxy, body-streaming, JIT-yield, idle, and 502 dispatch transitions.
- [x] Testing notes document the callback-slot/state invariants and the streaming-body exception.
- [x] Runtime debug helpers can snapshot and format connection state, callback slots, armed operations, and buffer lengths.

## P0: State Invariant Coverage Follow-ups

**Goal**: Extend the newly added invariant checks to less-common transitions and keep the coverage aligned with future runtime changes.

**Why**: Baseline slot/state invariant coverage is now in place for representative static, proxy, body-streaming, JIT-yield, idle, and 502 dispatch transitions. The remaining work is to widen that coverage so new paths do not drift from the same debug/metrics expectations.

**Work**:
- Add follow-up tests for less-common or newly introduced transitions not yet covered by the representative dispatch cases.
- Reuse the existing invariant helper/check pattern when adding new dispatch paths or callback-slot combinations.
- Audit future state-machine changes for:
  - new `conn.state` values or transitions that need invariant assertions,
  - upstream error/timeout paths that may bypass the representative 502/500 cases,
  - teardown/reset flows where callback slots should be cleared before returning to idle/free states.

**Acceptance**:
- The backlog item is complete when remaining uncovered transitions have explicit invariant assertions or are documented as intentionally exempt.

## P0: Fault Injection Harness

**Goal**: Make OS-level edge cases cheap to add and hard to skip.

**Why**: EINTR, mmap failure, partial I/O, and clock-boundary issues rarely happen on local loopback. Small local shims caught real gaps, but they are currently duplicated per test file.

**Work**:
- Extract reusable test shims for:
  - `read` / `write` / `send` / `recv` one-shot and repeated EINTR.
  - `mmap` / `mprotect` failure injection.
  - deterministic `clock_gettime` boundary values.
- Start with runtime modules that already have retry/failure branches:
  - `traffic_capture`
  - `access_log`
  - `epoll_backend`
  - `io_uring_backend`
  - `SlicePool` / `Arena`

**Acceptance**:
- At least one shared helper replaces ad hoc EINTR counters.
- New tests verify retry or fail-closed behavior without depending on real network permissions.

## P1: Malformed Upstream E2E

**Goal**: Exercise parser-to-callback-to-wire behavior for malformed upstream responses through real sockets.

**Why**: Parser unit tests reject malformed responses, and mock callback tests cover some 502 branches, but end-to-end proxy behavior should prove the production socket path also fails closed.

**Work**:
- Add integration tests with a raw TCP upstream returning:
  - `HTTP/1.1 XYZ Bad\r\n\r\n`
  - empty response / immediate EOF
  - partial status line
  - malformed CRLF in headers
  - conflicting or invalid content framing
- Verify client-visible result: 502 or close, depending on the current contract.
- Verify metrics/debug state where available.

**Acceptance**:
- Tests drive full route config -> proxy connect -> upstream response -> client response path.
- No dependency on external services; test server is local and deterministic.

## P1: Replay Coverage Matrix

**Goal**: Prevent caller/path coverage holes when routing behavior expands.

**Why**: `replay_one` previously only covered static routes, so proxy route behavior regressed until review. Future route actions and callers need explicit matrix coverage.

**Work**:
- Document and enforce `[caller x route action x expected result]` cases for:
  - `replay_one`
  - `replay_file`
  - `simulate_one`
  - `simulate_file`
- Include Static, Default, Proxy, JIT ReturnStatus, JIT Forward, malformed input, and unsupported action paths where relevant.

**Acceptance**:
- A table-driven test or comment block makes missing cells obvious.
- Adding a route action requires adding or intentionally documenting replay/sim coverage.

## P2: Coverage Tooling Hygiene

**Goal**: Make coverage reports actionable instead of broad percentage noise.

**Why**: The project has many generated, third-party, benchmark, and architecture-specific files. Raw coverage can hide runtime gaps or chase irrelevant files.

**Work**:
- Review `scripts/coverage_report.py` exclusions and CI coverage target.
- Add per-area coverage summaries for runtime, parser, replay/sim, and compiler/JIT.
- Track coverage deltas for changed files in PRs if practical.

**Acceptance**:
- CI coverage output identifies the lowest-covered first-party runtime files.
- Third-party and benchmark files do not dominate coverage decisions.

## P2: TODO Maintenance

**Goal**: Keep this file as a live backlog, not a history log.

**Rules**:
- Move completed implementation milestones into `Recently Completed`.
- Keep active work scoped, prioritized, and testable.
- When review feedback creates a new recurring pattern, add it here with an acceptance criterion.
