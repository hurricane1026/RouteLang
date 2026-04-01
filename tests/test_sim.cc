// Simulation tests — uses real Shard + EpollBackend + loopback TCP.
// Verifies that captured traffic replayed through production code path
// produces identical routing results, and collects latency/resource metrics.
#include "rut/runtime/sim_engine.h"
#include "rut/runtime/shard.h"
#include "rut/runtime/traffic_capture.h"
#include "rut/runtime/traffic_replay.h"
#include "test.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

using namespace rut;

using RealShard = Shard<EpollBackend>;

// Helper: set up a real Shard with a RouteConfig, return its listen port.
struct SimServer {
    RealShard shard;
    i32 listen_fd = -1;
    u16 port = 0;
    RouteConfig config;

    bool setup(RouteConfig* cfg) {
        auto lfd = create_listen_socket(0);
        if (!lfd) return false;
        listen_fd = lfd.value();
        port = get_port(listen_fd);

        if (!shard.init(0, listen_fd)) {
            close(listen_fd);
            return false;
        }
        shard.owns_listen_fd = true;

        // Wire route config
        shard.route_config = cfg;
        shard.active_config = cfg;

        // Enable metrics
        shard.loop->metrics = &shard.shard_metrics;

        if (!shard.spawn()) {
            shard.shutdown();
            return false;
        }
        // Small delay for thread startup
        usleep(1000);
        return true;
    }

    void teardown() {
        shard.shutdown();
    }
};

// === Basic simulation ===

TEST(sim, single_request_200) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);

    SimServer srv;
    REQUIRE(srv.setup(&cfg));

    CaptureEntry entry{};
    const char req[] = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
    __builtin_memcpy(entry.raw_headers, req, sizeof(req) - 1);
    entry.raw_header_len = sizeof(req) - 1;
    entry.resp_status = 200;
    entry.method = static_cast<u8>(LogHttpMethod::Get);

    SimResult result = sim_one(srv.port, entry);
    CHECK(result.success);
    CHECK_EQ(result.actual_status, 200);
    CHECK(result.status_match);
    CHECK_GT(result.latency_us, 0u);

    srv.teardown();
}

TEST(sim, static_404) {
    RouteConfig cfg;
    cfg.add_static("/", 0, 404);

    SimServer srv;
    REQUIRE(srv.setup(&cfg));

    CaptureEntry entry{};
    const char req[] = "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n";
    __builtin_memcpy(entry.raw_headers, req, sizeof(req) - 1);
    entry.raw_header_len = sizeof(req) - 1;
    entry.resp_status = 404;
    entry.method = static_cast<u8>(LogHttpMethod::Get);

    SimResult result = sim_one(srv.port, entry);
    CHECK(result.success);
    CHECK_EQ(result.actual_status, 404);
    CHECK(result.status_match);

    srv.teardown();
}

TEST(sim, mismatch_detected) {
    RouteConfig cfg;
    cfg.add_static("/", 0, 200);  // returns 200

    SimServer srv;
    REQUIRE(srv.setup(&cfg));

    // Capture says expected 404, but server returns 200
    CaptureEntry entry{};
    const char req[] = "GET /test HTTP/1.1\r\nHost: x\r\n\r\n";
    __builtin_memcpy(entry.raw_headers, req, sizeof(req) - 1);
    entry.raw_header_len = sizeof(req) - 1;
    entry.resp_status = 404;  // expected
    entry.method = static_cast<u8>(LogHttpMethod::Get);

    SimResult result = sim_one(srv.port, entry);
    CHECK(result.success);
    CHECK_EQ(result.actual_status, 200);
    CHECK(!result.status_match);

    srv.teardown();
}

// === Multiple requests ===

