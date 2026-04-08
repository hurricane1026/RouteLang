#include "rut/compiler/rir.h"
#include "rut/compiler/rir_builder.h"
#include "rut/jit/codegen.h"
#include "rut/jit/handler_abi.h"
#include "rut/jit/jit_engine.h"
#include "rut/jit/runtime_helpers.h"
#include "rut/runtime/connection.h"
#include "test.h"

using namespace rut;
using namespace rut::rir;
using namespace rut::jit;

// ── Helpers ────────────────────────────────────────────────────────

static Str lit(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return {s, n};
}

// Unwrap Expected.
#define V(expr)                                                \
    __extension__({                                            \
        auto&& _v_result = (expr);                             \
        REQUIRE(static_cast<bool>(_v_result));                 \
        static_cast<decltype(_v_result)&&>(_v_result).value(); \
    })

#define VOK(expr) REQUIRE(static_cast<bool>(expr))

// MmapArena-backed module initialization (same pattern as test_rir.cc).
struct TestContext {
    MmapArena arena;
    Module mod;

    bool init() {
        if (!arena.init(4096)) return false;
        mod.name = lit("test_jit.rut");
        mod.arena = &arena;

        static constexpr u32 kMaxFuncs = 8;
        mod.functions = arena.alloc_array<Function>(kMaxFuncs);
        if (!mod.functions) {
            arena.destroy();
            return false;
        }
        mod.func_count = 0;
        mod.func_cap = kMaxFuncs;
        static constexpr u32 kMaxStructs = 8;
        mod.struct_defs = arena.alloc_array<StructDef*>(kMaxStructs);
        if (!mod.struct_defs) {
            arena.destroy();
            return false;
        }
        mod.struct_count = 0;
        mod.struct_cap = kMaxStructs;
        return true;
    }

    void destroy() { arena.destroy(); }
};

// A minimal HTTP GET request for testing.
static const char kGetApiRequest[] =
    "GET /api/users HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "\r\n";

static const char kGetRootRequest[] =
    "GET / HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "\r\n";

// ── Tests ──────────────────────────────────────────────────────────

// Test: runtime helpers work from C++ (sanity check)
TEST(jit, helpers_sanity) {
    const char* out_ptr = nullptr;
    u32 out_len = 0;
    rut_helper_req_path(reinterpret_cast<const u8*>(kGetApiRequest),
                        sizeof(kGetApiRequest) - 1,
                        &out_ptr,
                        &out_len);
    rut::test::out("  path: ");
    if (out_ptr) {
        for (u32 i = 0; i < out_len; i++) {
            char ch = out_ptr[i];
            (void)::write(1, &ch, 1);
        }
    }
    rut::test::out(" len=");
    rut::test::out_int(static_cast<int>(out_len));
    rut::test::out("\n");
    CHECK(out_ptr != nullptr);
    CHECK(out_len > 0);
}

// Test: Simple handler that always returns 200.
// RIR equivalent: return 200
TEST(jit, return_200) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("always_200"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    b.set_insert_point(fn, entry);
    VOK(b.emit_ret_status(200));

    // Codegen
    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    // JIT compile
    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    // Lookup
    void* addr = engine.lookup("handler_always_200");
    REQUIRE(addr != nullptr);

    auto handler = reinterpret_cast<HandlerFn>(addr);
    auto result = HandlerResult::unpack(handler(nullptr,
                                                nullptr,
                                                reinterpret_cast<const u8*>(kGetApiRequest),
                                                sizeof(kGetApiRequest) - 1,
                                                nullptr));

    CHECK(result.action == HandlerAction::ReturnStatus);
    CHECK(result.status_code == 200);

    engine.shutdown();
    tc.destroy();
}

