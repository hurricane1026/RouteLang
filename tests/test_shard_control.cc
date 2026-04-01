// Per-shard independent control system tests.
#include "rut/runtime/epoll_event_loop.h"
#include "rut/runtime/route_table.h"
#include "rut/runtime/shard.h"
#include "rut/runtime/shard_control.h"
#include "rut/runtime/socket.h"
#include "test.h"
#include "test_helpers.h"

#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

using namespace rut;

// === ShardControlBlock tests ===

TEST(shard_control, control_block_init_zero) {
    ShardControlBlock cb{};
    CHECK(cb.pending_config == nullptr);
    CHECK(cb.pending_jit == nullptr);
}

TEST(shard_control, control_block_config_write_read) {
    ShardControlBlock cb{};
    RouteConfig cfg;
    cfg.route_count = 42;

    cb.pending_config.store(&cfg, std::memory_order_release);

    CHECK(cb.pending_config.load(std::memory_order_acquire) == &cfg);
    CHECK_EQ(cb.pending_config.load(std::memory_order_acquire)->route_count, 42u);
}

TEST(shard_control, control_block_jit_write_read) {
    ShardControlBlock cb{};
    u8 fake_code = 0xCC;

    cb.pending_jit.store(static_cast<void*>(&fake_code), std::memory_order_release);

    CHECK(cb.pending_jit.load(std::memory_order_acquire) == &fake_code);
}

TEST(shard_control, control_block_alignas) {
    // Verify ShardControlBlock is cache-line aligned.
    CHECK_EQ(alignof(ShardControlBlock), 64u);
}

// === ShardEpoch tests ===

TEST(shard_control, epoch_starts_zero) {
    ShardEpoch ep{};
    CHECK_EQ(ep.epoch, 0u);
}

TEST(shard_control, epoch_enter_leave_increments) {
    // Use a real EventLoop with epoll backend to test epoch_enter/leave.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    REQUIRE(loop->init(0, lfd).has_value());

    ShardEpoch ep{};
    ep.epoch = 0;
    loop->epoch = &ep;

    loop->epoch_enter();
    CHECK_EQ(ep.epoch, 1u);  // enter incremented

    loop->epoch_leave();
    CHECK_EQ(ep.epoch, 2u);  // leave incremented

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, epoch_noop_when_null) {
    // epoch_enter/leave must be zero-cost when epoch pointer is null.
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    CHECK(loop->epoch == nullptr);
    // Should not crash or modify anything.
    loop->epoch_enter();
    loop->epoch_leave();

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, epoch_alignas) {
    CHECK_EQ(alignof(ShardEpoch), 64u);
}

// === poll_command tests (using SmallLoop-style approach) ===

