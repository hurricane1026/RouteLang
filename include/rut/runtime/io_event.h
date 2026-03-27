#pragma once

#include "rut/common/types.h"

namespace rut {

// I/O event types — shared by both io_uring and epoll backends
enum class IoEventType : u8 {
    Accept,
    Recv,
    Send,
    UpstreamConnect,
    UpstreamRecv,
    Timeout,
};

// Unified completion event — field order optimized for minimal padding.
struct IoEvent {
    u32 conn_id;
    i32 result;  // bytes transferred or error code
    u16 buf_id;  // provided buffer id (io_uring only; valid iff has_buf != 0)
    u8 has_buf;  // non-zero if this event owns a provided buffer in buf_id
    IoEventType type;
};

}  // namespace rut
