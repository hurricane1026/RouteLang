// Arena tests — comprehensive code path coverage.
// Ported scenarios from clapdb/Arena test suite where applicable,
// adapted to our no-stdlib Arena API.
#include "rout/runtime/arena.h"

#include "test.h"

using namespace rout;

// ============================================================
// Block internals
// ============================================================

TEST(block, data_starts_after_header) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* b = a.current;
    u64 hdr = (sizeof(Arena::Block) + 15) & ~u64(15);
    u8* expected = reinterpret_cast<u8*>(b) + hdr;
    CHECK_EQ(b->data(), expected);
    a.destroy();
}

TEST(block, capacity_equals_size_minus_header) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* b = a.current;
    u64 hdr = (sizeof(Arena::Block) + 15) & ~u64(15);
    CHECK_EQ(b->capacity(), b->size - hdr);
    a.destroy();
}

TEST(block, remaining_decreases_after_alloc) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    u64 r0 = a.current->remaining();
    a.alloc(100);
    u64 r1 = a.current->remaining();
    CHECK_EQ(r0 - r1, 104u);  // 100 aligned to 104
    a.destroy();
}

TEST(block, chain_integrity) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    Arena::Block* first = a.current;
    CHECK(first->prev == nullptr);

    // Force new block
    a.alloc(first->capacity());
    a.alloc(64);

    Arena::Block* second = a.current;
    CHECK(second != first);
    CHECK_EQ(second->prev, first);
    CHECK(first->prev == nullptr);
    a.destroy();
}

TEST(block, three_block_chain) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    Arena::Block* b0 = a.current;

    // Force 3 blocks by allocating more than one block can hold each time
    a.alloc(b0->capacity() + 64);  // overflow → b1
    Arena::Block* b1 = a.current;
    CHECK_NE(b1, b0);

    a.alloc(b1->capacity() + 64);  // overflow → b2
    Arena::Block* b2 = a.current;
    CHECK_NE(b2, b1);

    // Verify chain: b2→b1→b0→nullptr
    CHECK_EQ(b2->prev, b1);
    CHECK_EQ(b1->prev, b0);
    CHECK(b0->prev == nullptr);
    a.destroy();
}

// ============================================================
// Basic allocation (from clapdb ArenaTest.AllocateTest)
// ============================================================

TEST(arena, init_succeeds) {
    Arena a;
    CHECK_EQ(a.init(4096), 0);
    CHECK(a.current != nullptr);
    CHECK_GT(a.space_allocated(), 0u);
    a.destroy();
}

TEST(arena, init_min_256) {
    Arena a;
    REQUIRE_EQ(a.init(1), 0);
    CHECK_GE(a.current->size, 256u);
    a.destroy();
}

TEST(arena, small_alloc) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    void* p = a.alloc(64);
    CHECK(p != nullptr);
    CHECK_EQ(a.space_used(), 64u);
    a.destroy();
}

TEST(arena, alloc_returns_sequential_addresses) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    u8* p1 = static_cast<u8*>(a.alloc(16));
    u8* p2 = static_cast<u8*>(a.alloc(32));
    u8* p3 = static_cast<u8*>(a.alloc(8));
    CHECK_EQ(p2 - p1, 16);
    CHECK_EQ(p3 - p2, 32);
    a.destroy();
}

TEST(arena, multiple_allocs_in_one_block) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    void* p1 = a.alloc(100);
    void* p2 = a.alloc(200);
    void* p3 = a.alloc(300);
    CHECK(p1 && p2 && p3);
    CHECK_NE(p1, p2);
    CHECK_NE(p2, p3);
    // 100→104, 200→200, 300→304 (aligned)
    CHECK_EQ(a.space_used(), 104u + 200u + 304u);
    a.destroy();
}

// ============================================================
// Alignment (from clapdb AlignTest)
// ============================================================

