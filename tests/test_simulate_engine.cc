#include "rut/sim/simulate_engine.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace rut;
using namespace rut::sim;

namespace {

static CaptureEntry make_entry(const char* req, u16 status, const char* upstream = nullptr) {
    CaptureEntry entry{};
    u32 len = 0;
    while (req[len]) len++;
    const u32 bounded_len =
        len > CaptureEntry::kMaxHeaderLen ? static_cast<u32>(CaptureEntry::kMaxHeaderLen) : len;
    __builtin_memcpy(entry.raw_headers, req, bounded_len);
    entry.raw_header_len = static_cast<u16>(bounded_len);
    entry.resp_status = status;
    if (upstream) {
        u32 i = 0;
        while (upstream[i] && i + 1 < sizeof(entry.upstream_name)) {
            entry.upstream_name[i] = upstream[i];
            i++;
        }
        entry.upstream_name[i] = '\0';
    }
    return entry;
}

static bool init_engine(const Manifest& manifest, ModuleContext& ctx, Engine& engine) {
    if (!build_module_from_manifest(manifest, ctx)) return false;
    return engine.init(ctx.module, manifest.upstreams, manifest.upstream_count);
}

static bool load_manifest_text(const char* text, Manifest* manifest) {
    char path[] = "/tmp/rut_sim_manifest_XXXXXX";
    const i32 fd = mkstemp(path);
    if (fd < 0) return false;
    const size_t len = strlen(text);
    const bool ok =
        write(fd, text, len) == static_cast<ssize_t>(len) && load_manifest(path, *manifest);
    close(fd);
    unlink(path);
    return ok;
}

}  // namespace

TEST(simulate_engine, load_manifest_file) {
    char path[] = "/tmp/rut_sim_manifest_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    static const char kManifest[] =
        "# comment\n"
        "upstream 7 api-v1\n"
        "route GET /health status 204\n"
        "route ANY /api proxy 7\n";
    REQUIRE(write(fd, kManifest, sizeof(kManifest) - 1) ==
            static_cast<ssize_t>(sizeof(kManifest) - 1));
    close(fd);

    Manifest manifest;
    REQUIRE(load_manifest(path, manifest));
    CHECK_EQ(manifest.upstream_count, 1u);
    CHECK_EQ(manifest.route_count, 2u);
    CHECK_EQ(manifest.upstreams[0].id, 7u);
    CHECK_EQ(manifest.routes[0].status_code, 204u);
    CHECK_EQ(manifest.routes[1].upstream_id, 7u);

    unlink(path);
}