TEST(shard_control, poll_command_noop_when_null) {
    // When control is null, poll_command is a no-op.
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    CHECK(loop->control == nullptr);
    loop->poll_command();  // should not crash

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, poll_command_reload) {
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    const RouteConfig* active_config = nullptr;
    ShardControlBlock cb{};
    RouteConfig cfg;
    cfg.route_count = 7;

    loop->config_ptr = &active_config;
    loop->control = &cb;

    // Write config update.
    cb.pending_config.store(&cfg, std::memory_order_release);

    loop->poll_command();

    CHECK(active_config == &cfg);
    CHECK_EQ(active_config->route_count, 7u);
    CHECK(cb.pending_config == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, poll_command_swap_jit) {
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    void* jit_code = nullptr;
    ShardControlBlock cb{};
    loop->control = &cb;
    loop->jit_code_ptr = &jit_code;

    u8 fake_code = 0xCC;
    cb.pending_jit.store(static_cast<void*>(&fake_code), std::memory_order_release);

    loop->poll_command();

    CHECK(jit_code == &fake_code);
    CHECK(cb.pending_jit == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, poll_command_none_is_noop) {
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    const RouteConfig* active_config = nullptr;
    ShardControlBlock cb{};
    loop->config_ptr = &active_config;
    loop->control = &cb;

    // Both pending are nullptr — poll_command should not modify active_config.
    loop->poll_command();
    CHECK(active_config == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, poll_command_simultaneous_config_and_jit) {
    // Both pending_config and pending_jit are set. Both should be applied in one poll.
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    const RouteConfig* active_config = nullptr;
    void* jit_code = nullptr;
    ShardControlBlock cb{};

    loop->config_ptr = &active_config;
    loop->jit_code_ptr = &jit_code;
    loop->control = &cb;

    RouteConfig cfg;
    cfg.route_count = 55;
    u8 fake_jit = 0x90;

    cb.pending_config.store(&cfg, std::memory_order_release);
    cb.pending_jit.store(static_cast<void*>(&fake_jit), std::memory_order_release);

    loop->poll_command();

    CHECK(active_config == &cfg);
    CHECK_EQ(active_config->route_count, 55u);
    CHECK(jit_code == &fake_jit);
    CHECK(cb.pending_config == nullptr);
    CHECK(cb.pending_jit == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

// === Shard integration tests ===

TEST(shard_control, shard_init_wiring) {
    // Verify control block is initialized and wired into EventLoop.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    auto rc = shard.init(0, lfd);
    REQUIRE(rc.has_value());

    CHECK(shard.control.pending_config == nullptr);
    CHECK(shard.control.pending_jit == nullptr);
    CHECK(shard.loop->control == &shard.control);
    CHECK(shard.loop->epoch == &shard.epoch);
    CHECK(shard.loop->config_ptr == &shard.active_config);
    CHECK(shard.loop->jit_code_ptr == &shard.jit_code);

    shard.shutdown();
    close(lfd);
}

TEST(shard_control, shard_reload_config) {
    // Spawn a real shard, send ReloadConfig, verify active_config changes.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    auto rc = shard.init(0, lfd);
    REQUIRE(rc.has_value());
    CHECK(shard.active_config == nullptr);

    auto spawn_rc = shard.spawn();
    REQUIRE(spawn_rc.has_value());

    // Allocate a RouteConfig via mmap (no malloc).
    void* cfg_mem = mmap(
        nullptr, sizeof(RouteConfig), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(cfg_mem != MAP_FAILED);
    auto* cfg = new (cfg_mem) RouteConfig();
    cfg->route_count = 99;

    shard.reload_config(cfg);

    // Give the shard thread time to process the command.
    // Poll until active_config is updated or timeout after ~100ms.
    for (u32 i = 0; i < 2000; i++) {
        const RouteConfig* cur = shard.active_config;
        if (cur == cfg) break;
        usleep(1000);
    }

    const RouteConfig* cur = shard.active_config;
    CHECK(cur == cfg);

    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
    munmap(cfg_mem, sizeof(RouteConfig));
}

TEST(shard_control, shard_swap_jit) {
    // Spawn a real shard, send SwapJit, verify jit_code changes.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    auto rc = shard.init(0, lfd);
    REQUIRE(rc.has_value());
    CHECK(shard.jit_code == nullptr);

    auto spawn_rc = shard.spawn();
    REQUIRE(spawn_rc.has_value());

    u8 fake_jit = 0x90;  // NOP
    shard.swap_jit(&fake_jit);

    // Poll until jit_code is updated.
    for (u32 i = 0; i < 2000; i++) {
        void* cur = shard.jit_code;
        if (cur == &fake_jit) break;
        usleep(1000);
    }

    void* cur = shard.jit_code;
    CHECK(cur == &fake_jit);

    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
}

// === Epoch through real request cycles (SmallLoop) ===

TEST(shard_control, epoch_odd_during_request) {
    // Accept + recv triggers on_header_received which calls epoch_enter.
    // Verify epoch is odd after recv (in-request), even after send (request done).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    CHECK_EQ(ep.epoch, 0u);  // no request yet

    // Recv triggers on_header_received → epoch_enter.
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 1u);  // enter incremented
    CHECK_EQ(c->state, ConnState::Sending);

    // Send completion triggers on_response_sent → epoch_leave.
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(ep.epoch, 2u);  // leave incremented
    CHECK_EQ(c->state, ConnState::ReadingHeader);
}

TEST(shard_control, epoch_across_keepalive) {
    // Two full request cycles. Monotonic epoch: each cycle = +2 (enter+leave).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);

    // First request cycle: enter=1, leave=2.
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 1u);
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(ep.epoch, 2u);

    // Second request cycle: enter=3, leave=4.
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 3u);
    loop.inject_and_dispatch(
        make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
    CHECK_EQ(ep.epoch, 4u);
}

TEST(shard_control, epoch_on_error_close) {
    // Accept, recv, inject a Send error (result < 0).
    // Verify epoch_leave was called even on the error path (epoch goes back to even).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    u32 cid = c->id;

    // Recv → epoch_enter (epoch = 1).
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 1u);

    // Send error → close_conn_impl calls epoch_leave (req_start_us != 0).
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -32));
    CHECK_EQ(ep.epoch, 2u);            // leave incremented (enter=1, leave=2)
    CHECK_EQ(loop.conns[cid].fd, -1);  // connection closed
}

// === Config pointer through request cycle ===

TEST(shard_control, config_ptr_readable_in_loop) {
    // Set loop.config_ptr to point to a local RouteConfig*.
    // Verify *loop.config_ptr reads the config.
    SmallLoop loop;
    loop.setup();

    RouteConfig cfg;
    cfg.route_count = 77;
    const RouteConfig* active = &cfg;
    loop.config_ptr = &active;

    CHECK(loop.config_ptr != nullptr);
    CHECK(*loop.config_ptr == &cfg);
    CHECK_EQ((*loop.config_ptr)->route_count, 77u);
}

TEST(shard_control, reload_config_updates_ptr) {
    // Set initial config, poll_command with pending_config pointing to a new config.
    SmallLoop loop;
    loop.setup();

    RouteConfig cfg1;
    cfg1.route_count = 10;
    RouteConfig cfg2;
    cfg2.route_count = 20;

    const RouteConfig* active = &cfg1;
    ShardControlBlock cb{};

    loop.config_ptr = &active;
    loop.control = &cb;

    CHECK(active == &cfg1);

    // Send config update.
    cb.pending_config.store(&cfg2, std::memory_order_release);
    loop.poll_command();

    CHECK(active == &cfg2);
    CHECK_EQ(active->route_count, 20u);
    CHECK(cb.pending_config == nullptr);
}

// === Command sequencing ===

TEST(shard_control, sequential_commands) {
    // Send config update, consume it, then send JIT update, consume it.
    SmallLoop loop;
    loop.setup();

    RouteConfig cfg;
    cfg.route_count = 42;
    const RouteConfig* active = nullptr;
    void* jit_code = nullptr;
    ShardControlBlock cb{};

    loop.config_ptr = &active;
    loop.jit_code_ptr = &jit_code;
    loop.control = &cb;

    // First: config reload.
    cb.pending_config.store(&cfg, std::memory_order_release);
    loop.poll_command();
    CHECK(active == &cfg);
    CHECK(cb.pending_config == nullptr);

    // Second: JIT swap.
    u8 fake_jit = 0x90;
    cb.pending_jit.store(static_cast<void*>(&fake_jit), std::memory_order_release);
    loop.poll_command();
    CHECK(jit_code == &fake_jit);
    CHECK(cb.pending_jit == nullptr);

    // Both were applied.
    CHECK(active == &cfg);
    CHECK(jit_code == &fake_jit);
}

// === Stop behavior ===

TEST(shard_control, stop_does_not_affect_other_shards) {
    // Create two real shards. Stop one. Verify the other is still running.
    auto lfd1_result = create_listen_socket(0);
    REQUIRE(lfd1_result.has_value());
    i32 lfd1 = lfd1_result.value();

    auto lfd2_result = create_listen_socket(0);
    REQUIRE(lfd2_result.has_value());
    i32 lfd2 = lfd2_result.value();

    Shard<EpollEventLoop> shard1;
    Shard<EpollEventLoop> shard2;
    REQUIRE(shard1.init(0, lfd1).has_value());
    REQUIRE(shard2.init(1, lfd2).has_value());

    REQUIRE(shard1.spawn().has_value());
    REQUIRE(shard2.spawn().has_value());

    // Stop shard1.
    shard1.stop();
    shard1.join();

    // shard2 should still be running.
    CHECK(!shard1.loop->is_running());
    CHECK(shard2.loop->is_running());

    shard2.stop();
    shard2.join();

    shard1.shutdown();
    shard2.shutdown();
    close(lfd1);
    close(lfd2);
}

// === JIT pointer ===

TEST(shard_control, jit_swap_updates_ptr) {
    // Set jit_code_ptr, send JIT updates, verify the pointer was updated.
    SmallLoop loop;
    loop.setup();

    void* jit_code = nullptr;
    ShardControlBlock cb{};
    loop.jit_code_ptr = &jit_code;
    loop.control = &cb;

    u8 code_a = 0xCC;
    u8 code_b = 0x90;

    // First swap.
    cb.pending_jit.store(static_cast<void*>(&code_a), std::memory_order_release);
    loop.poll_command();
    CHECK(jit_code == &code_a);

    // Second swap overwrites.
    cb.pending_jit.store(static_cast<void*>(&code_b), std::memory_order_release);
    loop.poll_command();
    CHECK(jit_code == &code_b);
}

// === Wake timeliness (real shard) ===

TEST(shard_control, command_processed_within_timer_tick) {
    // Spawn a real shard, send ReloadConfig with known config pointer.
    // Shard processes commands on each event loop iteration (timer tick = 1s).
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    REQUIRE(shard.init(0, lfd).has_value());
    CHECK(shard.active_config == nullptr);

    REQUIRE(shard.spawn().has_value());

    void* cfg_mem = mmap(
        nullptr, sizeof(RouteConfig), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(cfg_mem != MAP_FAILED);
    auto* cfg = new (cfg_mem) RouteConfig();
    cfg->route_count = 123;

    shard.reload_config(cfg);

    // Sleep 1.5s — shard processes on next timer tick (~1s interval).
    struct timespec ts = {1, 500000000L};
    nanosleep(&ts, nullptr);

    const RouteConfig* cur = shard.active_config;
    CHECK(cur == cfg);
    CHECK_EQ(cur->route_count, 123u);

    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
    munmap(cfg_mem, sizeof(RouteConfig));
}

// === Edge cases ===

TEST(shard_control, poll_command_without_config_ptr) {
    // control is set but config_ptr is null. Send config update.
    // poll_command should not crash (null check on config_ptr).
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    ShardControlBlock cb{};
    loop->control = &cb;
    loop->config_ptr = nullptr;  // explicitly null

    RouteConfig cfg;
    cfg.route_count = 50;
    cb.pending_config.store(&cfg, std::memory_order_release);

    // Should not crash — pending_config with null config_ptr is a no-op.
    loop->poll_command();
    CHECK(cb.pending_config == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

TEST(shard_control, poll_command_without_jit_ptr) {
    // control is set but jit_code_ptr is null. Send JIT update.
    // poll_command should not crash (null check on jit_code_ptr).
    RealLoop* loop = create_real_loop();
    REQUIRE(loop != nullptr);
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(loop->init(0, lfd).has_value());

    ShardControlBlock cb{};
    loop->control = &cb;
    loop->jit_code_ptr = nullptr;  // explicitly null

    u8 fake_code = 0xCC;
    cb.pending_jit.store(static_cast<void*>(&fake_code), std::memory_order_release);

    // Should not crash — pending_jit with null jit_code_ptr is a no-op.
    loop->poll_command();
    CHECK(cb.pending_jit == nullptr);

    loop->shutdown();
    close(lfd);
    destroy_real_loop(loop);
}

// === Direct apply when shard not running ===

TEST(shard_control, reload_before_spawn_applies_directly) {
    // reload_config before spawn should set active_config directly.
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    auto rc = s.init(0, lfd);
    REQUIRE(rc.has_value());

    RouteConfig cfg;
    s.reload_config(&cfg);
    CHECK_EQ(s.active_config, &cfg);  // applied directly

    s.shutdown();
    close(lfd);
}

TEST(shard_control, swap_jit_before_spawn_applies_directly) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    auto rc = s.init(0, lfd);
    REQUIRE(rc.has_value());

    i32 dummy = 42;
    s.swap_jit(&dummy);
    CHECK_EQ(s.jit_code, &dummy);  // applied directly

    s.shutdown();
    close(lfd);
}

TEST(shard_control, reload_after_stop_applies_directly) {
    // Spawn, stop, join, then reload — should not hang.
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    auto rc = s.init(0, lfd);
    REQUIRE(rc.has_value());

    s.spawn(false);
    s.stop();
    s.join();

    RouteConfig cfg;
    s.reload_config(&cfg);  // must not hang
    CHECK_EQ(s.active_config, &cfg);

    s.shutdown();
    close(lfd);
}

// === Seed active_config from route_config in spawn ===

TEST(shard_control, spawn_seeds_active_config_from_route_config) {
    // route_config is set after init(), before spawn().
    // active_config should reflect it after spawn.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> s;
    auto rc = s.init(0, lfd);
    REQUIRE(rc.has_value());

    // route_config set AFTER init, BEFORE spawn (standard pattern).
    RouteConfig cfg;
    cfg.route_count = 42;
    s.route_config = &cfg;

    REQUIRE(s.spawn(false).has_value());

    // active_config should be seeded from route_config.
    CHECK_EQ(s.active_config, &cfg);

    s.stop();
    s.join();
    s.shutdown();
    close(lfd);
}

// === Reload between stop and join ===

TEST(shard_control, reload_after_stop_before_join) {
    // stop() → reload_config() → join(): must not deadlock,
    // and the reload must take effect.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> s;
    REQUIRE(s.init(0, lfd).has_value());
    REQUIRE(s.spawn(false).has_value());

    s.stop();
    // Shard thread is exiting or has exited.
    // Sleep briefly to let it finish its final iteration.
    struct timespec ts = {0, 100000000L};  // 100ms
    nanosleep(&ts, nullptr);

    RouteConfig new_cfg;
    new_cfg.route_count = 99;
    s.reload_config(&new_cfg);  // must not deadlock

    s.join();

    // After join, reload should have taken effect.
    CHECK_EQ(s.active_config, &new_cfg);

    s.shutdown();
    close(lfd);
}

// === Stale command drained on join ===

TEST(shard_control, join_clears_stale_pending_without_overwrite) {
    // reload(A) → stop → reload(B) overwrites A in pending → join applies B.
    // With atomic pointer exchange, B is the latest store. join() picks it up.
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> s;
    REQUIRE(s.init(0, lfd).has_value());
    REQUIRE(s.spawn(false).has_value());

    RouteConfig old_cfg;
    old_cfg.route_count = 77;
    s.reload_config(&old_cfg);  // queue via control block (thread_spawned=true)
    s.stop();

    // thread_spawned is still true, so this overwrites old_cfg in pending_config.
    RouteConfig new_cfg;
    new_cfg.route_count = 99;
    s.reload_config(&new_cfg);

    s.join();  // applies whatever is pending (new_cfg, since it overwrote old_cfg)

    CHECK_EQ(s.active_config, &new_cfg);
    CHECK_EQ(s.active_config->route_count, 99u);

    s.shutdown();
    close(lfd);
}

// === Epoch via timer close ===

TEST(shard_control, epoch_leave_on_timer_close) {
    // Accept a connection, recv (epoch_enter → epoch=1). Add connection to
    // timer with timeout=0 (current slot). Dispatch a Timeout event to fire
    // timer.tick(), which closes the connection via close_conn → epoch_leave.
    // Verify epoch=2 (enter + leave).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    // Accept a connection.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    CHECK_EQ(ep.epoch, 0u);

    // Recv triggers on_header_received → epoch_enter.
    u32 cid = c->id;
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 1u);
    CHECK_EQ(c->state, ConnState::Sending);

    // Move the connection to the current timer slot (timeout=0 means current
    // cursor slot). Refresh with 0 places it at cursor+0 = current slot.
    loop.timer.refresh(c, 0);

    // Dispatch a Timeout event. timer.tick() advances cursor, closing
    // connections in the current slot via close_conn → epoch_leave.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Timeout, 1));
    CHECK_EQ(ep.epoch, 2u);

    // Connection should be closed.
    CHECK_EQ(loop.conns[cid].fd, -1);
}

// === Epoch with concurrent requests ===

TEST(shard_control, epoch_with_concurrent_requests) {
    // Accept two connections. Recv on both (two epoch_enters → epoch=2).
    // Send complete on first (epoch_leave → epoch=3).
    // Send complete on second (epoch_leave → epoch=4).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    // Accept two connections.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c1 = loop.find_fd(42);
    REQUIRE(c1 != nullptr);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    auto* c2 = loop.find_fd(43);
    REQUIRE(c2 != nullptr);

    CHECK_EQ(ep.epoch, 0u);

    // Recv on first → epoch_enter (epoch=1).
    loop.inject_and_dispatch(make_ev(c1->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 1u);

    // Recv on second → epoch_enter (epoch=2).
    loop.inject_and_dispatch(make_ev(c2->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 2u);

    // Send complete on first → epoch_leave (epoch=3).
    loop.inject_and_dispatch(
        make_ev(c1->id, IoEventType::Send, static_cast<i32>(c1->send_buf.len())));
    CHECK_EQ(ep.epoch, 3u);

    // Send complete on second → epoch_leave (epoch=4).
    loop.inject_and_dispatch(
        make_ev(c2->id, IoEventType::Send, static_cast<i32>(c2->send_buf.len())));
    CHECK_EQ(ep.epoch, 4u);
}

// === Epoch monotonic under load ===

TEST(shard_control, epoch_monotonic_under_load) {
    // Do 5 full request cycles (accept, recv, send). Verify epoch = 10 (5 * 2).
    // Demonstrates monotonic advancement under steady traffic (the property
    // that lets RCU work).
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    for (u32 i = 0; i < 5; i++) {
        i32 fake_fd = static_cast<i32>(100 + i);
        loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, fake_fd));
        auto* c = loop.find_fd(fake_fd);
        REQUIRE(c != nullptr);

        // Recv → epoch_enter.
        loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 50));
        CHECK_EQ(ep.epoch, i * 2 + 1);

        // Send → epoch_leave.
        loop.inject_and_dispatch(
            make_ev(c->id, IoEventType::Send, static_cast<i32>(c->send_buf.len())));
        CHECK_EQ(ep.epoch, i * 2 + 2);
    }

    CHECK_EQ(ep.epoch, 10u);
}

