#pragma once

#include "rut/runtime/connection_base.h"

namespace rut {

// Backward compatibility alias — all existing code uses "Connection".
// Future phases may split into EpollConnection / IoUringConnection.
using Connection = ConnectionBase;

}  // namespace rut