// Test: Handler with path prefix guard.
// RIR equivalent:
//   %path = req.path
//   %prefix = const.str "/api"
//   %has = str.has_prefix %path, %prefix
//   br %has, ok_block, reject_block
//   reject_block: ret.status 404
//   ok_block: ret.status 200
TEST(jit, guard_path_prefix) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("path_guard"), lit("/api"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto ok_blk = V(b.create_block(fn, lit("ok")));
    auto reject_blk = V(b.create_block(fn, lit("reject")));

    // Entry block: check path prefix
    b.set_insert_point(fn, entry);
    auto path = V(b.emit_req_path());
    auto prefix = V(b.emit_const_str(lit("/api")));
    auto has = V(b.emit_str_has_prefix(path, prefix));
    VOK(b.emit_br(has, ok_blk, reject_blk));

    // Reject block: 404
    b.set_insert_point(fn, reject_blk);
    VOK(b.emit_ret_status(404));

    // OK block: 200
    b.set_insert_point(fn, ok_blk);
    VOK(b.emit_ret_status(200));

    // Codegen + JIT
    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto* addr = engine.lookup("handler_path_guard");
    REQUIRE(addr != nullptr);
    auto handler = reinterpret_cast<HandlerFn>(addr);

    // Test with /api path — should return 200
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 200);
    }

    // Test with / path — should return 404
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetRootRequest),
                                               sizeof(kGetRootRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 404);
    }

    engine.shutdown();
    tc.destroy();
}

// Test: Handler that reads a header.
// RIR equivalent:
//   %auth = req.header "Authorization"
//   %is_nil = opt.is_nil %auth
//   br %is_nil, no_auth, has_auth
//   no_auth: ret.status 401
//   has_auth: ret.status 200
TEST(jit, header_check) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("auth_check"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto no_auth = V(b.create_block(fn, lit("no_auth")));
    auto has_auth = V(b.create_block(fn, lit("has_auth")));

    b.set_insert_point(fn, entry);
    auto auth = V(b.emit_req_header(lit("Authorization")));
    auto is_nil = V(b.emit_opt_is_nil(auth));
    VOK(b.emit_br(is_nil, no_auth, has_auth));

    b.set_insert_point(fn, no_auth);
    VOK(b.emit_ret_status(401));

    b.set_insert_point(fn, has_auth);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto* addr = engine.lookup("handler_auth_check");
    REQUIRE(addr != nullptr);
    auto handler = reinterpret_cast<HandlerFn>(addr);

    // Request without Authorization → 401
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 401);
    }

    // Request with Authorization → 200
    {
        static const char req[] =
            "GET /api HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Authorization: Bearer token123\r\n"
            "\r\n";
        auto r = HandlerResult::unpack(
            handler(nullptr, nullptr, reinterpret_cast<const u8*>(req), sizeof(req) - 1, nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 200);
    }

    engine.shutdown();
    tc.destroy();
}

// Test: Handler with comparison.
// RIR equivalent:
//   %method = req.method
//   %get = const.method 'G'
//   %is_get = cmp.eq %method, %get
//   br %is_get, ok, reject
//   reject: ret.status 405
//   ok: ret.status 200
TEST(jit, method_check) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("method_check"), lit("/"), 0));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto ok = V(b.create_block(fn, lit("ok")));
    auto reject = V(b.create_block(fn, lit("reject")));

    b.set_insert_point(fn, entry);
    auto method = V(b.emit_req_method());
    auto get_const = V(b.emit_const_method(static_cast<u8>(0)));  // GET = 0 in HttpMethod enum
    auto is_get = V(b.emit_cmp(Opcode::CmpEq, method, get_const));
    VOK(b.emit_br(is_get, ok, reject));

    b.set_insert_point(fn, reject);
    VOK(b.emit_ret_status(405));

    b.set_insert_point(fn, ok);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto* addr = engine.lookup("handler_method_check");
    REQUIRE(addr != nullptr);
    auto handler = reinterpret_cast<HandlerFn>(addr);

    // GET request → 200
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        // Method enum value: GET=0 matches const_method(0)
        CHECK(r.status_code == 200);
    }

    // POST request → 405
    {
        static const char post_req[] =
            "POST /api HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(post_req),
                                               sizeof(post_req) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 405);
    }

    engine.shutdown();
    tc.destroy();
}