// === Config pointer updates after multiple reloads ===

TEST(shard_control, config_ptr_updates_after_reload) {
    // Wire SmallLoop with config_ptr → local RouteConfig*. Set initial config.
    // Send config update with new config. Call poll_command. Verify
    // *config_ptr now points to new config. Send ANOTHER update. Verify
    // updated again.
    SmallLoop loop;
    loop.setup();

    RouteConfig cfg1;
    cfg1.route_count = 10;
    RouteConfig cfg2;
    cfg2.route_count = 20;
    RouteConfig cfg3;
    cfg3.route_count = 30;

    const RouteConfig* active = &cfg1;
    ShardControlBlock cb{};

    loop.config_ptr = &active;
    loop.control = &cb;

    CHECK(active == &cfg1);
    CHECK_EQ(active->route_count, 10u);

    // First reload: cfg1 → cfg2.
    cb.pending_config.store(&cfg2, std::memory_order_release);
    loop.poll_command();

    CHECK(active == &cfg2);
    CHECK_EQ(active->route_count, 20u);
    CHECK(cb.pending_config == nullptr);

    // Second reload: cfg2 → cfg3.
    cb.pending_config.store(&cfg3, std::memory_order_release);
    loop.poll_command();

    CHECK(active == &cfg3);
    CHECK_EQ(active->route_count, 30u);
    CHECK(cb.pending_config == nullptr);
}

