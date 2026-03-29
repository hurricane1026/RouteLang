# Backend Split: epoll / io_uring 独立实现方案

## 1. 目标

将当前 `EventLoop<Backend>` 模板 + `if constexpr` 方式拆分为两个独立的 EventLoop 实现：

- **EpollEventLoop** — 同步 I/O，即时 slice 回收，无 pending_ops/armed flags
- **IoUringEventLoop** — 异步 I/O，CQE 驱动延迟回收，multishot recv，provided buffer ring

每个实现拥有独立的：
- Connection 结构体（epoll 更精简）
- EventLoop 分发/运行逻辑
- 客户端和 upstream 独立的 recv buffer（消除共享 buffer 问题）

## 2. 当前架构

```
src/main.cc
  detect_io_uring() → run_shards<IoUringBackend> | run_shards<EpollBackend>
    Shard<Backend>
      EventLoop<Backend> : EventLoopCRTP<EventLoop<Backend>>
        Backend backend
        Connection conns[16384]     ← 单一 Connection 结构体
        callbacks.h templates       ← 无 if constexpr
        event_loop.h               ← 15 个 if constexpr 分支
```

### 关键发现

1. **callbacks.h 已经是 backend 无关的** — 0 个 `if constexpr`，通过 CRTP `Loop` 模板参数调用 `loop->submit_recv(conn)` 等方法
2. **所有 `if constexpr` 集中在 event_loop.h** — 15 处，控制回收策略、armed 追踪、cancel 路径
3. **共享 recv_buf 是多个 bug 的根源** — 客户端和 upstream 数据写入同一个 buffer，导致 proxy pipelining 和 streaming 的 buffer 污染

## 3. 目标架构

```
include/rut/runtime/
  event_loop_common.h       ← 共享 CRTP base, timer, drain, epoch, poll_command
  epoll_event_loop.h        ← EpollEventLoop（自包含，无模板参数）
  iouring_event_loop.h      ← IoUringEventLoop（自包含，无模板参数）
  connection_base.h         ← 共享字段
  epoll_connection.h        ← EpollConnection（精简）
  iouring_connection.h      ← IoUringConnection（完整 + upstream_recv_buf）
  callbacks.h               ← 保持不变（已经 backend 无关）
  shard.h                   ← 保持模板 Shard<EventLoopType>
```

## 4. 共享 vs 拆分矩阵

| 组件 | 共享/拆分 | 理由 |
|------|----------|------|
| HTTP parser, Chunked parser | 共享 | 纯解析，无 I/O 依赖 |
| Timer wheel, SlicePool | 共享 | 通用组件 |
| Access log, Metrics | 共享 | I/O 无关 |
| Route table, Upstream pool | 共享 | 只读配置 / 连接池 |
| Shard control | 共享 | 原子配置交换 |
| **Callbacks** | **共享** | 已经 backend 无关（CRTP） |
| **Shard** | **共享（模板）** | 0 个 if constexpr |
| **Connection** | **拆分** | epoll 不需要 armed/pending_ops |
| **EventLoop** | **拆分** | 消除 15 个 if constexpr |
| Backend structs | 已拆分 | 无需改动 |

## 5. 分阶段实现计划

### Phase 1: Connection 结构体拆分 (0.5 天)

**新文件：**
- `connection_base.h` — 提取共享字段（~100 行）
- `epoll_connection.h` — `using EpollConnection = ConnectionBase`（~30 行）
- `iouring_connection.h` — ConnectionBase + pending_ops + armed flags + upstream_recv_buf（~50 行）

**Epoll Connection 移除的字段：**
- `pending_ops`, `recv_armed`, `send_armed`, `upstream_recv_armed`, `upstream_send_armed`

**io_uring Connection 新增的字段：**
- `u8* upstream_recv_slice` — 独立的 upstream 接收 buffer
- `Buffer upstream_recv_buf` — 消除共享 buffer 问题

### Phase 2: EventLoop 拆分 (1.5 天)

**新文件：**
- `event_loop_common.h` — CRTP base + 共享逻辑（drain, stop, epoch, poll_command）（~80 行）
- `epoll_event_loop.h` — 即时回收，无 pending_ops（~250 行）
- `iouring_event_loop.h` — 延迟回收，CQE 驱动（~350 行）

**EpollEventLoop 精简点：**
- `free_conn_impl()`: 即时回收（无 pending_ops 检查）
- `submit_*_impl()`: 无 pending_ops++, 无 multishot guard
- `close_conn_impl()`: 无 cancel SQE
- `dispatch()`: 无 CQE 计数, 无 stale CQE 回收
- `on_accept()`: 无 deferred accept
- `run()`: 无 reclaim_pending
- 无 `pending_free[]`, `deferred_accepts[]`

**IoUringEventLoop 保留：**
- 所有延迟回收逻辑
- `reclaim_pending()`, `reclaim_slot()`, `retry_deferred_accepts()`
- armed flag 管理
- cancel SQE 追踪

### Phase 3: Callback 清理 (0.5 天)

Callbacks 已经 backend 无关，但有几处 workaround 可以清理：

