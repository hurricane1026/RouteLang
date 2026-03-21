# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Rue** is a strongly-typed DSL (`.rue` files) and high-performance HTTP gateway runtime replacing nginx + OpenResty. The project has two major components:

1. **The Rue Language** — A Swift-inspired DSL where HTTP concepts (methods, status codes, headers, CIDR, media types) are first-class typed objects. All functions inline at compile time into flat state machines. Async is invisible (no async/await — compiler auto-generates state machines at I/O points).

2. **The Runtime** — A custom C++ gateway using per-core share-nothing shards, io_uring (preferred) / epoll (fallback), LLVM ORC JIT, and zero-malloc hot paths. Built with `-fno-exceptions`, `-fno-rtti`, no C++ stdlib.

## Architecture

```
.rue source → Lexer/Parser → Typed AST → Inline Expansion → RIR (custom IR) → LLVM IR → ORC JIT → Native function pointers
                                                                                                              ↓
                                                                              Per-core shards (io_uring/epoll, share-nothing)
```

### Key architectural layers:
- **Compiler pipeline**: Hand-written recursive descent parser → type checker → inline expansion → Rue IR (RIR) → LLVM IR generation → ORC JIT
- **Rue IR (RIR)**: Custom flat typed IR between AST and LLVM IR. Enables domain-specific optimizations, backend independence, and `--emit-rir` debugging.
- **Runtime**: Template-parameterized on I/O backend (`IoUringBackend` / `EpollBackend`). Both produce the same `IoEvent` stream — upper layers are backend-agnostic.
- **Memory**: Per-shard allocators — Arena (per-request temporaries, reset = one pointer write), SlicePool (16KB network buffers, free-list), SlabPool (fixed-size objects like Connection). No malloc/free on hot path.
- **Hot reload**: RCU pattern — compile on background thread, atomic pointer swap of `CompiledConfig`, epoch-based reclamation of old JIT code.

### Implementation phases:
- **Phase 1**: Minimal runtime — io_uring/epoll backends, memory allocators, HTTP parser, connection management, static routing, proxy (~5K lines C++)
- **Phase 2**: Language + JIT — lexer/parser, type checker, LLVM IR codegen, hot reload, state machine transform, radix trie router (~8K lines)
- **Phase 3**: Production hardening — TLS (OpenSSL + kTLS), HTTP/2, observability, graceful shutdown, LSP server

## Build

```bash
./dev.sh build        # configure (clang++) + build
./dev.sh test         # build + run tests
./dev.sh tidy         # clang-tidy
./dev.sh format       # clang-format check
./dev.sh format-fix   # clang-format in-place
./dev.sh all          # build + test + tidy + format-check
./dev.sh clean        # rm -rf build
```

Or manually:
```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
ninja -C build
./build/tests/test_network
```

Binary: `build/src/rue`. Compiler: clang++. Linter: clang-tidy (`.clang-tidy`). Formatter: clang-format (`.clang-format`, Google-based, 4-space indent).

## C++ Conventions

- No C++ standard library headers or libraries: use custom `FixedVec<T, Cap>`, `Str` (non-owning string view), `FlatMap<K, V, Cap>`, `ListNode` (intrusive linked list)
- No `new`, no `malloc` — all allocation via mmap-backed per-shard allocators (Arena, SlabPool, SlicePool)
- **No exceptions** (`-fno-exceptions`): code never throws. No try/catch, no `std::expected`. Errors are returned via result values or handled inline.
- No RTTI (`-fno-rtti`)
- No heap allocation on hot paths
- Compile-time backend selection via templates, not virtual dispatch
- All cross-shard communication is lock-free: atomics, per-shard counters aggregated on read, RCU for config
- Cache-line alignment (`alignas(64)`) for shared atomics to prevent false sharing

## Runtime Design Decisions

### Connection handling: function-pointer callbacks (not coroutines)

Each `Connection` holds a `void (*on_complete)(EventLoop&, Connection&, IoEvent)`. On I/O completion, the event loop calls `conn.on_complete(loop, conn, event)`. Each callback processes the result, sets `on_complete` to the next step, and submits the next I/O operation.

Coroutines were explored but rejected: frame allocation adds complexity (FramePool/mmap), and cross-connection scenarios (fire-and-forget, traffic mirroring) create frame lifetime issues. fp callbacks have zero infrastructure overhead.

### Timer management: timer wheel (O(1) all operations)

60-slot array + intrusive linked list per slot, 1-second resolution, driven by single timerfd per shard. Chosen over nginx's rbtree (O(log n)) for simplicity (~50 lines) and cache-friendliness at C1000K scale.

### Memory: Arena + SlicePool (not Arena-only like nginx)

- **Arena**: per-request bump allocator, bulk reset on request completion. For parsed headers, route params, JIT handler temporaries.
- **SlicePool**: per-shard free-list of fixed 16KB slices. For network recv/send buffers with unpredictable lifetimes (streaming proxy, body rewrite). Idle connections hold zero slices.

nginx uses only Arena + fixed `proxy_buffers` per connection, which doesn't scale to C100K+ (128KB/conn × 100K = 12.8GB). SlicePool gives zero memory overhead for idle connections.

### io_uring vs epoll: dual backend, workload-dependent tradeoffs

Real benchmarks show io_uring is not universally faster:
- High-concurrency ping-pong: **io_uring +50-70%**
- Single-connection streaming: **epoll 2-3x faster**
- HTTP proxy (Envoy): **io_uring +10%**
- Accept bursts: **io_uring +20-50%**

io_uring's main value at C1000K: provided buffer ring (idle connections = 0 buffer), multishot accept/recv, syscall batching. Both backends produce identical `IoEvent` streams — upper layers are unaware of which backend is active.

## Language Design Reference

The Rue language uses Swift-inspired syntax. Key patterns:
- `guard ... else { return <status> }` for middleware reject-or-continue
- Named parameters: `auth(req, role: "user")`
- Trailing closures: `get /path { req in ... }`
- Request/Response middleware distinguished by parameter signature (not keywords)
- Domain types (`Duration`, `ByteSize`, `StatusCode`, `IP`, `CIDR`, `MediaType`) are compile-time validated
- `yield` instructions in RIR mark I/O suspend points where the compiler splits functions into state machine states