// === Swap JIT after stop before join ===

TEST(shard_control, swap_jit_after_stop_before_join) {
    // Create real shard, spawn, stop, sleep 100ms, swap_jit(new ptr), join.
    // Verify jit_code == new ptr. (Mirrors reload_after_stop_before_join
    // but for JIT.)
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn(false).has_value());

    shard.stop();
    // Shard thread is exiting or has exited.
    // Sleep briefly to let it finish its final iteration.
    struct timespec ts = {0, 100000000L};  // 100ms
    nanosleep(&ts, nullptr);

    u8 fake_jit = 0x90;         // NOP
    shard.swap_jit(&fake_jit);  // must not deadlock

    shard.join();

    // After join, swap should have taken effect.
    CHECK(shard.jit_code == &fake_jit);

    shard.shutdown();
    close(lfd);
}

// === Fire-and-forget overwrite: last write wins ===

TEST(shard_control, consecutive_reload_last_wins) {
    // Two rapid reload_config calls before shard polls.
    // Only the second config should be applied.
    SmallLoop loop;
    loop.setup();

    const RouteConfig* active = nullptr;
    ShardControlBlock cb{};
    loop.config_ptr = &active;
    loop.control = &cb;

    RouteConfig cfg1;
    cfg1.route_count = 11;
    RouteConfig cfg2;
    cfg2.route_count = 22;

    // Two writes, no poll in between — second overwrites first.
    cb.pending_config.store(&cfg1, std::memory_order_release);
    cb.pending_config.store(&cfg2, std::memory_order_release);

    loop.poll_command();
    CHECK_EQ(active, &cfg2);
    CHECK_EQ(active->route_count, 22u);
}