1. `on_upstream_response()`: 移除 `ev.type == Recv` fallback（epoll 已经用 UpstreamRecv）
2. `on_header_received()`: stale UpstreamRecv handler 只在 io_uring 需要
3. `on_response_body_recvd()`: 有了独立 upstream_recv_buf，客户端 Recv 污染问题消失

### Phase 4: Shard 适配 (0.25 天)

```cpp
// 旧:
template <typename Backend> struct Shard { EventLoop<Backend>* loop; };

// 新:
template <typename EventLoopType> struct Shard { EventLoopType* loop; };
```

### Phase 5: main.cc 切换 (0.25 天)

```cpp
// 旧:
run_shards<IoUringBackend>(...)
run_shards<EpollBackend>(...)

// 新:
run_shards<IoUringEventLoop>(...)
run_shards<EpollEventLoop>(...)
```

### Phase 6: 测试基础设施 (0.5 天)

- `SmallLoop` → 使用 EpollConnection（无 armed/pending_ops）
- `AsyncSmallLoop` → 使用 IoUringConnection（含 upstream_recv_buf）

### Phase 7: Buffer 隔离 (1.5 天)

**核心改动：** io_uring 的 `dispatch()` 在 event type 为 `UpstreamRecv` 时，将数据从 `recv_buf` 移到 `upstream_recv_buf`。

**Callback 改动：**
- `on_upstream_response()`: 从 `upstream_recv_buf` 读取
- `on_response_body_recvd()`: 从 `upstream_recv_buf` 读取
- 消除所有 "stale client Recv" workaround

**内存策略：** upstream_recv_slice 懒分配（proxy connect 时才分配，非 proxy 连接不占用）

## 6. 关键设计决策

### ADR 1: Callbacks 保持共享模板

不创建 epoll_callbacks.h 和 iouring_callbacks.h。Callbacks 通过 CRTP `Loop` 参数已经 backend 无关。复制 1176 行回调代码只增加维护负担。

### ADR 2: Shard 保持模板

`Shard<EventLoopType>` 而非 EpollShard/IoUringShard。Shard 有 0 个 if constexpr，只用模板参数做类型和 sizeof。

### ADR 3: upstream buffer 懒分配

只在 proxy connect 发起时分配 upstream_recv_slice，非 proxy 连接不分配。大多数连接可能不 proxy，每连接省 16KB。

### ADR 4: EventLoop dispatch 做 buffer 路由（非 backend）

Backend 的 `wait()` 继续写入 `recv_buf`。EventLoop 的 `dispatch()` 在 event type 为 UpstreamRecv 时移动数据到 `upstream_recv_buf`。Backend 不需要知道连接级 buffer 语义。

## 7. 风险分析

| 风险 | 可能性 | 影响 | 缓解 |
|------|--------|------|------|
| Callback 行为在 epoll/io_uring 间不一致 | 中 | 高 | Callbacks 无 if constexpr，行为不变。两个 loop type 跑全量测试 |
| 遗漏 if constexpr 分支 | 中 | 高 | grep 所有 15 处，逐一验证。编译错误会捕获大部分 |
| Buffer 隔离破坏 upstream 解析 | 中 | 高 | on_upstream_response 改读 upstream_recv_buf。漏改会导致解析空 buffer |
| 3 slices/连接 SlicePool 耗尽 | 低 | 中 | 懒分配 upstream_recv_slice，只 proxy 连接占用 |
| 测试覆盖缺口 | 中 | 中 | AsyncSmallLoop 测试较少（~11 vs ~100+），需补充 |

## 8. 时间估算

| 阶段 | 工作 | 估时 |
|------|------|------|
| Phase 1: Connection 拆分 | 提取 base，创建 epoll/iouring 变体 | 0.5 天 |
| Phase 2: EventLoop 拆分 | 创建两个独立 EventLoop | 1.5 天 |
| Phase 3: Callback 清理 | 移除 workaround | 0.5 天 |
| Phase 4: Shard 适配 | 模板参数更新 | 0.25 天 |
| Phase 5: main.cc 切换 | 类型切换 + 删除旧模板 | 0.25 天 |
| Phase 6: 测试基础设施 | 更新 helpers，验证全量测试 | 0.5 天 |
| Phase 7: Buffer 隔离 | upstream_recv_buf + dispatch 路由 | 1.5 天 |
| 集成测试 | 全量测试 + 手动 proxy 测试 | 1 天 |
| **总计** | | **6 天** |

## 9. 分支策略

1. Feature branch: `split-backends`
2. 每阶段一个 commit
3. Phase 2 后新旧代码可共存 — 旧代码不删直到新类型测试通过
4. Phase 5 是切换 commit
5. 最终 commit 删除旧 `EventLoop<Backend>` 模板

## 10. 依赖图

```
Phase 1 (Connection) ──┬──→ Phase 2 (EventLoop) ──┬──→ Phase 4 (Shard)
                       │                          ├──→ Phase 5 (main.cc)
                       │                          ├──→ Phase 6 (Tests)
                       │                          └──→ Phase 3 (Callbacks)
                       │
                       └──→ Phase 7 (Buffer 隔离, 可并行)
```

Phase 1 + Phase 7 设计可并行。Phase 4/5/6 可在 Phase 2 完成后并行。
