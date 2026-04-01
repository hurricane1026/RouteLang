// Tests for traffic capture: CaptureEntry, CaptureRing, file I/O, and
// integration with the mock event loop (capture through callback pipeline).
#include "rut/runtime/traffic_capture.h"
#include "test.h"
#include "test_helpers.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace rut;

// === CaptureEntry layout ===

TEST(capture_entry, size) {
    CHECK_EQ(sizeof(CaptureEntry), 8256u);
}

TEST(capture_entry, metadata_offset) {
    // raw_headers starts at offset 64
    CaptureEntry entry{};
    auto* base = reinterpret_cast<u8*>(&entry);
    auto* headers = reinterpret_cast<u8*>(&entry.raw_headers);
    CHECK_EQ(static_cast<u32>(headers - base), 64u);
}

// === CaptureRing ===

TEST(capture_ring, push_pop_single) {
    CaptureRing ring;
    ring.init();
    CHECK_EQ(ring.available(), 0u);

    CaptureEntry entry{};
    entry.resp_status = 200;
    entry.method = static_cast<u8>(LogHttpMethod::Get);
    const char* path = "GET / HTTP/1.1\r\nHost: test\r\n\r\n";
    u32 path_len = 0;
    while (path[path_len]) path_len++;
    __builtin_memcpy(entry.raw_headers, path, path_len);
    entry.raw_header_len = static_cast<u16>(path_len);

    CHECK(ring.push(entry));
    CHECK_EQ(ring.available(), 1u);

    CaptureEntry out{};
    CHECK(ring.pop(out));
    CHECK_EQ(out.resp_status, 200);
    CHECK_EQ(out.method, static_cast<u8>(LogHttpMethod::Get));
    CHECK_EQ(out.raw_header_len, static_cast<u16>(path_len));
    CHECK_EQ(ring.available(), 0u);
}

TEST(capture_ring, pop_empty) {
    CaptureRing ring;
    ring.init();
    CaptureEntry out{};
    CHECK(!ring.pop(out));
}

TEST(capture_ring, full_drops) {
    CaptureRing ring;
    ring.init();

    CaptureEntry entry{};
    for (u32 i = 0; i < CaptureRing::kCapacity; i++) {
        entry.resp_status = static_cast<u16>(i);
        CHECK(ring.push(entry));
    }
    CHECK_EQ(ring.available(), CaptureRing::kCapacity);

    // Ring full — push should fail
    entry.resp_status = 999;
    CHECK(!ring.push(entry));

    // Pop one, push should succeed again
    CaptureEntry out{};
    CHECK(ring.pop(out));
    CHECK_EQ(out.resp_status, 0);  // first entry
    CHECK(ring.push(entry));
}

TEST(capture_ring, fifo_order) {
    CaptureRing ring;
    ring.init();

    CaptureEntry entry{};
    for (u32 i = 0; i < 10; i++) {
        entry.resp_status = static_cast<u16>(100 + i);
        ring.push(entry);
    }

    CaptureEntry out{};
    for (u32 i = 0; i < 10; i++) {
        CHECK(ring.pop(out));
        CHECK_EQ(out.resp_status, static_cast<u16>(100 + i));
    }
}

// === File header ===

TEST(capture_file, header_init_valid) {
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    CHECK(capture_file_header_valid(&hdr));
    CHECK_EQ(hdr.version, 1u);
    CHECK_EQ(hdr.entry_size, static_cast<u32>(sizeof(CaptureEntry)));
    CHECK_EQ(hdr.entry_count, 0u);
}

TEST(capture_file, header_invalid_magic) {
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.magic[0] = 'X';
    CHECK(!capture_file_header_valid(&hdr));
}

// === File I/O round-trip ===

