// Access log tests: SPSC ring buffer, text output, zstd compression, flusher.
#include "rut/runtime/access_log.h"
#include "test.h"
#include "test_helpers.h"

#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

using namespace rut;

// --- Helper: create a sample entry ---

static AccessLogEntry make_entry(u16 status, u32 duration_us, u8 shard, const char* path) {
    AccessLogEntry e{};
    e.timestamp_us = 1711123456789000ULL;  // fixed for deterministic tests
    e.status = status;
    e.duration_us = duration_us;
    e.shard_id = shard;
    e.method = static_cast<u8>(LogHttpMethod::Get);
    e.req_size = 256;
    e.resp_size = 1024;
    e.addr = 0x0100007F;  // 127.0.0.1 in network byte order
    u32 i = 0;
    while (path[i] && i < sizeof(e.path) - 1) {
        e.path[i] = path[i];
        i++;
    }
    e.path[i] = '\0';
    e.upstream[0] = '\0';
    e.upstream_us = 0;
    return e;
}

// Helper: count newlines.
static u32 count_lines(const char* buf, u32 len) {
    u32 n = 0;
    for (u32 i = 0; i < len; i++)
        if (buf[i] == '\n') n++;
    return n;
}

// Helper: read all from fd.
static u32 read_all(i32 fd, char* buf, u32 buf_size) {
    u32 total = 0;
    while (total < buf_size) {
        ssize_t n = read(fd, buf + total, buf_size - total);
        if (n <= 0) break;
        total += static_cast<u32>(n);
    }
    return total;
}

static void write_request(Connection& c, const char* raw) {
    c.recv_buf.reset();
    u32 len = 0;
    while (raw[len]) len++;
    u8* dst = c.recv_buf.write_ptr();
    for (u32 i = 0; i < len; i++) dst[i] = static_cast<u8>(raw[i]);
    c.recv_buf.commit(len);
}

// === AccessLogEntry size ===

TEST(access_log_entry, size_is_128) {
    CHECK_EQ(sizeof(AccessLogEntry), 128u);
}

// === SPSC Ring: basic operations ===

TEST(ring, init_empty) {
    AccessLogRing ring;
    ring.init();
    CHECK_EQ(ring.available(), 0u);
    AccessLogEntry out{};
    CHECK(!ring.pop(out));
}

TEST(ring, push_pop_one) {
    AccessLogRing ring;
    ring.init();
    auto entry = make_entry(200, 1234, 0, "/users");
    ring.push(entry);
    CHECK_EQ(ring.available(), 1u);
    AccessLogEntry out{};
    CHECK(ring.pop(out));
    CHECK_EQ(out.status, 200u);
    CHECK_EQ(out.duration_us, 1234u);
    CHECK_EQ(ring.available(), 0u);
}

TEST(ring, push_pop_multiple) {
    AccessLogRing ring;
    ring.init();
    for (u32 i = 0; i < 10; i++) ring.push(make_entry(static_cast<u16>(200 + i), i * 100, 0, "/"));
    CHECK_EQ(ring.available(), 10u);
    for (u32 i = 0; i < 10; i++) {
        AccessLogEntry out{};
        CHECK(ring.pop(out));
        CHECK_EQ(out.status, static_cast<u16>(200 + i));
    }
    CHECK_EQ(ring.available(), 0u);
}

TEST(ring, fifo_order) {
    AccessLogRing ring;
    ring.init();
    ring.push(make_entry(200, 0, 0, "/first"));
    ring.push(make_entry(404, 0, 0, "/second"));
    ring.push(make_entry(500, 0, 0, "/third"));
    AccessLogEntry out{};
    ring.pop(out);
    CHECK_EQ(out.status, 200u);
    ring.pop(out);
    CHECK_EQ(out.status, 404u);
    ring.pop(out);
    CHECK_EQ(out.status, 500u);
}