// ── Runtime Helper Tests ───────────────────────────────────────────

TEST(helpers, req_method) {
    // GET
    u8 m = rut_helper_req_method(reinterpret_cast<const u8*>(kGetApiRequest),
                                 sizeof(kGetApiRequest) - 1);
    CHECK(m == 0);  // HttpMethod::GET = 0

    // POST
    static const char post[] = "POST / HTTP/1.1\r\nHost: h\r\n\r\n";
    m = rut_helper_req_method(reinterpret_cast<const u8*>(post), sizeof(post) - 1);
    CHECK(m == 1);  // HttpMethod::POST = 1
}

TEST(helpers, req_header_case_insensitive) {
    static const char req[] =
        "GET / HTTP/1.1\r\n"
        "Content-Type: text/html\r\n"
        "\r\n";
    u8 has = 0;
    const char* ptr = nullptr;
    u32 len = 0;

    // Exact case
    rut_helper_req_header(
        reinterpret_cast<const u8*>(req), sizeof(req) - 1, "Content-Type", 12, &has, &ptr, &len);
    CHECK(has == 1);
    CHECK(len == 9);  // "text/html"

    // Lowercase lookup
    has = 0;
    ptr = nullptr;
    len = 0;
    rut_helper_req_header(
        reinterpret_cast<const u8*>(req), sizeof(req) - 1, "content-type", 12, &has, &ptr, &len);
    CHECK(has == 1);
    CHECK(len == 9);

    // Missing header
    has = 1;
    rut_helper_req_header(
        reinterpret_cast<const u8*>(req), sizeof(req) - 1, "X-Missing", 9, &has, &ptr, &len);
    CHECK(has == 0);
}

TEST(helpers, req_remote_addr) {
    Connection conn;
    conn.reset();
    conn.peer_addr = 0x0A000001;  // 10.0.0.1 in network order
    u32 addr = rut_helper_req_remote_addr(&conn);
    CHECK(addr == 0x0A000001);
}

TEST(helpers, str_has_prefix_edge_cases) {
    // Empty prefix matches everything
    CHECK(rut_helper_str_has_prefix("hello", 5, "", 0) == 1);

    // Equal strings
    CHECK(rut_helper_str_has_prefix("/api", 4, "/api", 4) == 1);

    // Prefix longer than string
    CHECK(rut_helper_str_has_prefix("/a", 2, "/api", 4) == 0);

    // Empty string with non-empty prefix
    CHECK(rut_helper_str_has_prefix("", 0, "/", 1) == 0);

    // Both empty
    CHECK(rut_helper_str_has_prefix("", 0, "", 0) == 1);
}

TEST(helpers, str_trim_prefix) {
    const char* out = nullptr;
    u32 len = 0;

    // Prefix present
    rut_helper_str_trim_prefix("/api/users", 10, "/api", 4, &out, &len);
    CHECK(len == 6);  // "/users"
    CHECK(out[0] == '/');
    CHECK(out[1] == 'u');

    // Prefix not present
    rut_helper_str_trim_prefix("/other", 6, "/api", 4, &out, &len);
    CHECK(len == 6);  // unchanged
    CHECK(out[0] == '/');
    CHECK(out[1] == 'o');

    // Trim entire string
    rut_helper_str_trim_prefix("/api", 4, "/api", 4, &out, &len);
    CHECK(len == 0);
}

// ── HandlerResult pack/unpack Tests ───────────────────────────────

TEST(result, pack_unpack_status) {
    auto r = HandlerResult::make_status(404);
    u64 packed = r.pack();
    auto r2 = HandlerResult::unpack(packed);
    CHECK(r2.action == HandlerAction::ReturnStatus);
    CHECK(r2.status_code == 404);
    CHECK(r2.upstream_id == 0);
    CHECK(r2.next_state == 0);
}

