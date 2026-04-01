// Tests for traffic replay: ReplayReader, replay_one, replay_file.
#include "rut/runtime/traffic_replay.h"
#include "test.h"
#include "test_helpers.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

using namespace rut;

// --- Helper: create a capture file with N entries ---

struct TempCapture {
    char path[64];
    i32 fd;

    bool create(const CaptureEntry* entries, u32 count) {
        __builtin_memcpy(path, "/tmp/rut_replay_XXXXXX", 23);
        fd = mkstemp(path);
        if (fd < 0) return false;

        CaptureFileHeader hdr;
        capture_file_header_init(&hdr);
        hdr.entry_count = count;
        if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return false;

        for (u32 i = 0; i < count; i++) {
            if (capture_write_entry(fd, entries[i]) != 0) return false;
        }

        // Seek back to start for reading
        lseek(fd, 0, SEEK_SET);
        close(fd);
        fd = -1;
        return true;
    }

    void cleanup() { unlink(path); }
};

static CaptureEntry make_captured_request(const char* req, u16 status) {
    CaptureEntry entry{};
    u32 len = 0;
    while (req[len]) len++;
    if (len > CaptureEntry::kMaxHeaderLen) len = CaptureEntry::kMaxHeaderLen;
    __builtin_memcpy(entry.raw_headers, req, len);
    entry.raw_header_len = static_cast<u16>(len);
    entry.resp_status = status;
    entry.method = static_cast<u8>(LogHttpMethod::Get);
    entry.timestamp_us = realtime_us();
    return entry;
}

// === ReplayReader ===

