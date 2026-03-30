#include "rut/runtime/arena.h"

#include "rut/runtime/slice_pool.h"

namespace rut {

u8* SlicePoolBackend::acquire(u64 needed, u64* out_size) {
    if (!pool) return nullptr;
    if (needed > SlicePool::kSliceSize) return nullptr;  // won't fit in one slice
    u8* slice = pool->alloc();
    if (!slice) return nullptr;
    *out_size = SlicePool::kSliceSize;
    return slice;
}

void SlicePoolBackend::release(u8* ptr, u64 /*size*/) {
    if (pool) pool->free(ptr);
}

}  // namespace rut
