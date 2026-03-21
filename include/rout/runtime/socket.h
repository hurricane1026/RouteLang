#pragma once

#include "rout/common/types.h"

namespace rout {

// Create a non-blocking, reusable listen socket.
// Returns fd on success, -errno on failure.
// Uses SO_REUSEPORT so each shard can bind the same port.
i32 create_listen_socket(u16 port);

// Set fd to non-blocking mode.
i32 set_nonblocking(i32 fd);

}  // namespace rout
