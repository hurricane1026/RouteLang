#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/error.h"

#include <errno.h>
#include <sys/mman.h>  // mmap, munmap

namespace rut {

// Arena — bump allocator with block chaining. Zero stdlib, no malloc, no new.
//
// Pure bump allocation. No destructors, no cleanup, no ownership tracking.
// Objects allocated from Arena must be trivially destructible, or the caller
// is responsible for calling destructors manually before reset/destroy.
//
// Block memory comes from mmap/munmap. No malloc/free anywhere.
//
// Typical usage: per-request temporaries.
//   Arena arena;
//   arena.init(4096);
//   auto* hdr = arena.alloc_t<Header>();
//   auto* params = arena.alloc_array<Param>(n);
//   ... process request ...
//   arena.reset();  // instant, reuses first block

struct Arena {
    struct Block {
        Block* prev;
        u64 size;  // total mmap'd size (including this header)
        u64 used;  // bytes allocated after header

        u8* data() { return reinterpret_cast<u8*>(this) + ((sizeof(Block) + 15) & ~u64(15)); }
        u64 capacity() const { return size - ((sizeof(Block) + 15) & ~u64(15)); }
        u64 remaining() const { return capacity() - used; }
    };

    Block* current = nullptr;
    u64 block_size = 0;
    u64 total_allocated = 0;

    core::Expected<void, Error> init(u64 initial_block_size) {
        block_size = initial_block_size < 256 ? 256 : initial_block_size;
        total_allocated = 0;
        current = alloc_block(block_size, nullptr);
        if (!current) return core::make_unexpected(Error::from_errno(Error::Source::Arena));
        return {};
    }

    // Bump allocate, 8-byte aligned.
    void* alloc(u64 size) {
        if (!current) return nullptr;
        // Overflow check: size + 7 must not wrap
        if (size > static_cast<u64>(-1) - 7) return nullptr;
        size = (size + 7) & ~static_cast<u64>(7);
        if (current->remaining() >= size) {
            void* p = current->data() + current->used;
            current->used += size;
            return p;
        }
        // Use aligned header size, matching Block::data() offset
        constexpr u64 kHdr = (sizeof(Block) + 15) & ~static_cast<u64>(15);
        if (size > static_cast<u64>(-1) - kHdr) return nullptr;
        u64 needed = size + kHdr;
        u64 new_size = block_size > needed ? block_size : needed;
        Block* b = alloc_block(new_size, current);
        if (!b) return nullptr;
        current = b;
        void* p = current->data() + current->used;
        current->used += size;
        return p;
    }

    // Typed bump alloc + placement new.
    template <typename T, typename... Args>
    T* alloc_t(Args&&... args) {
        void* p = alloc(sizeof(T));
        if (!p) return nullptr;
        return ::new (p) T(static_cast<Args&&>(args)...);
    }

    // Array bump alloc, value-initialized via placement new.
    template <typename T>
    T* alloc_array(u32 count) {
        if (count > 0 && sizeof(T) > static_cast<u64>(-1) / count) return nullptr;
        void* p = alloc(static_cast<u64>(sizeof(T)) * count);
        if (!p) return nullptr;
        auto* a = static_cast<T*>(p);
        for (u32 i = 0; i < count; i++) ::new (&a[i]) T{};
        return a;
    }

    // Free all blocks except first, reset pointer. O(blocks).
    void reset() {
        if (!current) return;
        Block* b = current;
        while (b->prev) {
            Block* prev = b->prev;
            u64 sz = b->size;
            munmap(b, sz);
            total_allocated -= sz;
            b = prev;
        }
        b->used = 0;
        current = b;
    }

    // Free everything.
    void destroy() {
        Block* b = current;
        while (b) {
            Block* prev = b->prev;
            munmap(b, b->size);
            b = prev;
        }
        current = nullptr;
        total_allocated = 0;
    }

    u64 space_used() const {
        u64 t = 0;
        for (Block* b = current; b; b = b->prev) t += b->used;
        return t;
    }

    u64 space_allocated() const { return total_allocated; }

private:
    static u64 page_align(u64 size) {
        if (size > static_cast<u64>(-1) - 4095) return 0;  // overflow guard
        return (size + 4095) & ~static_cast<u64>(4095);
    }

    Block* alloc_block(u64 size, Block* prev) {
        size = page_align(size);
        if (size == 0) {
            errno = ENOMEM;  // overflow in page_align
            return nullptr;
        }
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        auto* b = static_cast<Block*>(p);
        b->prev = prev;
        b->size = size;
        b->used = 0;
        total_allocated += size;
        return b;
    }
};

}  // namespace rut
