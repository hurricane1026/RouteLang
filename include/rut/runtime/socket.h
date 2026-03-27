#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/error.h"

namespace rut {

// Create a non-blocking, reusable listen socket.
// Returns fd on success, Error on failure.
// Uses SO_REUSEPORT so each shard can bind the same port.
core::Expected<i32, Error> create_listen_socket(u16 port);

// Set fd to non-blocking mode. (kept as i32 — internal helper, error is rare)
i32 set_nonblocking(i32 fd);

}  // namespace rut