TEST(result, pack_unpack_forward) {
    auto r = HandlerResult::make_forward(7);
    u64 packed = r.pack();
    auto r2 = HandlerResult::unpack(packed);
    CHECK(r2.action == HandlerAction::Forward);
    CHECK(r2.upstream_id == 7);
    CHECK(r2.status_code == 0);
}

TEST(result, pack_unpack_yield) {
    auto r = HandlerResult::make_yield(3, YieldKind::Forward);
    u64 packed = r.pack();
    auto r2 = HandlerResult::unpack(packed);
    CHECK(r2.action == HandlerAction::Yield);
    CHECK(r2.next_state == 3);
    CHECK(r2.yield_kind == YieldKind::Forward);
}

TEST(result, pack_matches_codegen_layout) {
    // Verify pack() produces the same bit layout as the codegen's i64.
    // Codegen for RetStatus(200): action(0) | (200 << 8)
    u64 codegen_200 = 0 | (static_cast<u64>(200) << 8);
    auto r = HandlerResult::unpack(codegen_200);
    CHECK(r.action == HandlerAction::ReturnStatus);
    CHECK(r.status_code == 200);

    // And the other direction
    auto r2 = HandlerResult::make_status(200);
    CHECK(r2.pack() == codegen_200);
}

// ── Codegen: Unconditional Jump ───────────────────────────────────

// handler:
//   entry: jmp ok
//   ok: ret.status 200
TEST(jit, unconditional_jmp) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("jmp_test"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto ok = V(b.create_block(fn, lit("ok")));

    b.set_insert_point(fn, entry);
    VOK(b.emit_jmp(ok));

    b.set_insert_point(fn, ok);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_jmp_test"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.action == HandlerAction::ReturnStatus);
    CHECK(r.status_code == 200);

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: ConstBool + comparison operators ─────────────────────

// handler:
//   %t = const.bool true
//   %f = const.bool false
//   %eq = cmp.eq %t, %f  → false
//   br %eq, bad, good
//   bad: ret.status 500
//   good: ret.status 200
TEST(jit, const_bool_and_cmp_ne) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("bool_test"), lit("/"), 0));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto good = V(b.create_block(fn, lit("good")));
    auto bad = V(b.create_block(fn, lit("bad")));

    b.set_insert_point(fn, entry);
    auto t = V(b.emit_const_bool(true));
    auto f = V(b.emit_const_bool(false));
    auto eq = V(b.emit_cmp(Opcode::CmpEq, t, f));
    VOK(b.emit_br(eq, bad, good));

    b.set_insert_point(fn, bad);
    VOK(b.emit_ret_status(500));

    b.set_insert_point(fn, good);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_bool_test"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.action == HandlerAction::ReturnStatus);
    CHECK(r.status_code == 200);  // true != false → good path

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: ConstI32 + CmpLt ─────────────────────────────────────

// Simulates: if (42 < 100) return 200 else return 500
TEST(jit, const_i32_cmp_lt) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("i32_lt"), lit("/"), 0));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto yes = V(b.create_block(fn, lit("yes")));
    auto no = V(b.create_block(fn, lit("no")));

    b.set_insert_point(fn, entry);
    auto a = V(b.emit_const_i32(42));
    auto bb_val = V(b.emit_const_i32(100));
    auto lt = V(b.emit_cmp(Opcode::CmpLt, a, bb_val));
    VOK(b.emit_br(lt, yes, no));

    b.set_insert_point(fn, yes);
    VOK(b.emit_ret_status(200));

    b.set_insert_point(fn, no);
    VOK(b.emit_ret_status(500));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_i32_lt"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.action == HandlerAction::ReturnStatus);
    CHECK(r.status_code == 200);  // 42 < 100 → yes

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: Unsigned comparison (ByteSize) ───────────────────────