TEST(arena, alignment_8byte) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    void* p1 = a.alloc(1);
    void* p2 = a.alloc(1);
    CHECK_EQ(reinterpret_cast<u64>(p1) % 8, 0u);
    CHECK_EQ(reinterpret_cast<u64>(p2) % 8, 0u);
    CHECK_EQ(static_cast<u8*>(p2) - static_cast<u8*>(p1), 8);
    a.destroy();
}

TEST(arena, alignment_various_sizes) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    for (u64 sz = 1; sz <= 64; sz++) {
        void* p = a.alloc(sz);
        CHECK_EQ(reinterpret_cast<u64>(p) % 8, 0u);
    }
    a.destroy();
}

TEST(arena, zero_size_alloc) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    void* p = a.alloc(0);
    CHECK(p != nullptr);
    a.destroy();
}

// ============================================================
// Block overflow (from clapdb ArenaTest.NewBlockTest)
// ============================================================

TEST(arena, overflow_to_new_block) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    u64 cap = a.current->capacity();
    a.alloc(cap);
    a.alloc(64);
    CHECK(a.current->prev != nullptr);
    CHECK_GE(a.space_allocated(), 256u * 2);
    a.destroy();
}

TEST(arena, large_alloc_gets_own_block) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    void* p = a.alloc(1024);
    CHECK(p != nullptr);
    CHECK_GE(a.current->size, 1024u + sizeof(Arena::Block));
    a.destroy();
}

TEST(arena, many_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    for (int i = 0; i < 100; i++) CHECK(a.alloc(200) != nullptr);
    int blocks = 0;
    for (Arena::Block* b = a.current; b; b = b->prev) blocks++;
    CHECK_GT(blocks, 1);
    a.destroy();
}

TEST(arena, alloc_exact_block_capacity) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    u64 cap = a.current->capacity();
    u64 aligned_cap = cap & ~static_cast<u64>(7);
    void* p = a.alloc(aligned_cap);
    CHECK(p != nullptr);
    CHECK_EQ(a.current->remaining(), cap - aligned_cap);
    a.destroy();
}

// ============================================================
// Reset (from clapdb ArenaTest.ResetTest, FreeBlocksExceptFirstTest)
// ============================================================

TEST(arena, reset_single_block_only) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    Arena::Block* first = a.current;
    a.alloc(100);
    a.alloc(200);
    CHECK_GT(a.space_used(), 0u);
    a.reset();
    CHECK_EQ(a.current, first);
    CHECK_EQ(a.current->used, 0u);
    CHECK_EQ(a.space_used(), 0u);
    a.destroy();
}

TEST(arena, reset_reuses_first_block) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    Arena::Block* first = a.current;
    a.alloc(100);
    a.reset();
    // Alloc again — should use same first block
    void* p = a.alloc(64);
    CHECK(p != nullptr);
    CHECK_EQ(a.current, first);
    a.destroy();
}

TEST(arena, reset_frees_extra_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    u64 initial = a.space_allocated();
    for (int i = 0; i < 10; i++) a.alloc(4000);
    CHECK_GT(a.space_allocated(), initial);
    a.reset();
    CHECK_EQ(a.space_allocated(), initial);
    CHECK(a.current->prev == nullptr);
    a.destroy();
}

TEST(arena, reset_total_allocated_tracking) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    u64 initial = a.space_allocated();
    // Create 3 extra blocks
    for (int i = 0; i < 3; i++) {
        a.alloc(a.current->capacity());
        a.alloc(64);
    }
    u64 peak = a.space_allocated();
    CHECK_GT(peak, initial);
    a.reset();
    CHECK_EQ(a.space_allocated(), initial);
    a.destroy();
}

TEST(arena, reset_then_alloc_10_cycles) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 20; i++) CHECK(a.alloc(100) != nullptr);
        a.reset();
        CHECK_EQ(a.space_used(), 0u);
    }
    a.destroy();
}

// ============================================================
// Typed alloc (from clapdb ArenaTest.CreateTest, CreateArrayTest)
// ============================================================

struct Point {
    i32 x, y;
};

