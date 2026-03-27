#pragma once

#include "rut/common/types.h"
#include "rut/runtime/io_event.h"

namespace rut {

// I/O backend concept — satisfied by both IoUringBackend and EpollBackend.
// Selected at compile time via template parameter — no virtual dispatch.
//
// Required interface:
//   i32  init(u32 shard_id, i32 listen_fd);  // returns 0 on success, -errno on failure
//   void add_accept();
//   void add_recv(i32 fd, u32 conn_id);
//   void add_send(i32 fd, u32 conn_id, const u8* buf, u32 len);
//   void add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len);
//   void cancel(i32 fd, u32 conn_id);
//   u32  wait(IoEvent* events, u32 max_events, Connection* conns, u32 max_conns);
//
// Proactor model: wait() returns completed I/O events.
// - io_uring: native proactor, I/O is already done when CQE arrives
// - epoll: reactor internally, but wait() does recv/send and emits completions
//
// Error convention: IoEvent.result < 0 means -errno.

// Max events per wait call
static constexpr u32 kMaxEventsPerWait = 256;

// Provided buffer ring size (io_uring)
static constexpr u32 kProvidedBufCount = 2048;
static constexpr u32 kProvidedBufSize = 4096;  // 4KB per buffer

// Buffer group ID for provided buffer ring
static constexpr u16 kBufGroupId = 0;

}  // namespace rut