TEST(simulate_engine, static_status_match) {
    Manifest manifest{};
    manifest.route_count = 1;
    manifest.routes[0].method = 'G';
    strcpy(manifest.routes[0].pattern, "/health");
    manifest.routes[0].action = ManifestAction::ReturnStatus;
    manifest.routes[0].status_code = 204;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    const auto result =
        simulate_one(engine, make_entry("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", 204));
    CHECK_EQ(result.verdict, Verdict::Match);
    CHECK_EQ(result.actual_status, 204u);

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, proxy_upstream_match) {
    Manifest manifest{};
    manifest.upstream_count = 1;
    manifest.upstreams[0].id = 7;
    strcpy(manifest.upstreams[0].name, "api-v1");
    manifest.route_count = 1;
    manifest.routes[0].method = 0;
    strcpy(manifest.routes[0].pattern, "/api");
    manifest.routes[0].action = ManifestAction::Proxy;
    manifest.routes[0].upstream_id = 7;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    const auto result = simulate_one(
        engine, make_entry("GET /api/users HTTP/1.1\r\nHost: x\r\n\r\n", 502, "api-v1"));
    CHECK_EQ(result.verdict, Verdict::Match);
    CHECK_EQ(result.action, jit::HandlerAction::Proxy);

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, default_200_when_no_route_matches) {
    Manifest manifest{};

    ModuleContext ctx;
    REQUIRE(ctx.init(1));
    Engine engine;
    REQUIRE(engine.init(ctx.module, manifest.upstreams, manifest.upstream_count));

    const auto result =
        simulate_one(engine, make_entry("GET /miss HTTP/1.1\r\nHost: x\r\n\r\n", 200));
    CHECK_EQ(result.verdict, Verdict::Match);
    CHECK_EQ(result.actual_status, 200u);

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, param_prefix_route_matches) {
    Manifest manifest{};
    manifest.route_count = 1;
    manifest.routes[0].method = 'G';
    strcpy(manifest.routes[0].pattern, "/users/:id");
    manifest.routes[0].action = ManifestAction::ReturnStatus;
    manifest.routes[0].status_code = 201;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    const auto result =
        simulate_one(engine, make_entry("GET /users/123/profile HTTP/1.1\r\nHost: x\r\n\r\n", 201));
    CHECK_EQ(result.verdict, Verdict::Match);

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, param_route_rejects_empty_segment) {
    Manifest manifest{};
    manifest.route_count = 1;
    manifest.routes[0].method = 'G';
    strcpy(manifest.routes[0].pattern, "/users/:id");
    manifest.routes[0].action = ManifestAction::ReturnStatus;
    manifest.routes[0].status_code = 201;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    const auto result =
        simulate_one(engine, make_entry("GET /users/ HTTP/1.1\r\nHost: x\r\n\r\n", 200));
    CHECK_EQ(result.verdict, Verdict::Match);
    CHECK_EQ(result.actual_status, 200u);

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, make_entry_clamps_long_headers) {
    char req[CaptureEntry::kMaxHeaderLen + 32];
    for (u32 i = 0; i + 1 < sizeof(req); i++) req[i] = 'a';
    req[sizeof(req) - 1] = '\0';

    const auto entry = make_entry(req, 204);
    CHECK_EQ(entry.raw_header_len, static_cast<u16>(CaptureEntry::kMaxHeaderLen));
}

TEST(simulate_engine, load_manifest_rejects_overflowing_u32) {
    char path[] = "/tmp/rut_sim_manifest_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);

    static const char kManifest[] = "upstream 42949672960 api-v1\n";
    REQUIRE(write(fd, kManifest, sizeof(kManifest) - 1) ==
            static_cast<ssize_t>(sizeof(kManifest) - 1));
    close(fd);

    Manifest manifest;
    CHECK(!load_manifest(path, manifest));
    unlink(path);
}

TEST(simulate_engine, load_manifest_rejects_too_long_pattern) {
    char pattern[160];
    pattern[0] = '/';
    for (u32 i = 1; i + 1 < sizeof(pattern); i++) pattern[i] = 'a';
    pattern[sizeof(pattern) - 1] = '\0';

    char manifest_buf[256];
    const i32 manifest_len =
        snprintf(manifest_buf, sizeof(manifest_buf), "route GET %s status 204\n", pattern);
    REQUIRE(manifest_len > 0);

    char path[] = "/tmp/rut_sim_manifest_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, manifest_buf, static_cast<size_t>(manifest_len)) == manifest_len);
    close(fd);

    Manifest manifest;
    CHECK(!load_manifest(path, manifest));
    unlink(path);
}

TEST(simulate_engine, load_manifest_rejects_invalid_inputs) {
    struct Case {
        const char* text;
    };

    static const Case kCases[] = {
        {"upstream x api-v1\n"},
        {"upstream 65536 api-v1\n"},
        {"route FETCH /x status 204\n"},
        {"route GET /x bounce 7\n"},
        {"route GET /x status 70000\n"},
        {"route GET /x proxy 65536\n"},
        {"route GET /x status 42949672960\n"},
        {"route GET /x proxy 42949672960\n"},
        {"route GET /x status\n"},
    };

    for (const auto& tc : kCases) {
        Manifest manifest;
        CHECK(!load_manifest_text(tc.text, &manifest));
    }
}