TEST(replay_reader, open_valid_file) {
    CaptureEntry entries[2];
    entries[0] = make_captured_request("GET / HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    entries[1] = make_captured_request("GET /b HTTP/1.1\r\nHost: x\r\n\r\n", 200);

    TempCapture tmp;
    REQUIRE(tmp.create(entries, 2));

    ReplayReader reader;
    CHECK_EQ(reader.open(tmp.path), 0);
    CHECK_EQ(reader.entry_count(), 2u);

    CaptureEntry out{};
    CHECK_EQ(reader.next(out), 0);
    CHECK_EQ(out.raw_header_len, entries[0].raw_header_len);
    CHECK_EQ(reader.next(out), 0);
    CHECK_EQ(out.raw_header_len, entries[1].raw_header_len);
    CHECK_EQ(reader.next(out), -1);  // EOF

    reader.close();
    tmp.cleanup();
}

TEST(replay_reader, open_nonexistent_fails) {
    ReplayReader reader;
    CHECK_EQ(reader.open("/tmp/rut_does_not_exist_12345"), -1);
}

TEST(replay_reader, open_invalid_magic_fails) {
    char path[] = "/tmp/rut_badmagic_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.magic[0] = 'X';  // corrupt
    write(fd, &hdr, sizeof(hdr));
    close(fd);

    ReplayReader reader;
    CHECK_EQ(reader.open(path), -1);

    unlink(path);
}

TEST(replay_reader, empty_file) {
    TempCapture tmp;
    REQUIRE(tmp.create(nullptr, 0));

    ReplayReader reader;
    CHECK_EQ(reader.open(tmp.path), 0);
    CHECK_EQ(reader.entry_count(), 0u);

    CaptureEntry out{};
    CHECK_EQ(reader.next(out), -1);

    reader.close();
    tmp.cleanup();
}

// === replay_one ===

TEST(replay_one, basic_200) {
    SmallLoop loop;
    loop.setup();

    CaptureEntry entry = make_captured_request(
        "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n", 200);

    ReplayResult result = replay_one(loop, entry, 42);
    CHECK(result.replayed);
    CHECK_EQ(result.expected_status, 200);
    CHECK_EQ(result.actual_status, 200);
    CHECK(result.status_match);
}

TEST(replay_one, status_mismatch) {
    SmallLoop loop;
    loop.setup();

    // Captured entry says 404, but current config returns 200
    CaptureEntry entry = make_captured_request(
        "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 404);

    ReplayResult result = replay_one(loop, entry, 42);
    CHECK(result.replayed);
    CHECK_EQ(result.expected_status, 404);
    CHECK_EQ(result.actual_status, 200);  // current config always returns 200
    CHECK(!result.status_match);
}

TEST(replay_one, multiple_sequential) {
    SmallLoop loop;
    loop.setup();

    const char* reqs[] = {
        "GET /a HTTP/1.1\r\nHost: x\r\n\r\n",
        "POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
        "DELETE /c HTTP/1.1\r\nHost: x\r\n\r\n",
    };

    for (i32 i = 0; i < 3; i++) {
        CaptureEntry entry = make_captured_request(reqs[i], 200);
        ReplayResult result = replay_one(loop, entry, 100 + i);
        CHECK(result.replayed);
        CHECK(result.status_match);
    }
}

// === replay_file ===

TEST(replay_file, full_roundtrip) {
    // Create capture file with 5 entries (all expect 200)
    CaptureEntry entries[5];
    const char* reqs[] = {
        "GET /1 HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /2 HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /3 HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /4 HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /5 HTTP/1.1\r\nHost: x\r\n\r\n",
    };
    for (u32 i = 0; i < 5; i++)
        entries[i] = make_captured_request(reqs[i], 200);

    TempCapture tmp;
    REQUIRE(tmp.create(entries, 5));

    ReplayReader reader;
    REQUIRE(reader.open(tmp.path) == 0);

    SmallLoop loop;
    loop.setup();

    ReplaySummary summary = replay_file(loop, reader);
    CHECK_EQ(summary.total, 5u);
    CHECK_EQ(summary.replayed, 5u);
    CHECK_EQ(summary.matched, 5u);
    CHECK_EQ(summary.mismatched, 0u);
    CHECK_EQ(summary.failed, 0u);

    reader.close();
    tmp.cleanup();
}

TEST(replay_file, with_mismatches) {
    // 3 entries: 2 expect 200 (match), 1 expects 404 (mismatch)
    CaptureEntry entries[3];
    entries[0] = make_captured_request("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    entries[1] = make_captured_request("GET /miss HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    entries[2] = make_captured_request("GET /ok2 HTTP/1.1\r\nHost: x\r\n\r\n", 200);

    TempCapture tmp;
    REQUIRE(tmp.create(entries, 3));

    ReplayReader reader;
    REQUIRE(reader.open(tmp.path) == 0);

    SmallLoop loop;
    loop.setup();

    ReplaySummary summary = replay_file(loop, reader);
    CHECK_EQ(summary.total, 3u);
    CHECK_EQ(summary.replayed, 3u);
    CHECK_EQ(summary.matched, 2u);
    CHECK_EQ(summary.mismatched, 1u);

    reader.close();
    tmp.cleanup();
}

TEST(replay_file, empty_capture) {
    TempCapture tmp;
    REQUIRE(tmp.create(nullptr, 0));

    ReplayReader reader;
    REQUIRE(reader.open(tmp.path) == 0);

    SmallLoop loop;
    loop.setup();

    ReplaySummary summary = replay_file(loop, reader);
    CHECK_EQ(summary.total, 0u);
    CHECK_EQ(summary.replayed, 0u);

    reader.close();
    tmp.cleanup();
}

// === End-to-end: capture → file → replay → verify ===

TEST(replay_e2e, capture_then_replay) {
    // Step 1: Capture traffic from a live loop
    auto* ring = static_cast<CaptureRing*>(
        mmap(nullptr, sizeof(CaptureRing), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(ring != MAP_FAILED);
    ring->init();

    SmallLoop capture_loop;
    capture_loop.setup();
    capture_loop.set_capture(ring);

    // Send 3 requests through the capture loop
    capture_loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* conn = capture_loop.find_fd(42);
    REQUIRE(conn != nullptr);

    const char* reqs[] = {
        "GET /users HTTP/1.1\r\nHost: api.test\r\n\r\n",
        "POST /login HTTP/1.1\r\nHost: api.test\r\nContent-Length: 0\r\n\r\n",
        "GET /health HTTP/1.1\r\nHost: api.test\r\n\r\n",
    };
    for (int i = 0; i < 3; i++) {
        conn->recv_buf.reset();
        u32 len = 0;
        while (reqs[i][len]) len++;
        conn->recv_buf.write(reinterpret_cast<const u8*>(reqs[i]), len);
        IoEvent recv_ev = make_ev(conn->id, IoEventType::Recv, static_cast<i32>(len));
        capture_loop.backend.inject(recv_ev);
        IoEvent events[8];
        u32 n = capture_loop.backend.wait(events, 8);
        for (u32 j = 0; j < n; j++) capture_loop.dispatch(events[j]);
        capture_loop.inject_and_dispatch(
            make_ev(conn->id, IoEventType::Send, static_cast<i32>(conn->send_buf.len())));
    }
    CHECK_EQ(ring->available(), 3u);

    // Step 2: Write captured entries to file
    char path[] = "/tmp/rut_e2e_replay_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 3;
    write(fd, &hdr, sizeof(hdr));

    CaptureEntry cap{};
    for (u32 i = 0; i < 3; i++) {
        ring->pop(cap);
        capture_write_entry(fd, cap);
    }
    close(fd);

    // Step 3: Replay against a fresh loop (simulating "new config")
    ReplayReader reader;
    REQUIRE(reader.open(path) == 0);

    SmallLoop replay_loop;
    replay_loop.setup();

    ReplaySummary summary = replay_file(replay_loop, reader);
    CHECK_EQ(summary.total, 3u);
    CHECK_EQ(summary.replayed, 3u);
    CHECK_EQ(summary.matched, 3u);  // same config → same results
    CHECK_EQ(summary.mismatched, 0u);

    reader.close();
    unlink(path);
    munmap(ring, sizeof(CaptureRing));
}

// === Route matching tests (static routes) ===

// Helper: set up SmallLoop with a RouteConfig wired into config_ptr.
struct RoutedLoop {
    SmallLoop loop;
    const RouteConfig* active_config;

    void setup(RouteConfig* cfg) {
        loop.setup();
        active_config = cfg;
        loop.config_ptr = &active_config;
    }
};

TEST(route, static_200) {
    RouteConfig cfg;
    cfg.add_static("/health", 'G', 200);

    RoutedLoop rl;
    rl.setup(&cfg);

    CaptureEntry entry = make_captured_request(
        "GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    ReplayResult result = replay_one(rl.loop, entry, 42);
    CHECK(result.replayed);
    CHECK_EQ(result.actual_status, 200);
    CHECK(result.status_match);
}

TEST(route, static_404) {
    RouteConfig cfg;
    cfg.add_static("/", 0, 404);  // catch-all 404

    RoutedLoop rl;
    rl.setup(&cfg);

    CaptureEntry entry = make_captured_request(
        "GET /anything HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    ReplayResult result = replay_one(rl.loop, entry, 42);
    CHECK(result.replayed);
    CHECK_EQ(result.actual_status, 404);
    CHECK(result.status_match);
}

TEST(route, no_match_default_200) {
    RouteConfig cfg;
    cfg.add_static("/api", 'G', 200);
    // /other doesn't match /api

    RoutedLoop rl;
    rl.setup(&cfg);

    CaptureEntry entry = make_captured_request(
        "GET /other HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    ReplayResult result = replay_one(rl.loop, entry, 42);
    CHECK(result.replayed);
    CHECK_EQ(result.actual_status, 200);  // default
}

TEST(route, method_filtering) {
    RouteConfig cfg;
    cfg.add_static("/admin", 'P', 403);  // POST /admin → 403
    cfg.add_static("/admin", 'G', 200);  // GET /admin → 200

    RoutedLoop rl;
    rl.setup(&cfg);

    // GET /admin → matches second rule → 200
    CaptureEntry get_entry = make_captured_request(
        "GET /admin HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    ReplayResult get_result = replay_one(rl.loop, get_entry, 42);
    CHECK(get_result.replayed);
    CHECK_EQ(get_result.actual_status, 200);

    // POST /admin → matches first rule → 403
    CaptureEntry post_entry = make_captured_request(
        "POST /admin HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 403);
    post_entry.method = static_cast<u8>(LogHttpMethod::Post);
    ReplayResult post_result = replay_one(rl.loop, post_entry, 43);
    CHECK(post_result.replayed);
    CHECK_EQ(post_result.actual_status, 403);
}

TEST(route, multiple_routes_first_match_wins) {
    RouteConfig cfg;
    cfg.add_static("/api/v1", 0, 200);  // specific prefix
    cfg.add_static("/api", 0, 301);     // broader prefix
    cfg.add_static("/", 0, 404);        // catch-all

    RoutedLoop rl;
    rl.setup(&cfg);

    // /api/v1/users → matches first rule
    CaptureEntry e1 = make_captured_request(
        "GET /api/v1/users HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    CHECK_EQ(replay_one(rl.loop, e1, 42).actual_status, 200);

    // /api/v2 → matches second rule (prefix /api)
    CaptureEntry e2 = make_captured_request(
        "GET /api/v2 HTTP/1.1\r\nHost: x\r\n\r\n", 301);
    CHECK_EQ(replay_one(rl.loop, e2, 43).actual_status, 301);

    // /other → matches third rule (prefix /)
    CaptureEntry e3 = make_captured_request(
        "GET /other HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    CHECK_EQ(replay_one(rl.loop, e3, 44).actual_status, 404);
}

TEST(route, replay_file_with_routing) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);
    cfg.add_static("/api", 0, 200);
    cfg.add_static("/", 0, 404);

    // Capture entries with expected statuses
    CaptureEntry entries[4];
    entries[0] = make_captured_request("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    entries[1] = make_captured_request("GET /api/data HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    entries[2] = make_captured_request("GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 404);
    entries[3] = make_captured_request("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 200);

    TempCapture tmp;
    REQUIRE(tmp.create(entries, 4));

    ReplayReader reader;
    REQUIRE(reader.open(tmp.path) == 0);

    RoutedLoop rl;
    rl.setup(&cfg);

    ReplaySummary summary = replay_file(rl.loop, reader);
    CHECK_EQ(summary.total, 4u);
    CHECK_EQ(summary.replayed, 4u);
    CHECK_EQ(summary.matched, 4u);
    CHECK_EQ(summary.mismatched, 0u);

    reader.close();
    tmp.cleanup();
}

// Detect config change: replay captured traffic against a DIFFERENT config
TEST(route, detect_config_regression) {
    // Old config: /api → 200, /admin → 403, / → 404
    // Captured traffic expects these statuses.
    CaptureEntry entries[3];
    entries[0] = make_captured_request("GET /api/users HTTP/1.1\r\nHost: x\r\n\r\n", 200);
    entries[1] = make_captured_request("GET /admin HTTP/1.1\r\nHost: x\r\n\r\n", 403);
    entries[2] = make_captured_request("GET /other HTTP/1.1\r\nHost: x\r\n\r\n", 404);

    TempCapture tmp;
    REQUIRE(tmp.create(entries, 3));

    // New config: /admin is now 200 (permission change), /api removed
    RouteConfig new_cfg;
    new_cfg.add_static("/admin", 0, 200);  // changed from 403 to 200
    new_cfg.add_static("/", 0, 404);       // catch-all

    ReplayReader reader;
    REQUIRE(reader.open(tmp.path) == 0);

    RoutedLoop rl;
    rl.setup(&new_cfg);

    ReplaySummary summary = replay_file(rl.loop, reader);
    CHECK_EQ(summary.total, 3u);
    CHECK_EQ(summary.replayed, 3u);

    // /api/users: was 200, new config / → 404 → mismatch
    // /admin: was 403, now 200 → mismatch
    // /other: was 404, / → 404 → match
    CHECK_EQ(summary.matched, 1u);    // /other (404 → 404)
    CHECK_EQ(summary.mismatched, 2u); // /api/users + /admin changed

    reader.close();
    tmp.cleanup();
}

int main(int argc, char** argv) { return rut::test::run_all(argc, argv); }