TEST(capture_file, write_read_roundtrip) {
    // Create temp file
    char path[] = "/tmp/rut_capture_test_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    // Write header
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 3;
    ssize_t hw = write(fd, &hdr, sizeof(hdr));
    CHECK_EQ(static_cast<u32>(hw), static_cast<u32>(sizeof(hdr)));

    // Write 3 entries
    for (u32 i = 0; i < 3; i++) {
        CaptureEntry entry{};
        entry.resp_status = static_cast<u16>(200 + i);
        entry.method = static_cast<u8>(LogHttpMethod::Get);
        entry.raw_header_len = 5;
        entry.raw_headers[0] = 'G';
        entry.raw_headers[1] = 'E';
        entry.raw_headers[2] = 'T';
        entry.raw_headers[3] = ' ';
        entry.raw_headers[4] = '/';
        CHECK_EQ(capture_write_entry(fd, entry), 0);
    }

    // Seek back and read
    lseek(fd, 0, SEEK_SET);

    CaptureFileHeader read_hdr;
    ssize_t hr = read(fd, &read_hdr, sizeof(read_hdr));
    CHECK_EQ(static_cast<u32>(hr), static_cast<u32>(sizeof(read_hdr)));
    CHECK(capture_file_header_valid(&read_hdr));
    CHECK_EQ(read_hdr.entry_count, 3u);

    for (u32 i = 0; i < 3; i++) {
        CaptureEntry out{};
        CHECK_EQ(capture_read_entry(fd, out), 0);
        CHECK_EQ(out.resp_status, static_cast<u16>(200 + i));
        CHECK_EQ(out.raw_header_len, 5);
        CHECK_EQ(out.raw_headers[0], static_cast<u8>('G'));
    }

    close(fd);
    unlink(path);
}

// === Integration: capture through mock event loop ===

TEST(capture_integration, basic_request_captured) {
    // Allocate ring on heap (too large for stack: 256 * 8256 = ~2MB)
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    // Accept connection
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    // capture_buf should be wired
    CHECK(conn->capture_buf != nullptr);

    // Inject a valid HTTP request into recv_buf
    conn->recv_buf.reset();
    const char req[] = "GET /api/test HTTP/1.1\r\nHost: example.com\r\n\r\n";
    u32 req_len = sizeof(req) - 1;
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), req_len);

    // Dispatch recv — triggers on_header_received → capture_stage_headers
    // Use direct callback dispatch (recv_buf already has data)
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(req_len));
    // Don't use inject_and_dispatch (it overwrites recv_buf with mock bytes).
    // Instead, inject directly and dispatch manually.
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // After on_header_received, capture_buf should have the headers staged
    CHECK_GT(conn->capture_header_len, 0);

    // Complete the send — triggers on_response_sent → on_request_complete → capture write
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Ring should have one entry
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    CHECK(ring->pop(cap));
    CHECK_EQ(cap.resp_status, 200);
    CHECK_EQ(cap.method, static_cast<u8>(LogHttpMethod::Get));
    CHECK_GT(cap.raw_header_len, 0);

    // Verify raw headers contain the request line
    bool found_get = cap.raw_headers[0] == 'G' &&
                     cap.raw_headers[1] == 'E' &&
                     cap.raw_headers[2] == 'T';
    CHECK(found_get);

    munmap(ring, sizeof(CaptureRing));
}

TEST(capture_integration, no_capture_when_disabled) {
    SmallLoop loop;
    loop.setup();
    // capture_ring is nullptr — no capture

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    CHECK(conn->capture_buf == nullptr);

    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Recv, 50));
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
    // No crash, no capture — just verify it doesn't blow up
}

TEST(capture_integration, keepalive_captures_both) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Two keep-alive request cycles
    for (int cycle = 0; cycle < 2; cycle++) {
        conn->recv_buf.reset();
        const char req[] = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);

        IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
        loop.backend.inject(recv_ev);
        IoEvent events[8];
        u32 n = loop.backend.wait(events, 8);
        for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

        loop.inject_and_dispatch(
            make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
    }

    // Both requests captured
    CHECK_EQ(ring->available(), 2u);

    munmap(ring, sizeof(CaptureRing));
}

// === Runtime enable/disable via control block ===

TEST(capture_control, enable_via_control_block) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    // Wire control block
    ShardControlBlock ctrl{};
    ctrl.pending_capture.store(nullptr, std::memory_order_relaxed);
    loop.control = &ctrl;

    // Initially no capture
    CHECK(loop.capture_ring == nullptr);

    // Enable via control block
    ctrl.pending_capture.store(ring, std::memory_order_release);
    loop.poll_command();
    CHECK_EQ(loop.capture_ring, ring);

    munmap(ring, sizeof(CaptureRing));
}

TEST(capture_control, disable_via_control_block) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    ShardControlBlock ctrl{};
    ctrl.pending_capture.store(nullptr, std::memory_order_relaxed);
    loop.control = &ctrl;

    // Enable
    ctrl.pending_capture.store(ring, std::memory_order_release);
    loop.poll_command();
    CHECK_EQ(loop.capture_ring, ring);

    // Disable
    ctrl.pending_capture.store(kCaptureDisable, std::memory_order_release);
    loop.poll_command();
    CHECK(loop.capture_ring == nullptr);

    munmap(ring, sizeof(CaptureRing));
}