TEST(arena, alloc_t_pod) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* p = a.alloc_t<Point>(10, 20);
    CHECK(p != nullptr);
    CHECK_EQ(p->x, 10);
    CHECK_EQ(p->y, 20);
    a.destroy();
}

struct Counter {
    i32 value;
    Counter() : value(42) {}
};

TEST(arena, alloc_t_with_constructor) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* c = a.alloc_t<Counter>();
    CHECK(c != nullptr);
    CHECK_EQ(c->value, 42);
    a.destroy();
}

struct Large {
    u8 data[512];
};

TEST(arena, alloc_t_large_struct) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* l = a.alloc_t<Large>();
    CHECK(l != nullptr);
    // Write pattern and verify
    for (int i = 0; i < 512; i++) l->data[i] = static_cast<u8>(i & 0xFF);
    for (int i = 0; i < 512; i++) CHECK_EQ(l->data[i], static_cast<u8>(i & 0xFF));
    a.destroy();
}

TEST(arena, alloc_t_multiple_types) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* p = a.alloc_t<Point>(1, 2);
    auto* c = a.alloc_t<Counter>();
    auto* l = a.alloc_t<Large>();
    CHECK(p && c && l);
    CHECK_EQ(p->x, 1);
    CHECK_EQ(c->value, 42);
    a.destroy();
}

TEST(arena, alloc_array_int) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* arr = a.alloc_array<i32>(10);
    CHECK(arr != nullptr);
    for (int i = 0; i < 10; i++) CHECK_EQ(arr[i], 0);
    for (int i = 0; i < 10; i++) arr[i] = i * 10;
    for (int i = 0; i < 10; i++) CHECK_EQ(arr[i], i * 10);
    a.destroy();
}

TEST(arena, alloc_array_struct) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* arr = a.alloc_array<Point>(5);
    CHECK(arr != nullptr);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(arr[i].x, 0);
        CHECK_EQ(arr[i].y, 0);
    }
    a.destroy();
}

TEST(arena, alloc_array_zero_count) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* arr = a.alloc_array<i32>(0);
    CHECK(arr != nullptr);  // zero-count returns valid pointer
    a.destroy();
}

// ============================================================
// Destroy (from clapdb ArenaTest.DstrTest, FreeBlocksTest)
// ============================================================

TEST(arena, destroy_frees_all) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    for (int i = 0; i < 20; i++) a.alloc(200);
    a.destroy();
    CHECK(a.current == nullptr);
    CHECK_EQ(a.total_allocated, 0u);
}

TEST(arena, double_destroy_safe) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.alloc(100);
    a.destroy();
    a.destroy();
    CHECK(a.current == nullptr);
}

TEST(arena, destroy_single_block) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.alloc(64);
    a.destroy();
    CHECK(a.current == nullptr);
    CHECK_EQ(a.total_allocated, 0u);
}

// ============================================================
// Metrics (from clapdb ArenaTest.SpaceTest, RemainsTest)
// ============================================================

TEST(arena, space_used_single_block) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.alloc(100);  // → 104
    a.alloc(8);
    CHECK_EQ(a.space_used(), 104u + 8u);
    a.destroy();
}

TEST(arena, space_used_across_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    u64 cap = a.current->capacity();
    a.alloc(cap);  // fill first block
    a.alloc(64);   // second block
    // First block: cap used. Second block: 64 used.
    CHECK_EQ(a.space_used(), cap + 64u);
    a.destroy();
}

TEST(arena, space_allocated_grows_with_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    u64 a0 = a.space_allocated();
    a.alloc(a.current->capacity());
    a.alloc(64);
    u64 a1 = a.space_allocated();
    CHECK_GT(a1, a0);
    a.destroy();
}

TEST(arena, space_used_zero_after_reset) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.alloc(500);
    CHECK_GT(a.space_used(), 0u);
    a.reset();
    CHECK_EQ(a.space_used(), 0u);
    a.destroy();
}