// Verifies that CmpLt on unsigned types uses unsigned predicates.
// ByteSize is i64 with unsigned semantics. A large ByteSize value
// has the high bit set; signed comparison would treat it as negative.
//
//   %a = const.bytesize 0x8000000000000000  (2^63, large positive unsigned)
//   %b = const.bytesize 1
//   %lt = cmp.lt %a, %b   → unsigned: false (2^63 > 1)
//                          → WRONG if signed: true (-2^63 < 1)
//   br %lt, wrong, correct
//   wrong: ret.status 500
//   correct: ret.status 200
TEST(jit, unsigned_cmp_lt) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("ucmp_lt"), lit("/"), 0));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto wrong = V(b.create_block(fn, lit("wrong")));
    auto correct = V(b.create_block(fn, lit("correct")));

    b.set_insert_point(fn, entry);
    // 0x8000000000000000 = 2^63 — high bit set, positive as unsigned
    auto a = V(b.emit_const_bytesize(static_cast<i64>(0x8000000000000000ULL)));
    auto bb_val = V(b.emit_const_bytesize(1));
    auto lt = V(b.emit_cmp(Opcode::CmpLt, a, bb_val));
    VOK(b.emit_br(lt, wrong, correct));

    b.set_insert_point(fn, wrong);
    VOK(b.emit_ret_status(500));

    b.set_insert_point(fn, correct);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_ucmp_lt"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.action == HandlerAction::ReturnStatus);
    CHECK(r.status_code == 200);  // 2^63 is NOT < 1 (unsigned)

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: OptUnwrap ────────────────────────────────────────────

// handler:
//   %hdr = req.header "Host"
//   %nil = opt.is_nil %hdr
//   br %nil, missing, found
//   missing: ret.status 400
//   found:
//     %val = opt.unwrap %hdr
//     %prefix = const.str "localhost"
//     %match = str.has_prefix %val, %prefix
//     br %match, ok, mismatch
//   ok: ret.status 200
//   mismatch: ret.status 421
TEST(jit, opt_unwrap_and_use) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("unwrap_test"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto missing = V(b.create_block(fn, lit("missing")));
    auto found = V(b.create_block(fn, lit("found")));
    auto ok = V(b.create_block(fn, lit("ok")));
    auto mismatch = V(b.create_block(fn, lit("mismatch")));

    b.set_insert_point(fn, entry);
    auto hdr = V(b.emit_req_header(lit("Host")));
    auto nil = V(b.emit_opt_is_nil(hdr));
    VOK(b.emit_br(nil, missing, found));

    b.set_insert_point(fn, missing);
    VOK(b.emit_ret_status(400));

    // Get the inner Str type for unwrap
    auto str_ty = V(b.make_type(TypeKind::Str));

    b.set_insert_point(fn, found);
    auto val = V(b.emit_opt_unwrap(hdr, str_ty));
    auto prefix = V(b.emit_const_str(lit("localhost")));
    auto match = V(b.emit_str_has_prefix(val, prefix));
    VOK(b.emit_br(match, ok, mismatch));

    b.set_insert_point(fn, ok);
    VOK(b.emit_ret_status(200));

    b.set_insert_point(fn, mismatch);
    VOK(b.emit_ret_status(421));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_unwrap_test"));
    REQUIRE(handler != nullptr);

    // Request with Host: localhost → 200
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 200);
    }

    // Request with Host: example.com → 421
    {
        static const char req[] =
            "GET / HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "\r\n";
        auto r = HandlerResult::unpack(
            handler(nullptr, nullptr, reinterpret_cast<const u8*>(req), sizeof(req) - 1, nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 421);
    }

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: StrTrimPrefix ────────────────────────────────────────

// handler:
//   %path = req.path
//   %prefix = const.str "/api"
//   %trimmed = str.trim_prefix %path, %prefix
//   %slash = const.str "/users"
//   %match = str.has_prefix %trimmed, %slash
//   br %match, ok, reject
//   ok: ret.status 200
//   reject: ret.status 404
TEST(jit, str_trim_prefix) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("trim_test"), lit("/api"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto ok = V(b.create_block(fn, lit("ok")));
    auto reject = V(b.create_block(fn, lit("reject")));

    b.set_insert_point(fn, entry);
    auto path = V(b.emit_req_path());
    auto prefix = V(b.emit_const_str(lit("/api")));
    auto trimmed = V(b.emit_str_trim_prefix(path, prefix));
    auto suffix = V(b.emit_const_str(lit("/users")));
    auto match = V(b.emit_str_has_prefix(trimmed, suffix));
    VOK(b.emit_br(match, ok, reject));

    b.set_insert_point(fn, ok);
    VOK(b.emit_ret_status(200));

    b.set_insert_point(fn, reject);
    VOK(b.emit_ret_status(404));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_trim_test"));
    REQUIRE(handler != nullptr);

    // /api/users → trim "/api" → "/users" → has_prefix "/users" → 200
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 200);
    }

    // / → trim "/api" fails → "/" → has_prefix "/users" → 404
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetRootRequest),
                                               sizeof(kGetRootRequest) - 1,
                                               nullptr));
        CHECK(r.action == HandlerAction::ReturnStatus);
        CHECK(r.status_code == 404);
    }

    engine.shutdown();
    tc.destroy();
}