TEST(capture_control, enable_captures_disable_stops) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    // Accept + request with capture on
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Manually set capture_buf (SmallLoop wires it from capture_storage)
    // Already wired by alloc_conn_impl since capture_ring is set

    conn->recv_buf.reset();
    const char req[] = "GET /on HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    CHECK_EQ(ring->available(), 1u);

    // Disable capture
    loop.set_capture(nullptr);

    // Second request — should NOT be captured
    conn->recv_buf.reset();
    const char req2[] = "GET /off HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req2), sizeof(req2) - 1);
    recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req2) - 1));
    loop.backend.inject(recv_ev);
    n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Still only 1 entry
    CHECK_EQ(ring->available(), 1u);

    munmap(ring, sizeof(CaptureRing));
}

TEST(capture_control, conn_before_enable_gets_backfilled) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    // Wire control block
    ShardControlBlock ctrl{};
    ctrl.pending_capture.store(nullptr, std::memory_order_relaxed);
    loop.control = &ctrl;

    // Accept connection BEFORE capture is enabled
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    CHECK(conn->capture_buf == nullptr);  // no capture yet

    // Enable capture via control block
    ctrl.pending_capture.store(ring, std::memory_order_release);
    loop.poll_command();
    CHECK_EQ(loop.capture_ring, ring);

    // Backfill should have set capture_buf on the existing connection
    CHECK(conn->capture_buf != nullptr);

    // Now send a request — it should be captured
    conn->recv_buf.reset();
    const char req[] = "GET /backfill HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Should have captured the request
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    CHECK(ring->pop(cap));
    CHECK_EQ(cap.resp_status, 200);
    // Verify raw headers contain "/backfill"
    bool found = false;
    for (u32 i = 0; i + 8 < cap.raw_header_len; i++) {
        if (cap.raw_headers[i] == '/' && cap.raw_headers[i + 1] == 'b') {
            found = true;
            break;
        }
    }
    CHECK(found);

    munmap(ring, sizeof(CaptureRing));
}

// --- State transition tests ---
// Cover all capture on/off × connection lifecycle combinations.

// Helper: send a request on an existing connection, bypassing inject_and_dispatch
// (which overwrites recv_buf with mock bytes). Returns send_buf length for send completion.
static u32 send_request(SmallLoop& loop, Connection& conn, const char* req_str) {
    u32 req_len = 0;
    while (req_str[req_len]) req_len++;

    conn.recv_buf.reset();
    conn.recv_buf.write(reinterpret_cast<const u8*>(req_str), req_len);

    IoEvent recv_ev = make_ev(conn.id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    u32 send_len = conn.send_buf.len();
    loop.inject_and_dispatch(make_ev(conn.id, IoEventType::Send, static_cast<i32>(send_len)));
    return send_len;
}

// 2. on → accept → off → request: disable after accept, request not captured
TEST(capture_transition, on_accept_off_request) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    // Accept while capture is on — capture_buf is wired
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    CHECK(conn->capture_buf != nullptr);

    // Disable capture
    loop.set_capture(nullptr);

    // Request should NOT be captured (capture_ring is null at completion)
    send_request(loop, *conn, "GET /after-disable HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 0u);

    munmap(ring, sizeof(CaptureRing));
}

// 3. on → accept → off → on → request: re-enable backfills, request captured
TEST(capture_transition, on_off_on_reenable_backfill) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    ShardControlBlock ctrl{};
    ctrl.pending_capture.store(nullptr, std::memory_order_relaxed);
    loop.control = &ctrl;

    // Enable, accept, disable
    ctrl.pending_capture.store(ring, std::memory_order_release);
    loop.poll_command();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    CHECK(conn->capture_buf != nullptr);

    ctrl.pending_capture.store(kCaptureDisable, std::memory_order_release);
    loop.poll_command();
    CHECK(loop.capture_ring == nullptr);

    // Accept a second connection while capture is off
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    auto* conn2 = loop.find_fd(43);
    REQUIRE(conn2 != nullptr);
    CHECK(conn2->capture_buf == nullptr);  // no capture during off

    // Re-enable — both connections should get backfilled
    ctrl.pending_capture.store(ring, std::memory_order_release);
    loop.poll_command();
    CHECK_EQ(loop.capture_ring, ring);
    CHECK(conn->capture_buf != nullptr);
    CHECK(conn2->capture_buf != nullptr);  // backfilled

    // Requests on both should be captured
    send_request(loop, *conn, "GET /re1 HTTP/1.1\r\nHost: x\r\n\r\n");
    send_request(loop, *conn2, "GET /re2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);

    munmap(ring, sizeof(CaptureRing));
}

// 4. Disable mid-request: headers staged (capture on), but completion sees capture off.
//    Should not crash, request silently not captured.
TEST(capture_transition, disable_mid_request) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Send request headers — triggers on_header_received, stages headers
    conn->recv_buf.reset();
    const char req[] = "GET /mid HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Headers are staged
    CHECK_GT(conn->capture_header_len, 0);

    // Disable capture BETWEEN header recv and send completion
    loop.set_capture(nullptr);

    // Complete the send — on_request_complete sees capture_ring == nullptr
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Not captured (ring was disabled at completion time)
    CHECK_EQ(ring->available(), 0u);
    // Connection still alive (keep-alive)
    CHECK_EQ(conn->state, ConnState::ReadingHeader);

    munmap(ring, sizeof(CaptureRing));
}