TEST(ring, full_drops_newest) {
    AccessLogRing ring;
    ring.init();
    for (u32 i = 0; i < AccessLogRing::kCapacity; i++)
        CHECK(ring.push(make_entry(static_cast<u16>(i), 0, 0, "/")));
    CHECK(!ring.push(make_entry(999, 0, 0, "/dropped")));
    AccessLogEntry out{};
    ring.pop(out);
    CHECK_EQ(out.status, 0u);
}

TEST(ring, push_succeeds_after_pop) {
    AccessLogRing ring;
    ring.init();
    for (u32 i = 0; i < AccessLogRing::kCapacity; i++)
        ring.push(make_entry(static_cast<u16>(i), 0, 0, "/"));
    AccessLogEntry out{};
    ring.pop(out);
    CHECK(ring.push(make_entry(999, 0, 0, "/new")));
    while (ring.available() > 1) ring.pop(out);
    ring.pop(out);
    CHECK_EQ(out.status, 999u);
}

TEST(ring, push_returns_true_when_not_full) {
    AccessLogRing ring;
    ring.init();
    CHECK(ring.push(make_entry(200, 0, 0, "/")));
    CHECK(ring.push(make_entry(200, 0, 0, "/")));
    CHECK_EQ(ring.available(), 2u);
}

// === Clocks ===

TEST(realtime, returns_nonzero) {
    CHECK(realtime_us() > 0);
}
TEST(realtime, non_decreasing) {
    CHECK(realtime_us() <= realtime_us());
}
TEST(monotonic, returns_nonzero) {
    CHECK(monotonic_us() > 0);
}
TEST(monotonic, non_decreasing) {
    CHECK(monotonic_us() <= monotonic_us());
}

// === Text formatting ===

TEST(format, basic_text) {
    auto entry = make_entry(200, 1234, 3, "/api/users");
    char buf[512];
    u32 n = format_access_log_text(entry, buf, sizeof(buf));
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strstr(buf, "GET") != nullptr);
    CHECK(strstr(buf, "/api/users") != nullptr);
    CHECK(strstr(buf, "200") != nullptr);
    CHECK(strstr(buf, "1234us") != nullptr);
    CHECK(strstr(buf, "127.0.0.1") != nullptr);
    CHECK(strstr(buf, "s=3") != nullptr);
    CHECK_EQ(buf[n - 1], '\n');
}