TEST(sim, multiple_routes) {
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);
    cfg.add_static("/api", 0, 200);
    cfg.add_static("/", 0, 404);

    SimServer srv;
    REQUIRE(srv.setup(&cfg));

    struct TestCase {
        const char* req;
        u16 expected;
    };
    TestCase cases[] = {
        {"GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 200},
        {"GET /api/users HTTP/1.1\r\nHost: x\r\n\r\n", 200},
        {"GET /missing HTTP/1.1\r\nHost: x\r\n\r\n", 404},
        {"POST /api/data HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", 200},
        {"GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 200},
    };

    SimSummary summary{};
    summary.latency_min = 0xFFFFFFFF;

    for (auto& tc : cases) {
        CaptureEntry entry{};
        u32 len = 0;
        while (tc.req[len]) len++;
        __builtin_memcpy(entry.raw_headers, tc.req, len);
        entry.raw_header_len = static_cast<u16>(len);
        entry.resp_status = tc.expected;
        entry.method = static_cast<u8>(LogHttpMethod::Get);

        SimResult result = sim_one(srv.port, entry);
        summary.total++;
        if (result.success) {
            summary.succeeded++;
            if (result.status_match) summary.matched++;
            else summary.mismatched++;
            summary.latency_sum += result.latency_us;
            if (result.latency_us < summary.latency_min) summary.latency_min = result.latency_us;
            if (result.latency_us > summary.latency_max) summary.latency_max = result.latency_us;
        } else {
            summary.failed++;
        }
    }

    // Metrics from shard
    summary.connections_total = srv.shard.shard_metrics.connections_total;
    summary.requests_total = srv.shard.shard_metrics.requests_total;

    CHECK_EQ(summary.total, 5u);
    CHECK_EQ(summary.succeeded, 5u);
    CHECK_EQ(summary.matched, 5u);
    CHECK_EQ(summary.mismatched, 0u);
    CHECK_GT(summary.latency_avg(), 0u);

    // Print report for LLM inspection
    char report[4096];
    u32 rlen = sim_format_summary(summary, report, sizeof(report));
    write(1, report, rlen);

    srv.teardown();
}

// === Output format ===

TEST(sim, format_result_output) {
    SimResult r{};
    r.method = static_cast<u8>(LogHttpMethod::Get);
    r.path[0] = '/'; r.path[1] = 'a'; r.path[2] = 'p'; r.path[3] = 'i'; r.path[4] = '\0';
    r.expected_status = 200;
    r.actual_status = 200;
    r.status_match = true;
    r.success = true;
    r.latency_us = 150;

    char buf[512];
    u32 len = sim_format_result(r, buf, sizeof(buf));
    CHECK_GT(len, 0u);

    // Should contain "MATCH"
    bool found_match = false;
    for (u32 i = 0; i + 4 < len; i++) {
        if (buf[i] == 'M' && buf[i + 1] == 'A' && buf[i + 2] == 'T') {
            found_match = true;
            break;
        }
    }
    CHECK(found_match);

    // MISS case
    SimResult r2{};
    r2.method = static_cast<u8>(LogHttpMethod::Post);
    r2.path[0] = '/'; r2.path[1] = 'x'; r2.path[2] = '\0';
    r2.expected_status = 200;
    r2.actual_status = 404;
    r2.status_match = false;
    r2.success = true;
    r2.latency_us = 300;

    len = sim_format_result(r2, buf, sizeof(buf));
    bool found_miss = false;
    for (u32 i = 0; i + 3 < len; i++) {
        if (buf[i] == 'M' && buf[i + 1] == 'I' && buf[i + 2] == 'S') {
            found_miss = true;
            break;
        }
    }
    CHECK(found_miss);
}

// === End-to-end: capture → file → simulate ===

TEST(sim, capture_file_simulate) {
    // Step 1: Create a capture file with known entries
    CaptureEntry entries[3];
    const char* reqs[] = {
        "GET /health HTTP/1.1\r\nHost: test\r\n\r\n",
        "GET /api/data HTTP/1.1\r\nHost: test\r\n\r\n",
        "GET /missing HTTP/1.1\r\nHost: test\r\n\r\n",
    };
    u16 expected[] = {200, 200, 404};

    for (u32 i = 0; i < 3; i++) {
        __builtin_memset(&entries[i], 0, sizeof(CaptureEntry));
        u32 len = 0;
        while (reqs[i][len]) len++;
        __builtin_memcpy(entries[i].raw_headers, reqs[i], len);
        entries[i].raw_header_len = static_cast<u16>(len);
        entries[i].resp_status = expected[i];
        entries[i].method = static_cast<u8>(LogHttpMethod::Get);
    }

    char path[] = "/tmp/rut_sim_e2e_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 3;
    write(fd, &hdr, sizeof(hdr));
    for (u32 i = 0; i < 3; i++) capture_write_entry(fd, entries[i]);
    close(fd);

    // Step 2: Set up server with matching config
    RouteConfig cfg;
    cfg.add_static("/health", 0, 200);
    cfg.add_static("/api", 0, 200);
    cfg.add_static("/", 0, 404);

    SimServer srv;
    REQUIRE(srv.setup(&cfg));

    // Step 3: Read capture file and simulate
    ReplayReader reader;
    REQUIRE(reader.open(path) == 0);

    SimSummary summary{};
    summary.latency_min = 0xFFFFFFFF;
    CaptureEntry entry{};

    char result_buf[512];
    while (reader.next(entry) == 0) {
        SimResult result = sim_one(srv.port, entry);
        summary.total++;
        if (result.success) {
            summary.succeeded++;
            if (result.status_match) summary.matched++;
            else summary.mismatched++;
            summary.latency_sum += result.latency_us;
            if (result.latency_us < summary.latency_min) summary.latency_min = result.latency_us;
            if (result.latency_us > summary.latency_max) summary.latency_max = result.latency_us;
        } else {
            summary.failed++;
        }
        // Print each result
        u32 rlen = sim_format_result(result, result_buf, sizeof(result_buf));
        write(1, result_buf, rlen);
    }

    summary.connections_total = srv.shard.shard_metrics.connections_total;
    summary.requests_total = srv.shard.shard_metrics.requests_total;

    // Print summary
    char sum_buf[2048];
    u32 slen = sim_format_summary(summary, sum_buf, sizeof(sum_buf));
    write(1, sum_buf, slen);

    CHECK_EQ(summary.total, 3u);
    CHECK_EQ(summary.succeeded, 3u);
    CHECK_EQ(summary.matched, 3u);

    reader.close();
    unlink(path);
    srv.teardown();
}

int main(int argc, char** argv) { return rut::test::run_all(argc, argv); }