// 5. Mixed: conn1 accepted while off, conn2 accepted while on.
//    set_capture() backfills conn1, so BOTH should be captured.
TEST(capture_transition, mixed_conns_both_captured_after_enable) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    // capture off initially

    // Accept conn1 while capture off
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn1 = loop.find_fd(42);
    REQUIRE(conn1 != nullptr);
    CHECK(conn1->capture_buf == nullptr);

    // Enable capture — backfills conn1
    loop.set_capture(ring);
    CHECK(conn1->capture_buf != nullptr);  // backfilled

    // Accept conn2 while capture on
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    auto* conn2 = loop.find_fd(43);
    REQUIRE(conn2 != nullptr);
    CHECK(conn2->capture_buf != nullptr);

    // Both requests captured
    send_request(loop, *conn1, "GET /c1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    send_request(loop, *conn2, "GET /c2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);

    munmap(ring, sizeof(CaptureRing));
}

// 6. Close connection after disable — no crash.
TEST(capture_transition, close_after_disable) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;

    // Disable capture, then close — should not crash
    loop.set_capture(nullptr);
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Recv, 0));  // EOF → close
    CHECK_EQ(loop.conns[cid].fd, -1);

    munmap(ring, sizeof(CaptureRing));
}

// 7. Keep-alive: capture off for req1, on for req2. req2 must NOT
//    contain stale header data from req1.
TEST(capture_transition, keepalive_off_then_on_no_stale) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Request 1 with capture on — establishes header_len
    send_request(loop, *conn, "GET /req1 HTTP/1.1\r\nHost: a\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    // Disable capture, send request 2
    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /req2-off HTTP/1.1\r\nHost: b\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);  // still 1

    // Re-enable capture, send request 3
    loop.set_capture(ring);
    send_request(loop, *conn, "GET /req3 HTTP/1.1\r\nHost: c\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);  // 1 from req1 + 1 from req3

    // Verify req3's capture has the correct headers (not req1's stale data)
    CaptureEntry cap1{}, cap3{};
    ring->pop(cap1);  // req1
    ring->pop(cap3);  // req3

    // req3 headers should contain "/req3", not "/req1"
    bool found_req3 = false;
    for (u32 i = 0; i + 4 < cap3.raw_header_len; i++) {
        if (cap3.raw_headers[i] == '/' && cap3.raw_headers[i + 1] == 'r' &&
            cap3.raw_headers[i + 2] == 'e' && cap3.raw_headers[i + 3] == 'q' &&
            cap3.raw_headers[i + 4] == '3') {
            found_req3 = true;
            break;
        }
    }
    CHECK(found_req3);

    munmap(ring, sizeof(CaptureRing));
}

// 8. The exact interleaving bug: capture on for header recv, off at completion,
//    then re-enabled before next request. Stale capture_header_len from req1
//    must not leak into the next request's capture entry.
TEST(capture_transition, stale_header_len_across_disable_reenable) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Request 1: headers staged (capture on), then disable before send completion
    conn->recv_buf.reset();
    const char r1[] = "GET /staged HTTP/1.1\r\nHost: x\r\nX-Big: aaaa\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(r1), sizeof(r1) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(r1) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_GT(conn->capture_header_len, 0);  // headers staged
    u16 stale_len = conn->capture_header_len;

    // Disable capture, complete the send
    loop.set_capture(nullptr);
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
    CHECK_EQ(ring->available(), 0u);  // not captured (disabled at completion)

    // Re-enable capture
    loop.set_capture(ring);

    // Request 2: shorter headers. capture_header_len must reflect req2, not req1.
    send_request(loop, *conn, "GET /s HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    ring->pop(cap);
    // req2 headers are shorter than req1's stale_len
    CHECK(cap.raw_header_len < stale_len);
    CHECK_EQ(cap.resp_status, 200);

    munmap(ring, sizeof(CaptureRing));
}