TEST(format, all_methods) {
    const char* expected[] = {
        "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "CONNECT", "TRACE", "OTHER"};
    for (u8 m = 0; m < 10; m++) {
        AccessLogEntry entry{};
        entry.timestamp_us = 1711123456789000ULL;
        entry.status = 200;
        entry.method = m;
        entry.path[0] = '/';
        entry.path[1] = '\0';
        char buf[512];
        u32 n = format_access_log_text(entry, buf, sizeof(buf));
        buf[n] = '\0';
        CHECK(strstr(buf, expected[m]) != nullptr);
    }
}

TEST(format, buf_too_small) {
    auto entry = make_entry(200, 0, 0, "/");
    char buf[10];
    u32 n = format_access_log_text(entry, buf, sizeof(buf));
    CHECK_EQ(n, 0u);
}

// === Flusher: plain text ===

TEST(flusher, flush_empty_rings) {
    AccessLogRing ring;
    ring.init();
    i32 fd = open("/dev/null", 1);
    REQUIRE(fd >= 0);
    AccessLogFlusher flusher;
    flusher.init(fd);
    flusher.add_ring(&ring);
    CHECK_EQ(flusher.flush_once(), 0u);
    close(fd);
}

TEST(flusher, flush_text_to_fd) {
    AccessLogRing ring;
    ring.init();
    ring.push(make_entry(200, 100, 0, "/a"));
    ring.push(make_entry(404, 200, 0, "/b"));

    i32 fds[2];
    REQUIRE(pipe(fds) == 0);
    AccessLogFlusher flusher;
    flusher.init(fds[1]);  // no compression
    flusher.add_ring(&ring);

    CHECK_EQ(flusher.flush_once(), 2u);
    close(fds[1]);

    char buf[4096];
    u32 n = read_all(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    buf[n] = '\0';

    CHECK_EQ(count_lines(buf, n), 2u);
    CHECK(strstr(buf, "200") != nullptr);
    CHECK(strstr(buf, "404") != nullptr);
    CHECK(strstr(buf, "/a") != nullptr);
    CHECK(strstr(buf, "/b") != nullptr);
}

TEST(flusher, flush_multiple_rings) {
    AccessLogRing ring1, ring2;
    ring1.init();
    ring2.init();
    ring1.push(make_entry(200, 0, 0, "/r1"));
    ring2.push(make_entry(500, 0, 1, "/r2"));

    i32 fd = open("/dev/null", 1);
    REQUIRE(fd >= 0);
    AccessLogFlusher flusher;
    flusher.init(fd);
    flusher.add_ring(&ring1);
    flusher.add_ring(&ring2);
    CHECK_EQ(flusher.flush_once(), 2u);
    close(fd);
}

// === Batch ===

TEST(batch, many_entries_text) {
    AccessLogRing ring;
    ring.init();
    u32 count = 200;
    for (u32 i = 0; i < count; i++)
        ring.push(make_entry(static_cast<u16>(200 + (i % 5)), i * 10, 0, "/batch"));

    i32 fds[2];
    REQUIRE(pipe(fds) == 0);
    (void)fcntl(fds[0], 1031 /*F_SETPIPE_SZ*/, 1048576);

    AccessLogFlusher flusher;
    flusher.init(fds[1]);
    flusher.add_ring(&ring);
    CHECK_EQ(flusher.flush_once(), count);
    close(fds[1]);

    char buf[131072];
    u32 n = read_all(fds[0], buf, sizeof(buf));
    close(fds[0]);
    CHECK_EQ(count_lines(buf, n), count);
}

TEST(batch, single_entry_flushes) {
    AccessLogRing ring;
    ring.init();
    ring.push(make_entry(503, 999, 2, "/single"));

    i32 fds[2];
    REQUIRE(pipe(fds) == 0);
    AccessLogFlusher flusher;
    flusher.init(fds[1]);
    flusher.add_ring(&ring);
    CHECK_EQ(flusher.flush_once(), 1u);
    close(fds[1]);

    char buf[2048];
    u32 n = read_all(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    buf[n] = '\0';
    CHECK(strstr(buf, "503") != nullptr);
    CHECK(strstr(buf, "999us") != nullptr);
    CHECK(strstr(buf, "s=2") != nullptr);
}

// === Zstd compression ===

TEST(zstd, compressed_output_is_smaller) {
    AccessLogRing ring;
    ring.init();
    for (u32 i = 0; i < 100; i++) ring.push(make_entry(200, i * 10, 0, "/api/users"));

    // Plain text size.
    i32 fds_plain[2];
    REQUIRE(pipe(fds_plain) == 0);
    AccessLogFlusher plain;
    plain.init(fds_plain[1], false);
    plain.add_ring(&ring);
    plain.flush_once();
    close(fds_plain[1]);
    char pbuf[65536];
    u32 plain_size = read_all(fds_plain[0], pbuf, sizeof(pbuf));
    close(fds_plain[0]);

    // Refill ring for compressed test.
    ring.init();
    for (u32 i = 0; i < 100; i++) ring.push(make_entry(200, i * 10, 0, "/api/users"));

    // Compressed size.
    i32 fds_zstd[2];
    REQUIRE(pipe(fds_zstd) == 0);
    AccessLogFlusher compressed;
    compressed.init(fds_zstd[1], true);
    compressed.add_ring(&ring);
    compressed.start();
    // Give flusher time to run.
    struct timespec ts = {0, 200000000L};  // 200ms
    nanosleep(&ts, nullptr);
    compressed.stop();  // stop calls endStream + final flush
    close(fds_zstd[1]);
    char cbuf[65536];
    u32 zstd_size = read_all(fds_zstd[0], cbuf, sizeof(cbuf));
    close(fds_zstd[0]);

    // Compressed should be significantly smaller.
    CHECK(zstd_size > 0);
    CHECK(zstd_size < plain_size / 2);
}

// === Callback integration ===

TEST(callback_log, emits_entry_on_response) {
    SmallLoop loop;
    loop.setup();
    AccessLogRing ring;
    ring.init();
    loop.access_log = &ring;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));

    CHECK_EQ(ring.available(), 1u);
    AccessLogEntry out{};
    ring.pop(out);
    CHECK_EQ(out.status, 200u);
    CHECK(out.duration_us < 1000000u);
    CHECK_EQ(out.resp_size, send_len);
}

TEST(callback_log, captures_request_metadata) {
    SmallLoop loop;
    loop.setup();
    AccessLogRing ring;
    ring.init();
    loop.access_log = &ring;

    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    c->peer_addr = 0x0100007F;

    const char* req = "POST /api/users?id=1 HTTP/1.1\r\nHost: example\r\n\r\n";
    write_request(*c, req);
    u32 req_len = c->recv_buf.len();
    IoEvent ev = make_ev(c->id, IoEventType::Recv, static_cast<i32>(req_len));
    loop.backend.inject(ev);
    IoEvent events[8];
    u32 n = loop.backend.wait(events, 8);
    for (u32 i = 0; i < n; i++) loop.dispatch(events[i]);

    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));

    AccessLogEntry out{};
    REQUIRE(ring.pop(out));
    CHECK_EQ(out.method, static_cast<u8>(LogHttpMethod::Post));
    CHECK_EQ(out.req_size, req_len);
    CHECK_EQ(out.addr, 0x0100007F);
    CHECK_EQ(out.path[0], '/');
    CHECK_EQ(out.path[1], 'a');
}