// ── Runtime helper: req_remote_addr via Connection ────────────────
// ReqRemoteAddr returns TypeKind::IP which can't be compared with
// ConstI32 (TypeKind::I32) at the RIR level — IP comparisons use
// IpInCidr (Phase 2). Here we test the helper directly from C++
// and verify the codegen emits a valid call.

TEST(jit, req_remote_addr) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    // Minimal handler that calls req.remote_addr then returns 200.
    // This verifies the codegen emits the helper call correctly;
    // we check the addr value via the C++ helper test above.
    auto* fn = V(b.create_function(lit("addr_test"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));

    b.set_insert_point(fn, entry);
    V(b.emit_req_remote_addr());  // exercise codegen, discard result
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_addr_test"));
    REQUIRE(handler != nullptr);

    Connection conn;
    conn.reset();
    conn.peer_addr = 0x0100007F;
    auto r = HandlerResult::unpack(handler(&conn,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.status_code == 200);

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: Multiple functions in one module ─────────────────────

TEST(jit, multiple_functions) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    // Function 1: always 200
    {
        auto* fn = V(b.create_function(lit("fn_a"), lit("/a"), 'G'));
        auto entry = V(b.create_block(fn, lit("entry")));
        b.set_insert_point(fn, entry);
        VOK(b.emit_ret_status(200));
    }

    // Function 2: always 404
    {
        auto* fn = V(b.create_function(lit("fn_b"), lit("/b"), 'G'));
        auto entry = V(b.create_block(fn, lit("entry")));
        b.set_insert_point(fn, entry);
        VOK(b.emit_ret_status(404));
    }

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto fn_a = reinterpret_cast<HandlerFn>(engine.lookup("handler_fn_a"));
    auto fn_b = reinterpret_cast<HandlerFn>(engine.lookup("handler_fn_b"));
    REQUIRE(fn_a != nullptr);
    REQUIRE(fn_b != nullptr);

    auto ra = HandlerResult::unpack(fn_a(nullptr,
                                         nullptr,
                                         reinterpret_cast<const u8*>(kGetApiRequest),
                                         sizeof(kGetApiRequest) - 1,
                                         nullptr));
    CHECK(ra.status_code == 200);

    auto rb = HandlerResult::unpack(fn_b(nullptr,
                                         nullptr,
                                         reinterpret_cast<const u8*>(kGetApiRequest),
                                         sizeof(kGetApiRequest) - 1,
                                         nullptr));
    CHECK(rb.status_code == 404);

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: Diamond CFG (if/else join) ───────────────────────────

// handler:
//   entry:
//     %path = req.path
//     %prefix = const.str "/admin"
//     %is_admin = str.has_prefix %path, %prefix
//     br %is_admin, admin, user
//   admin:
//     jmp merge
//   user:
//     jmp merge
//   merge:
//     ret.status 200
//
// Tests: forward Jmp + multiple predecessors on merge block
TEST(jit, diamond_cfg) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("diamond"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));
    auto admin = V(b.create_block(fn, lit("admin")));
    auto user = V(b.create_block(fn, lit("user")));
    auto merge = V(b.create_block(fn, lit("merge")));

    b.set_insert_point(fn, entry);
    auto path = V(b.emit_req_path());
    auto prefix = V(b.emit_const_str(lit("/admin")));
    auto is_admin = V(b.emit_str_has_prefix(path, prefix));
    VOK(b.emit_br(is_admin, admin, user));

    b.set_insert_point(fn, admin);
    VOK(b.emit_jmp(merge));

    b.set_insert_point(fn, user);
    VOK(b.emit_jmp(merge));

    b.set_insert_point(fn, merge);
    VOK(b.emit_ret_status(200));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_diamond"));
    REQUIRE(handler != nullptr);

    // Both paths merge to 200
    auto r1 = HandlerResult::unpack(handler(nullptr,
                                            nullptr,
                                            reinterpret_cast<const u8*>(kGetApiRequest),
                                            sizeof(kGetApiRequest) - 1,
                                            nullptr));
    CHECK(r1.status_code == 200);

    static const char admin_req[] = "GET /admin/dashboard HTTP/1.1\r\nHost: h\r\n\r\n";
    auto r2 = HandlerResult::unpack(handler(
        nullptr, nullptr, reinterpret_cast<const u8*>(admin_req), sizeof(admin_req) - 1, nullptr));
    CHECK(r2.status_code == 200);

    engine.shutdown();
    tc.destroy();
}

