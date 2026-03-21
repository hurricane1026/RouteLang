#pragma once

#include "rout/common/types.h"
#include "rout/runtime/error.h"

#include "core/expected.h"

#include <sys/mman.h>

namespace rout {

// SlicePool — fixed-size (16KB) memory slice allocator.
//
// Per-shard pool of pre-allocated 16KB slices for network I/O buffers.
// Connections borrow slices on demand (recv/send), return them when done.
// Idle connections hold 0 slices → zero memory overhead at C100K.
//
// All memory is mmap'd upfront (no malloc/free). Free-stack gives O(1)
// alloc/free with zero fragmentation.
//
// Usage:
//   SlicePool pool;
//   auto rc = pool.init(1024);  // 1024 × 16KB = 16MB
//   if (!rc) handle(rc.error());
//   u8* buf = pool.alloc();
//   pool.free(buf);
//   pool.destroy();

struct SlicePool {
    static constexpr u32 kSliceSize = 16384;  // 16KB per slice

    u8* base = nullptr;         // mmap'd region: count * kSliceSize bytes
    u32* free_stack = nullptr;  // mmap'd: free slice indices
    u32 free_top = 0;
    u32 count = 0;       // total number of slices
    u64 base_size = 0;   // size of mmap'd base region
    u64 stack_size = 0;  // size of mmap'd free_stack region

    // Initialize pool with `n` slices. Total memory: n * 16KB + n * 4 bytes.
    core::Expected<void, Error> init(u32 n) {
        count = n;
        free_top = n;

        // mmap slice data
        base_size = static_cast<u64>(n) * kSliceSize;
        void* data_mem =
            mmap(nullptr, base_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (data_mem == MAP_FAILED)
            return core::make_unexpected(Error::from_errno(Error::Source::SlicePool));
        base = static_cast<u8*>(data_mem);

        // mmap free stack
        stack_size = static_cast<u64>(n) * sizeof(u32);
        void* stack_mem =
            mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack_mem == MAP_FAILED) {
            munmap(base, base_size);
            base = nullptr;
            return core::make_unexpected(Error::from_errno(Error::Source::SlicePool));
        }
        free_stack = static_cast<u32*>(stack_mem);

        // Fill free stack: all slices available
        for (u32 i = 0; i < n; i++) free_stack[i] = i;

        return {};
    }

    // Allocate one 16KB slice. Returns pointer to slice, or nullptr if exhausted.
    u8* alloc() {
        if (free_top == 0) return nullptr;
        u32 idx = free_stack[--free_top];
        return base + static_cast<u64>(idx) * kSliceSize;
    }

    // Free a slice back to the pool. ptr must have been returned by alloc().
    void free(u8* ptr) {
        if (!ptr || !base || !free_stack || count == 0) return;
        if (ptr < base || ptr >= base + base_size) return;  // out of range
        u64 offset = static_cast<u64>(ptr - base);
        if (offset % kSliceSize != 0) return;  // not slice-aligned
        if (free_top >= count) return;         // overflow (double-free)
        u32 idx = static_cast<u32>(offset / kSliceSize);
        free_stack[free_top++] = idx;
    }

    // Number of available (free) slices.
    u32 available() const { return free_top; }

    // Number of in-use slices.
    u32 in_use() const { return count - free_top; }

    // Release all mmap'd memory.
    void destroy() {
        if (free_stack) {
            munmap(free_stack, stack_size);
            free_stack = nullptr;
        }
        if (base) {
            munmap(base, base_size);
            base = nullptr;
        }
        free_top = 0;
        count = 0;
    }
};

}  // namespace rout