TEST(shard_control, consecutive_swap_jit_last_wins) {
    SmallLoop loop;
    loop.setup();

    void* active_jit = nullptr;
    ShardControlBlock cb{};
    loop.jit_code_ptr = &active_jit;
    loop.control = &cb;

    i32 jit1 = 1;
    i32 jit2 = 2;

    cb.pending_jit.store(static_cast<void*>(&jit1), std::memory_order_release);
    cb.pending_jit.store(static_cast<void*>(&jit2), std::memory_order_release);

    loop.poll_command();
    CHECK_EQ(active_jit, &jit2);
}

// === Lifecycle: reload after join applies directly ===

TEST(shard_control, reload_after_join_applies_directly) {
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> s;
    REQUIRE(s.init(0, lfd).has_value());
    REQUIRE(s.spawn(false).has_value());
    s.stop();
    s.join();

    // After join, thread_spawned=false. reload should apply directly.
    RouteConfig cfg;
    cfg.route_count = 88;
    s.reload_config(&cfg);
    CHECK_EQ(s.active_config, &cfg);

    s.shutdown();
    close(lfd);
}

// === Epoch: force_close_all during drain deadline ===

TEST(shard_control, epoch_leave_on_force_close_all) {
    // Simulate drain deadline: force_close_all closes all connections.
    // Each connection with req_start_us != 0 should get epoch_leave.
    SmallLoop loop;
    loop.setup();

    ShardEpoch ep{};
    ep.epoch = 0;
    loop.epoch = &ep;

    // Accept two connections, start requests on both.
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    auto* c1 = loop.find_fd(42);
    auto* c2 = loop.find_fd(43);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);

    // Recv on both → epoch_enter on each (epoch = 2).
    loop.inject_and_dispatch(make_ev(c1->id, IoEventType::Recv, 50));
    loop.inject_and_dispatch(make_ev(c2->id, IoEventType::Recv, 50));
    CHECK_EQ(ep.epoch, 2u);

    // Force-close both (simulates drain deadline).
    // close_conn_impl calls epoch_leave for each (req_start_us != 0).
    loop.close_conn(*c1);
    loop.close_conn(*c2);
    CHECK_EQ(ep.epoch, 4u);  // 2 enters + 2 leaves
    CHECK_EQ(loop.conns[c1->id].fd, -1);
    CHECK_EQ(loop.conns[c2->id].fd, -1);
}