// ── Codegen: Deep chain (entry → b1 → b2 → b3 → ret) ────────────

TEST(jit, chained_blocks) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("chain"), lit("/"), 0));
    auto b0 = V(b.create_block(fn, lit("b0")));
    auto b1 = V(b.create_block(fn, lit("b1")));
    auto b2 = V(b.create_block(fn, lit("b2")));
    auto b3 = V(b.create_block(fn, lit("b3")));

    b.set_insert_point(fn, b0);
    VOK(b.emit_jmp(b1));

    b.set_insert_point(fn, b1);
    VOK(b.emit_jmp(b2));

    b.set_insert_point(fn, b2);
    VOK(b.emit_jmp(b3));

    b.set_insert_point(fn, b3);
    VOK(b.emit_ret_status(201));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_chain"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.status_code == 201);

    engine.shutdown();
    tc.destroy();
}

// ── JIT Engine: lookup failure ────────────────────────────────────

TEST(jit, lookup_nonexistent) {
    JitEngine engine;
    REQUIRE(engine.init());

    // No modules compiled — any lookup should return nullptr
    void* addr = engine.lookup("nonexistent_function");
    CHECK(addr == nullptr);

    engine.shutdown();
}

// ── Codegen: RetForward ─────────────────────────────────────────────

// handler:
//   %upstream = const.i32 3
//   ret.forward %upstream
TEST(jit, ret_forward) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("proxy_test"), lit("/"), 'G'));
    auto entry = V(b.create_block(fn, lit("entry")));

    b.set_insert_point(fn, entry);
    auto upstream = V(b.emit_const_i32(3));
    VOK(b.emit_ret_forward(upstream));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_proxy_test"));
    REQUIRE(handler != nullptr);

    auto r = HandlerResult::unpack(handler(nullptr,
                                           nullptr,
                                           reinterpret_cast<const u8*>(kGetApiRequest),
                                           sizeof(kGetApiRequest) - 1,
                                           nullptr));
    CHECK(r.action == HandlerAction::Forward);
    CHECK(r.upstream_id == 3);

    engine.shutdown();
    tc.destroy();
}

// ── Complex: multi-guard handler ──────────────────────────────────