// ============================================================
// Stress tests
// ============================================================

TEST(arena, stress_10k_small_allocs) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    for (int i = 0; i < 10000; i++) CHECK(a.alloc(8) != nullptr);
    CHECK_EQ(a.space_used(), 80000u);
    a.destroy();
}

TEST(arena, stress_mixed_sizes) {
    Arena a;
    REQUIRE_EQ(a.init(1024), 0);
    for (int i = 0; i < 1000; i++) {
        u64 sz = static_cast<u64>((i % 7 + 1) * 8);  // 8,16,24,32,40,48,56
        CHECK(a.alloc(sz) != nullptr);
    }
    a.destroy();
}

TEST(arena, stress_alloc_reset_1000_cycles) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    for (int cycle = 0; cycle < 1000; cycle++) {
        a.alloc(128);
        a.alloc(256);
        a.alloc(64);
        a.reset();
    }
    CHECK_EQ(a.space_used(), 0u);
    a.destroy();
}

TEST(arena, stress_large_allocs_across_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    // Each alloc forces a new block
    for (int i = 0; i < 50; i++) CHECK(a.alloc(4000) != nullptr);
    int blocks = 0;
    for (Arena::Block* b = a.current; b; b = b->prev) blocks++;
    CHECK_GE(blocks, 50);
    a.destroy();
}

TEST(arena, stress_reset_preserves_first_block_across_cycles) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    Arena::Block* first = a.current;
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 0; i < 10; i++) a.alloc(100);
        a.reset();
        CHECK_EQ(a.current, first);
    }
    a.destroy();
}

// ============================================================
// Data integrity — write then read back
// ============================================================

TEST(arena, data_integrity_after_alloc) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    auto* buf = static_cast<u8*>(a.alloc(256));
    REQUIRE(buf != nullptr);
    for (int i = 0; i < 256; i++) buf[i] = static_cast<u8>(i);
    for (int i = 0; i < 256; i++) CHECK_EQ(buf[i], static_cast<u8>(i));
    a.destroy();
}

TEST(arena, data_integrity_across_blocks) {
    Arena a;
    REQUIRE_EQ(a.init(256), 0);
    // Fill first block, force second
    u64 cap = a.current->capacity();
    auto* buf1 = static_cast<u8*>(a.alloc(cap));
    for (u64 i = 0; i < cap; i++) buf1[i] = static_cast<u8>(i & 0xFF);

    auto* buf2 = static_cast<u8*>(a.alloc(128));
    for (int i = 0; i < 128; i++) buf2[i] = static_cast<u8>(i + 100);

    // Verify first block data still intact
    for (u64 i = 0; i < cap; i++) CHECK_EQ(buf1[i], static_cast<u8>(i & 0xFF));
    // Verify second block
    for (int i = 0; i < 128; i++) CHECK_EQ(buf2[i], static_cast<u8>(i + 100));
    a.destroy();
}

// === Null guard tests (Copilot round 6) ===

// alloc() on uninitialized Arena (current==nullptr) must return nullptr, not crash.
// Without the null guard, this would segfault.
TEST(arena, alloc_before_init_returns_null) {
    Arena a;
    a.current = nullptr;
    CHECK(a.alloc(64) == nullptr);
}

// alloc() after destroy (current==nullptr) must return nullptr.
TEST(arena, alloc_after_destroy_returns_null) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.destroy();
    CHECK(a.alloc(64) == nullptr);
}

// reset() after destroy (current==nullptr) must not crash.
TEST(arena, reset_after_destroy_no_crash) {
    Arena a;
    REQUIRE_EQ(a.init(4096), 0);
    a.destroy();
    a.reset();  // must be a no-op, not a crash
    CHECK(a.current == nullptr);
}

// reset() on uninitialized Arena must not crash.
TEST(arena, reset_before_init_no_crash) {
    Arena a;
    a.current = nullptr;
    a.reset();
    CHECK(a.current == nullptr);
}

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}