// 9. Ring overflow: push more entries than capacity, verify no crash,
//    oldest entries survive, overflow entries are dropped.
TEST(capture_ring, overflow_pressure) {
    CaptureRing ring;
    ring.init();

    CaptureEntry entry{};
    entry.method = static_cast<u8>(LogHttpMethod::Post);

    // Fill ring completely
    for (u32 i = 0; i < CaptureRing::kCapacity; i++) {
        entry.resp_status = static_cast<u16>(i);
        CHECK(ring.push(entry));
    }
    CHECK_EQ(ring.available(), CaptureRing::kCapacity);

    // Overflow: next 100 pushes should all fail
    for (u32 i = 0; i < 100; i++) {
        entry.resp_status = static_cast<u16>(9000 + i);
        CHECK(!ring.push(entry));
    }
    CHECK_EQ(ring.available(), CaptureRing::kCapacity);

    // Drain half, then push should succeed again
    CaptureEntry out{};
    for (u32 i = 0; i < CaptureRing::kCapacity / 2; i++) {
        CHECK(ring.pop(out));
        CHECK_EQ(out.resp_status, static_cast<u16>(i));  // FIFO preserved
    }
    CHECK_EQ(ring.available(), CaptureRing::kCapacity / 2);

    // Push again — should succeed
    for (u32 i = 0; i < CaptureRing::kCapacity / 2; i++) {
        entry.resp_status = static_cast<u16>(5000 + i);
        CHECK(ring.push(entry));
    }
    CHECK_EQ(ring.available(), CaptureRing::kCapacity);
}

// ============================================================
// Systematic cross-state coverage
// ============================================================
//
// Notation: each request is labeled by capture state at
//   (header_received, request_complete)
// e.g. "on/on" = capture on at both points = captured
//      "on/off" = staged but not written
//      "off/on" = not staged, header_len=0, not written
//      "off/off" = not captured
//
// Keep-alive sequences test 2-3 requests per connection.

// --- A. Keep-alive toggle sequences ---

// A1: off → off → on (3 requests, capture only on for req3)
TEST(capture_cross, keepalive_off_off_on) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    // Accept while off
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // req1: off
    send_request(loop, *conn, "GET /r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 0u);

    // req2: still off
    send_request(loop, *conn, "GET /r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 0u);

    // Enable, req3: on
    loop.set_capture(ring);
    send_request(loop, *conn, "GET /r3 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    ring->pop(cap);
    CHECK_EQ(cap.resp_status, 200);
    // Verify it's req3's headers, not stale from req1/req2
    bool found = false;
    for (u32 i = 0; i + 2 < cap.raw_header_len; i++) {
        if (cap.raw_headers[i] == 'r' && cap.raw_headers[i + 1] == '3') {
            found = true;
            break;
        }
    }
    CHECK(found);

    munmap(ring, sizeof(CaptureRing));
}

// A2: off → on → off (3 requests, only req2 captured)
TEST(capture_cross, keepalive_off_on_off) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // req1: off
    send_request(loop, *conn, "GET /r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 0u);

    // Enable, req2: on
    loop.set_capture(ring);
    send_request(loop, *conn, "GET /r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    // Disable, req3: off
    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /r3 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);  // still 1

    munmap(ring, sizeof(CaptureRing));
}

// A3: on → on → off (3 requests, req1+req2 captured, req3 not)
TEST(capture_cross, keepalive_on_on_off) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    send_request(loop, *conn, "GET /r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    send_request(loop, *conn, "GET /r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);

    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /r3 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);  // still 2

    munmap(ring, sizeof(CaptureRing));
}

// A4: on → off → off (3 requests, only req1 captured)
TEST(capture_cross, keepalive_on_off_off) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    send_request(loop, *conn, "GET /r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    send_request(loop, *conn, "GET /r3 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    munmap(ring, sizeof(CaptureRing));
}