// Simulates a realistic middleware chain:
//   guard req.method == GET else { return 405 }
//   guard req.path.has_prefix("/api") else { return 404 }
//   guard req.header("Authorization") != nil else { return 401 }
//   return 200
TEST(jit, multi_guard) {
    TestContext tc;
    REQUIRE(tc.init());

    Builder b;
    b.init(&tc.mod);

    auto* fn = V(b.create_function(lit("multi_guard"), lit("/api"), 'G'));
    auto check_method = V(b.create_block(fn, lit("check_method")));
    auto check_path = V(b.create_block(fn, lit("check_path")));
    auto check_auth = V(b.create_block(fn, lit("check_auth")));
    auto ok = V(b.create_block(fn, lit("ok")));
    auto err_405 = V(b.create_block(fn, lit("err_405")));
    auto err_404 = V(b.create_block(fn, lit("err_404")));
    auto err_401 = V(b.create_block(fn, lit("err_401")));

    // Guard 1: method == GET
    b.set_insert_point(fn, check_method);
    auto method = V(b.emit_req_method());
    auto get = V(b.emit_const_method(0));
    auto is_get = V(b.emit_cmp(Opcode::CmpEq, method, get));
    VOK(b.emit_br(is_get, check_path, err_405));

    // Guard 2: path.has_prefix("/api")
    b.set_insert_point(fn, check_path);
    auto path = V(b.emit_req_path());
    auto prefix = V(b.emit_const_str(lit("/api")));
    auto has_prefix = V(b.emit_str_has_prefix(path, prefix));
    VOK(b.emit_br(has_prefix, check_auth, err_404));

    // Guard 3: header("Authorization") != nil
    b.set_insert_point(fn, check_auth);
    auto auth = V(b.emit_req_header(lit("Authorization")));
    auto no_auth = V(b.emit_opt_is_nil(auth));
    VOK(b.emit_br(no_auth, err_401, ok));

    b.set_insert_point(fn, ok);
    VOK(b.emit_ret_status(200));

    b.set_insert_point(fn, err_405);
    VOK(b.emit_ret_status(405));

    b.set_insert_point(fn, err_404);
    VOK(b.emit_ret_status(404));

    b.set_insert_point(fn, err_401);
    VOK(b.emit_ret_status(401));

    auto cg = codegen(tc.mod);
    REQUIRE(cg.ok);

    JitEngine engine;
    REQUIRE(engine.init());
    REQUIRE(engine.compile(cg.mod, cg.ctx));

    auto handler = reinterpret_cast<HandlerFn>(engine.lookup("handler_multi_guard"));
    REQUIRE(handler != nullptr);

    // All guards pass: GET /api with Authorization → 200
    {
        static const char req[] =
            "GET /api/v1 HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Authorization: Bearer tok\r\n"
            "\r\n";
        auto r = HandlerResult::unpack(
            handler(nullptr, nullptr, reinterpret_cast<const u8*>(req), sizeof(req) - 1, nullptr));
        CHECK(r.status_code == 200);
    }

    // Wrong method: POST → 405
    {
        static const char req[] =
            "POST /api/v1 HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Authorization: Bearer tok\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        auto r = HandlerResult::unpack(
            handler(nullptr, nullptr, reinterpret_cast<const u8*>(req), sizeof(req) - 1, nullptr));
        CHECK(r.status_code == 405);
    }

    // Wrong path: GET / → 404
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetRootRequest),
                                               sizeof(kGetRootRequest) - 1,
                                               nullptr));
        CHECK(r.status_code == 404);
    }

    // Missing auth: GET /api without Authorization → 401
    {
        auto r = HandlerResult::unpack(handler(nullptr,
                                               nullptr,
                                               reinterpret_cast<const u8*>(kGetApiRequest),
                                               sizeof(kGetApiRequest) - 1,
                                               nullptr));
        CHECK(r.status_code == 401);
    }

    engine.shutdown();
    tc.destroy();
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