TEST(callback_log, no_log_when_ring_null) {
    SmallLoop loop;
    loop.setup();
    loop.inject_and_dispatch(make_ev(0, IoEventType::Accept, 42));
    auto* c = loop.find_fd(42);
    REQUIRE(c != nullptr);
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Recv, 100));
    u32 send_len = c->send_buf.len();
    loop.inject_and_dispatch(make_ev(c->id, IoEventType::Send, static_cast<i32>(send_len)));
    CHECK(true);
}

// === Flusher: start() failure paths ===

TEST(flusher, start_returns_ok_on_success) {
    AccessLogRing ring;
    ring.init();
    i32 fd = open("/dev/null", 1);
    REQUIRE(fd >= 0);
    AccessLogFlusher flusher;
    flusher.init(fd);
    flusher.add_ring(&ring);
    CHECK(flusher.start().has_value());
    flusher.stop();
    close(fd);
}

TEST(flusher, start_on_bad_fd_still_starts) {
    // start() should succeed even if fd is invalid — the thread starts,
    // writes fail, and stop() joins cleanly.
    AccessLogFlusher flusher;
    flusher.init(-1);
    CHECK(flusher.start().has_value());
    flusher.stop();
}

TEST(flusher, start_idempotent) {
    i32 fd = open("/dev/null", 1);
    REQUIRE(fd >= 0);
    AccessLogFlusher flusher;
    flusher.init(fd);
    CHECK(flusher.start().has_value());
    CHECK(flusher.start().has_value());  // second call is a no-op
    flusher.stop();
    close(fd);
}

// === Flusher: write_with_poll via closed pipe ===

TEST(flusher, flush_to_closed_pipe) {
    AccessLogRing ring;
    ring.init();
    ring.push(make_entry(200, 100, 0, "/test"));

    i32 fds[2];
    REQUIRE(pipe(fds) == 0);
    // Close read end immediately — writes will fail with EPIPE/POLLHUP.
    close(fds[0]);

    AccessLogFlusher flusher;
    flusher.init(fds[1]);
    flusher.add_ring(&ring);
    // flush_once should handle the write failure gracefully.
    flusher.flush_once();
    close(fds[1]);
    CHECK(true);  // no crash
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