// A5: Double toggle: on → off → on → off (4 requests)
TEST(capture_cross, keepalive_double_toggle) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    send_request(loop, *conn, "GET /a HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);  // on

    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);  // off

    loop.set_capture(ring);
    send_request(loop, *conn, "GET /c HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);  // on

    loop.set_capture(nullptr);
    send_request(loop, *conn, "GET /d HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);  // off

    // Verify captured entries have correct headers
    CaptureEntry cap_a{}, cap_c{};
    ring->pop(cap_a);
    ring->pop(cap_c);

    bool found_a = false, found_c = false;
    for (u32 i = 0; i + 1 < cap_a.raw_header_len; i++)
        if (cap_a.raw_headers[i] == '/' && cap_a.raw_headers[i + 1] == 'a') found_a = true;
    for (u32 i = 0; i + 1 < cap_c.raw_header_len; i++)
        if (cap_c.raw_headers[i] == '/' && cap_c.raw_headers[i + 1] == 'c') found_c = true;
    CHECK(found_a);
    CHECK(found_c);

    munmap(ring, sizeof(CaptureRing));
}

// --- B. Per-request state: off at header, on at complete ---
// (enable between header_received and request_complete)
// Should NOT capture — header_len is 0, even though ring is now set.

TEST(capture_cross, enable_between_header_and_complete) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    // capture OFF initially

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Recv request while capture OFF — headers not staged
    conn->recv_buf.reset();
    const char req[] = "GET /between HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_EQ(conn->capture_header_len, 0);  // not staged

    // Enable capture BETWEEN header recv and send completion
    loop.set_capture(ring);
    CHECK(conn->capture_buf != nullptr);  // backfilled

    // Complete the send
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    // Should NOT capture — capture_header_len is 0
    CHECK_EQ(ring->available(), 0u);

    // Next request SHOULD be captured normally
    send_request(loop, *conn, "GET /next HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    munmap(ring, sizeof(CaptureRing));
}

// --- C. Multi-connection interleaved enable/disable ---

TEST(capture_cross, two_conns_alternating_capture) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 43));
    auto* c1 = loop.find_fd(42);
    auto* c2 = loop.find_fd(43);
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);

    // req on c1 (on) — captured
    send_request(loop, *c1, "GET /c1r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    // Disable, req on c2 (off) — not captured
    loop.set_capture(nullptr);
    send_request(loop, *c2, "GET /c2r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    // Re-enable, req on c2 (on) — captured
    loop.set_capture(ring);
    send_request(loop, *c2, "GET /c2r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 2u);

    // req on c1 (on) — captured
    send_request(loop, *c1, "GET /c1r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 3u);

    munmap(ring, sizeof(CaptureRing));
}

// --- D. Cross with drain mode ---

TEST(capture_cross, capture_during_drain) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Enable drain mode
    loop.draining = true;

    // Request during drain — should still be captured, response is "Connection: close"
    send_request(loop, *conn, "GET /drain HTTP/1.1\r\nHost: x\r\n\r\n");

    // Connection closed (drain sends Connection: close, keep_alive=false)
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    ring->pop(cap);
    CHECK_EQ(cap.resp_status, 200);  // still 200, just with Connection: close

    munmap(ring, sizeof(CaptureRing));
}

// --- E. Close with staged headers (capture on at header, conn reset before completion) ---

TEST(capture_cross, close_with_staged_headers) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    u32 cid = conn->id;

    // Recv request — headers staged
    conn->recv_buf.reset();
    const char req[] = "GET /close HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    CHECK_GT(conn->capture_header_len, 0);

    // Send fails → connection close (before on_request_complete writes capture)
    loop.inject_and_dispatch(make_ev(cid, IoEventType::Send, -32));  // EPIPE
    CHECK_EQ(loop.conns[cid].fd, -1);  // closed

    // Headers were staged but never written to ring (close bypasses on_request_complete)
    CHECK_EQ(ring->available(), 0u);

    munmap(ring, sizeof(CaptureRing));
}

// --- F. Proxy flow with capture ---

TEST(capture_cross, proxy_cycle_captured) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    // Accept + recv request
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    conn->recv_buf.reset();
    const char req[] = "GET /proxy HTTP/1.1\r\nHost: upstream\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Headers should be staged
    CHECK_GT(conn->capture_header_len, 0);

    // Switch to proxy mode
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    conn->state = ConnState::Proxying;
    loop.submit_connect(*conn, nullptr, 0);

    // Proxy cycle: connect → send → recv response → send to client
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, 0));
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::Send, static_cast<i32>(sizeof(req) - 1)));
    inject_upstream_response(loop, *conn);

    // Complete: send proxy response to client
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(kMockHttpResponseLen)));

    // Proxy response captured with correct headers
    CHECK_EQ(ring->available(), 1u);

    CaptureEntry cap{};
    ring->pop(cap);
    CHECK_EQ(cap.resp_status, 200);
    // Verify raw headers contain "/proxy"
    bool found = false;
    for (u32 i = 0; i + 5 < cap.raw_header_len; i++) {
        if (cap.raw_headers[i] == '/' && cap.raw_headers[i + 1] == 'p' &&
            cap.raw_headers[i + 2] == 'r') {
            found = true;
            break;
        }
    }
    CHECK(found);

    munmap(ring, sizeof(CaptureRing));
}

