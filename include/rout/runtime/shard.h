#pragma once

#include "rout/common/types.h"
#include "rout/runtime/connection.h"
#include "rout/runtime/io_event.h"

namespace rout {

// Per-shard state — one per CPU core, share-nothing
struct Shard {
    u32 id;
    i32 listen_fd;

    // TODO: memory pools (Arena, SlabPool, SlicePool)
    // TODO: connection table
    // TODO: timer wheel
    // TODO: upstream connection pools
    // TODO: route table pointer (atomically swappable)
};

}  // namespace rout
