#pragma once

#include "rout/common/types.h"

#include <errno.h>
#include <sys/mman.h>

namespace rout {

// SlabPool<T, Cap> — fixed-size object pool, O(1) alloc/free.
//
// All objects are pre-allocated in a single mmap'd region. Free-stack
// tracks available slots. No malloc, no fragmentation, cache-friendly.
//
// This is the generalized version of EventLoop's Connection[16384]+free_stack
// and UpstreamPool's UpstreamConn[4096]+free_stack.
//
// T must be trivially constructible (mmap zeroes memory).
// Caller is responsible for initializing objects after alloc.
//
// Usage:
//   SlabPool<Connection, 16384> pool;
//   pool.init();
//   auto [obj, idx] = pool.alloc();
//   // ... use obj ...
//   pool.free(idx);
//   pool.destroy();

template <typename T, u32 Cap>
struct SlabPool {
    T* objects = nullptr;       // mmap'd: Cap * sizeof(T)
    u32* free_stack = nullptr;  // mmap'd: Cap * sizeof(u32)
    u32 free_top = 0;
    u64 objects_size = 0;
    u64 stack_size = 0;

    // Initialize pool. mmap's objects + free stack.
    // Returns 0 on success, -errno on failure.
    i32 init() {
        free_top = Cap;

        // mmap objects
        objects_size = static_cast<u64>(Cap) * sizeof(T);
        void* obj_mem =
            mmap(nullptr, objects_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (obj_mem == MAP_FAILED) return -errno;
        objects = static_cast<T*>(obj_mem);

        // mmap free stack
        stack_size = static_cast<u64>(Cap) * sizeof(u32);
        void* stk_mem =
            mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stk_mem == MAP_FAILED) {
            munmap(objects, objects_size);
            objects = nullptr;
            return -errno;
        }
        free_stack = static_cast<u32*>(stk_mem);

        // Fill free stack
        for (u32 i = 0; i < Cap; i++) free_stack[i] = i;

        return 0;
    }

    // Allocate an object. Returns pointer + index, or {nullptr, 0} if full.
    T* alloc() {
        if (free_top == 0) return nullptr;
        u32 idx = free_stack[--free_top];
        return &objects[idx];
    }

    // Allocate and return index as well.
    T* alloc_with_id(u32& out_idx) {
        if (free_top == 0) {
            out_idx = 0;
            return nullptr;
        }
        out_idx = free_stack[--free_top];
        return &objects[out_idx];
    }

    // Free an object by index.
    void free(u32 idx) { free_stack[free_top++] = idx; }

    // Free an object by pointer.
    void free(T* obj) {
        u32 idx = static_cast<u32>(obj - objects);
        free_stack[free_top++] = idx;
    }

    // Get object by index.
    T& operator[](u32 idx) { return objects[idx]; }
    const T& operator[](u32 idx) const { return objects[idx]; }

    // Get index from pointer.
    u32 index_of(const T* obj) const { return static_cast<u32>(obj - objects); }

    // Stats.
    u32 capacity() const { return Cap; }
    u32 available() const { return free_top; }
    u32 in_use() const { return Cap - free_top; }

    // Release all mmap'd memory.
    void destroy() {
        if (free_stack) {
            munmap(free_stack, stack_size);
            free_stack = nullptr;
        }
        if (objects) {
            munmap(objects, objects_size);
            objects = nullptr;
        }
        free_top = 0;
    }
};

}  // namespace rout
