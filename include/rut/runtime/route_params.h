#pragma once

#include "rut/common/types.h"

namespace rut {

static constexpr u32 kMaxRouteParams = 16;

struct RouteParam {
    const char* name = nullptr;
    u32 name_len = 0;
    const char* value = nullptr;
    u32 value_len = 0;
};

}  // namespace rut