// Proxy 502 (upstream connect failure) with capture
TEST(capture_cross, proxy_502_captured) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    conn->recv_buf.reset();
    const char req[] = "GET /fail HTTP/1.1\r\nHost: x\r\n\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(req), sizeof(req) - 1);
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(sizeof(req) - 1));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    // Proxy connect fails → 502
    conn->upstream_fd = 100;
    conn->on_complete = &on_upstream_connected<SmallLoop>;
    loop.inject_and_dispatch(make_ev(conn->id, IoEventType::UpstreamConnect, -111));

    // 502 response sent to client
    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    CHECK_EQ(ring->available(), 1u);
    CaptureEntry cap{};
    ring->pop(cap);
    CHECK_EQ(cap.resp_status, 502);

    munmap(ring, sizeof(CaptureRing));
}

// --- G. Ring overflow with live event loop ---

TEST(capture_cross, ring_overflow_live) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Send enough requests to overflow the ring (capacity = 256)
    for (u32 i = 0; i < CaptureRing::kCapacity + 10; i++) {
        send_request(loop, *conn, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    }

    // Ring should be full at capacity, no crash
    CHECK_EQ(ring->available(), CaptureRing::kCapacity);

    // Drain some, then new requests succeed
    CaptureEntry out{};
    for (u32 i = 0; i < 10; i++) ring->pop(out);
    CHECK_EQ(ring->available(), CaptureRing::kCapacity - 10);

    send_request(loop, *conn, "GET /after HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), CaptureRing::kCapacity - 10 + 1);

    munmap(ring, sizeof(CaptureRing));
}

// ============================================================
// End-to-end and failure path verification
// ============================================================

// H1. Full pipeline: event loop → ring → file → read back → verify headers
TEST(capture_e2e, pipeline_to_file) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Three distinct requests
    const char* reqs[] = {
        "GET /users HTTP/1.1\r\nHost: api.example.com\r\nAccept: application/json\r\n\r\n",
        "POST /login HTTP/1.1\r\nHost: api.example.com\r\nContent-Length: 0\r\n\r\n",
        "DELETE /session HTTP/1.1\r\nHost: api.example.com\r\nAuthorization: Bearer tok\r\n\r\n",
    };
    for (int i = 0; i < 3; i++) {
        send_request(loop, *conn, reqs[i]);
    }
    CHECK_EQ(ring->available(), 3u);

    // Write to temp file
    char path[] = "/tmp/rut_e2e_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 3;
    ssize_t hw = write(fd, &hdr, sizeof(hdr));
    CHECK_EQ(static_cast<u32>(hw), static_cast<u32>(sizeof(hdr)));

    CaptureEntry entry{};
    for (u32 i = 0; i < 3; i++) {
        CHECK(ring->pop(entry));
        CHECK_EQ(capture_write_entry(fd, entry), 0);
    }
    CHECK_EQ(ring->available(), 0u);

    // Read back and verify
    lseek(fd, 0, SEEK_SET);

    CaptureFileHeader read_hdr;
    ssize_t hr = read(fd, &read_hdr, sizeof(read_hdr));
    CHECK_EQ(static_cast<u32>(hr), static_cast<u32>(sizeof(read_hdr)));
    CHECK(capture_file_header_valid(&read_hdr));
    CHECK_EQ(read_hdr.entry_count, 3u);

    // Verify each entry's headers match what was sent
    const char* expected_paths[] = {"/users", "/login", "/session"};
    const u8 expected_methods[] = {
        static_cast<u8>(LogHttpMethod::Get),
        static_cast<u8>(LogHttpMethod::Post),
        static_cast<u8>(LogHttpMethod::Delete),
    };

    for (u32 i = 0; i < 3; i++) {
        CaptureEntry cap{};
        CHECK_EQ(capture_read_entry(fd, cap), 0);
        CHECK_EQ(cap.resp_status, 200);
        CHECK_EQ(cap.method, expected_methods[i]);
        CHECK_GT(cap.raw_header_len, 0);

        // Find expected path in raw headers
        u32 plen = 0;
        while (expected_paths[i][plen]) plen++;
        bool found = false;
        for (u32 j = 0; j + plen <= cap.raw_header_len; j++) {
            bool match = true;
            for (u32 k = 0; k < plen; k++) {
                if (cap.raw_headers[j + k] != static_cast<u8>(expected_paths[i][k])) {
                    match = false;
                    break;
                }
            }
            if (match) { found = true; break; }
        }
        CHECK(found);
    }

    // EOF — no more entries
    CaptureEntry eof_cap{};
    CHECK_EQ(capture_read_entry(fd, eof_cap), -1);

    close(fd);
    unlink(path);
    munmap(ring, sizeof(CaptureRing));
}