// === Fire-and-forget: real shard config+jit simultaneous ===

TEST(shard_control, real_shard_simultaneous_config_and_jit) {
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();

    Shard<EpollEventLoop> shard;
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn(false).has_value());

    void* cfg_mem = mmap(
        nullptr, sizeof(RouteConfig), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(cfg_mem != MAP_FAILED);
    auto* cfg = new (cfg_mem) RouteConfig();
    cfg->route_count = 55;

    i32 fake_jit = 99;

    // Fire both simultaneously.
    shard.reload_config(cfg);
    shard.swap_jit(&fake_jit);

    // Poll until both applied (up to 2s).
    for (u32 i = 0; i < 2000; i++) {
        const RouteConfig* cur_cfg = shard.active_config;
        void* cur_jit = shard.jit_code;
        if (cur_cfg == cfg && cur_jit == &fake_jit) break;
        usleep(1000);
    }

    CHECK_EQ(shard.active_config, cfg);
    CHECK_EQ(shard.jit_code, static_cast<void*>(&fake_jit));

    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
    cfg->~RouteConfig();
    munmap(cfg_mem, sizeof(RouteConfig));
}

// === Shard capture control ===

TEST(shard_capture, enable_before_spawn_applies_directly) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(s.init(0, lfd).has_value());
    CaptureRing* ring = s.enable_capture();
    REQUIRE(ring != nullptr);
    CHECK_EQ(s.capture_ring, ring);
    CHECK_EQ(s.loop->capture_ring, ring);
    s.shutdown();
    close(lfd);
}