TEST(simulate_engine, param_route_matching_matrix) {
    struct Case {
        const char* req;
        u16 expected_status;
    };

    Manifest manifest{};
    manifest.route_count = 2;
    manifest.routes[0].method = 'G';
    strcpy(manifest.routes[0].pattern, "/users/:id");
    manifest.routes[0].action = ManifestAction::ReturnStatus;
    manifest.routes[0].status_code = 201;
    manifest.routes[1].method = 'G';
    strcpy(manifest.routes[1].pattern, "/teams/:team/members/:member");
    manifest.routes[1].action = ManifestAction::ReturnStatus;
    manifest.routes[1].status_code = 202;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    static const Case kCases[] = {
        {"GET /users/123 HTTP/1.1\r\nHost: x\r\n\r\n", 201},
        {"GET /users/123?active=1 HTTP/1.1\r\nHost: x\r\n\r\n", 201},
        {"GET /users/ HTTP/1.1\r\nHost: x\r\n\r\n", 200},
        {"GET /users HTTP/1.1\r\nHost: x\r\n\r\n", 200},
        {"GET /teams/alpha/members/bravo HTTP/1.1\r\nHost: x\r\n\r\n", 202},
        {"GET /teams/alpha/members/ HTTP/1.1\r\nHost: x\r\n\r\n", 200},
    };

    for (const auto& tc : kCases) {
        const auto result = simulate_one(engine, make_entry(tc.req, tc.expected_status));
        CHECK_EQ(result.verdict, Verdict::Match);
        CHECK_EQ(result.actual_status, tc.expected_status);
    }

    engine.shutdown();
    ctx.destroy();
}

TEST(simulate_engine, summary_counts_mismatch) {
    Manifest manifest{};
    manifest.upstream_count = 1;
    manifest.upstreams[0].id = 9;
    strcpy(manifest.upstreams[0].name, "edge");
    manifest.route_count = 2;
    manifest.routes[0].method = 'G';
    strcpy(manifest.routes[0].pattern, "/ok");
    manifest.routes[0].action = ManifestAction::ReturnStatus;
    manifest.routes[0].status_code = 200;
    manifest.routes[1].method = 'G';
    strcpy(manifest.routes[1].pattern, "/api");
    manifest.routes[1].action = ManifestAction::Proxy;
    manifest.routes[1].upstream_id = 9;

    ModuleContext ctx;
    Engine engine;
    REQUIRE(init_engine(manifest, ctx, engine));

    char path[] = "/tmp/rut_sim_capture_XXXXXX";
    i32 fd = mkstemp(path);
    REQUIRE(fd >= 0);
    CaptureFileHeader hdr;
    capture_file_header_init(&hdr);
    hdr.entry_count = 3;
    REQUIRE(write(fd, &hdr, sizeof(hdr)) == static_cast<ssize_t>(sizeof(hdr)));
    REQUIRE(capture_write_entry(fd, make_entry("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n", 200)) == 0);
    REQUIRE(capture_write_entry(
                fd, make_entry("GET /api/x HTTP/1.1\r\nHost: x\r\n\r\n", 503, "edge")) == 0);
    REQUIRE(capture_write_entry(
                fd, make_entry("GET /api/y HTTP/1.1\r\nHost: x\r\n\r\n", 503, "wrong")) == 0);
    close(fd);

    ReplayReader reader;
    REQUIRE(reader.open(path) == 0);
    const auto summary = simulate_file(engine, reader);
    CHECK_EQ(summary.total, 3u);
    CHECK_EQ(summary.matched, 2u);
    CHECK_EQ(summary.mismatched, 1u);
    CHECK_EQ(summary.failed, 0u);

    reader.close();
    unlink(path);
    engine.shutdown();
    ctx.destroy();
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