// H2. Empty file — read returns -1 immediately
TEST(capture_e2e, read_empty_file) {
    char path[] = "/tmp/rut_empty_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    // Write only header, no entries
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 0;
    write(fd, &hdr, sizeof(hdr));

    lseek(fd, sizeof(hdr), SEEK_SET);
    CaptureEntry cap{};
    CHECK_EQ(capture_read_entry(fd, cap), -1);  // no entries

    close(fd);
    unlink(path);
}

// H3. Partial write recovery — truncated entry detected on read
TEST(capture_e2e, truncated_entry_read_fails) {
    char path[] = "/tmp/rut_trunc_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 1;
    write(fd, &hdr, sizeof(hdr));

    // Write only half an entry
    CaptureEntry entry{};
    entry.resp_status = 200;
    write(fd, &entry, sizeof(entry) / 2);

    lseek(fd, sizeof(hdr), SEEK_SET);
    CaptureEntry cap{};
    CHECK_EQ(capture_read_entry(fd, cap), -1);  // incomplete → error

    close(fd);
    unlink(path);
}

// H4. set_capture idempotent — calling twice with same ring doesn't double-backfill
TEST(capture_e2e, set_capture_idempotent) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);
    CHECK(conn->capture_buf == nullptr);

    // First set_capture — backfills
    loop.set_capture(ring);
    CHECK(conn->capture_buf != nullptr);
    u8* buf1 = conn->capture_buf;

    // Second set_capture with same ring — should not change capture_buf
    loop.set_capture(ring);
    CHECK_EQ(conn->capture_buf, buf1);  // same pointer, not double-assigned

    // Request works correctly
    send_request(loop, *conn, "GET /idem HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring->available(), 1u);

    munmap(ring, sizeof(CaptureRing));
}

// H5. Disable then enable with different ring — old entries stay in old ring
TEST(capture_e2e, switch_ring) {
    auto* ring1 = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    auto* ring2 = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring1 != MAP_FAILED);
    REQUIRE(ring2 != MAP_FAILED);
    ring1->init();
    ring2->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring1);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Request to ring1
    send_request(loop, *conn, "GET /r1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring1->available(), 1u);
    CHECK_EQ(ring2->available(), 0u);

    // Switch to ring2
    loop.set_capture(ring2);

    // Request to ring2
    send_request(loop, *conn, "GET /r2 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_EQ(ring1->available(), 1u);  // unchanged
    CHECK_EQ(ring2->available(), 1u);

    munmap(ring1, sizeof(CaptureRing));
    munmap(ring2, sizeof(CaptureRing));
}

// H6. Large header near 8KB limit — verify truncation flag
TEST(capture_e2e, large_header_near_limit) {
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop loop;
    loop.setup();
    loop.set_capture(ring);

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = loop.find_fd(42);
    REQUIRE(conn != nullptr);

    // Build a request with many headers that stays under SmallLoop's 4KB buf
    // (SmallLoop uses 4KB buffers, so we can't test the full 8KB path here,
    // but we can verify capture works near the buffer limit)
    conn->recv_buf.reset();
    const char prefix[] = "GET /big HTTP/1.1\r\nHost: x\r\n";
    conn->recv_buf.write(reinterpret_cast<const u8*>(prefix), sizeof(prefix) - 1);

    // Fill with padding headers up to ~3.5KB
    u32 target = 3500;
    while (conn->recv_buf.len() < target) {
        const char hdr[] = "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
        u32 avail = conn->recv_buf.write_avail();
        if (avail < sizeof(hdr)) break;
        conn->recv_buf.write(reinterpret_cast<const u8*>(hdr), sizeof(hdr) - 1);
    }
    conn->recv_buf.write(reinterpret_cast<const u8*>("\r\n"), 2);

    u32 total = conn->recv_buf.len();
    IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(total));
    loop.backend.inject(recv_ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    loop.inject_and_dispatch(
        make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));

    CHECK_EQ(ring->available(), 1u);
    CaptureEntry cap{};
    ring->pop(cap);
    CHECK_GT(cap.raw_header_len, 100);   // captured something substantial
    CHECK(cap.raw_header_len <= total);   // not more than sent
    CHECK_EQ(cap.flags & kCaptureFlagTruncated, 0);  // not truncated (under 8KB)

    munmap(ring, sizeof(CaptureRing));
}

int main(int argc, char** argv) { return rut::test::run_all(argc, argv); }