TEST(shard_capture, enable_returns_existing_if_already_enabled) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(s.init(0, lfd).has_value());
    CaptureRing* ring1 = s.enable_capture();
    CaptureRing* ring2 = s.enable_capture();
    CHECK_EQ(ring1, ring2);
    s.shutdown();
    close(lfd);
}

TEST(shard_capture, disable_before_spawn_clears_ring) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(s.init(0, lfd).has_value());
    s.enable_capture();
    CHECK(s.loop->capture_ring != nullptr);
    s.disable_capture();
    CHECK(s.loop->capture_ring == nullptr);
    CHECK(s.capture_ring != nullptr);
    s.shutdown();
    close(lfd);
}

TEST(shard_capture, free_capture_ring_idempotent) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(s.init(0, lfd).has_value());
    s.free_capture_ring();
    CHECK(s.capture_ring == nullptr);
    s.enable_capture();
    s.free_capture_ring();
    s.free_capture_ring();
    CHECK(s.capture_ring == nullptr);
    s.shutdown();
    close(lfd);
}

TEST(shard_capture, enable_after_spawn_via_control_block) {
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    Shard<EpollEventLoop> shard;
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn().has_value());
    CaptureRing* ring = shard.enable_capture();
    REQUIRE(ring != nullptr);
    for (u32 i = 0; i < 2000; i++) {
        if (shard.loop->capture_ring == ring) break;
        usleep(1000);
    }
    CHECK_EQ(shard.loop->capture_ring, ring);
    CHECK(shard.loop->capture_region_ != nullptr);
    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
}

