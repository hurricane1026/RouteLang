#pragma once

#include "rout/common/types.h"

namespace rout {

// I/O event types — shared by both io_uring and epoll backends
enum class IoEventType : u8 {
    Accept,
    Recv,
    Send,
    UpstreamConnect,
    UpstreamRecv,
    Timeout,
};

// Unified completion event — both backends produce this
// Unified completion event — field order optimized for minimal padding (12 bytes).
struct IoEvent {
    u32 conn_id;
    i32 result;  // bytes transferred or error code
    u16 buf_id;  // provided buffer id (io_uring only, 0 for epoll)
    IoEventType type;
};

}  // namespace rout
