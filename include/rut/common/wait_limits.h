#pragma once

#include "rut/common/types.h"

namespace rut {

inline constexpr u32 kMaxRouteWaits = 4;
inline constexpr u32 kWaitResultSlotsPerWait = 2;
inline constexpr u32 kMaxJitHandlerSlots = kMaxRouteWaits * kWaitResultSlotsPerWait;

}  // namespace rut