TEST(shard_capture, disable_after_spawn_via_control_block) {
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    Shard<EpollEventLoop> shard;
    REQUIRE(shard.init(0, lfd).has_value());
    REQUIRE(shard.spawn().has_value());
    CaptureRing* ring = shard.enable_capture();
    REQUIRE(ring != nullptr);
    for (u32 i = 0; i < 2000; i++) {
        if (shard.loop->capture_ring == ring) break;
        usleep(1000);
    }
    shard.disable_capture();
    for (u32 i = 0; i < 2000; i++) {
        if (shard.loop->capture_ring == nullptr) break;
        usleep(1000);
    }
    CHECK(shard.loop->capture_ring == nullptr);
    CHECK(shard.loop->capture_region_ != nullptr);
    shard.stop();
    shard.join();
    shard.shutdown();
    close(lfd);
}

TEST(shard_capture, shutdown_cleans_up_capture) {
    Shard<EpollEventLoop> s;
    auto lfd_result = create_listen_socket(0);
    REQUIRE(lfd_result.has_value());
    i32 lfd = lfd_result.value();
    REQUIRE(s.init(0, lfd).has_value());
    s.enable_capture();
    CHECK(s.capture_ring != nullptr);
    s.shutdown();
    CHECK(s.capture_ring == nullptr);
    close(lfd);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
