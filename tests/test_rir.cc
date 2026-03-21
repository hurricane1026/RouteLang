#include "rout/compiler/rir.h"
#include "rout/compiler/rir_builder.h"
#include "rout/compiler/rir_printer.h"

#include "test.h"

using namespace rout;
using namespace rout::rir;

// ── Helpers ─────────────────────────────────────────────────────────

static Str lit(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return {s, n};
}

// Arena-backed module initialization.
struct TestContext {
    Arena arena;
    Module mod;

    bool init() {
        if (arena.init(4096) != 0) return false;
        mod.name = lit("test.rue");
        mod.arena = &arena;

        static constexpr u32 kMaxFuncs = 8;
        mod.functions = arena.alloc_array<Function>(kMaxFuncs);
        mod.func_count = 0;
        mod.func_cap = kMaxFuncs;
        static constexpr u32 kMaxStructs = 64;
        mod.struct_defs = arena.alloc_array<StructDef*>(kMaxStructs);
        mod.struct_count = 0;
        mod.struct_cap = kMaxStructs;
        return true;
    }

    void destroy() { arena.destroy(); }
};

// ── Type System Tests ───────────────────────────────────────────────

TEST(RirTypes, PrimitiveTypes) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* t_bool = b.make_type(TypeKind::Bool);
    auto* t_str = b.make_type(TypeKind::Str);
    auto* t_i32 = b.make_type(TypeKind::I32);

    CHECK_EQ(static_cast<u8>(t_bool->kind), static_cast<u8>(TypeKind::Bool));
    CHECK_EQ(static_cast<u8>(t_str->kind), static_cast<u8>(TypeKind::Str));
    CHECK_EQ(static_cast<u8>(t_i32->kind), static_cast<u8>(TypeKind::I32));
    CHECK(t_bool->inner == nullptr);

    ctx.destroy();
}

TEST(RirTypes, OptionalType) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* t_str = b.make_type(TypeKind::Str);
    auto* t_opt_str = b.make_type(TypeKind::Optional, t_str);

    CHECK_EQ(static_cast<u8>(t_opt_str->kind), static_cast<u8>(TypeKind::Optional));
    CHECK(t_opt_str->inner == t_str);
    CHECK_EQ(static_cast<u8>(t_opt_str->inner->kind), static_cast<u8>(TypeKind::Str));

    ctx.destroy();
}

TEST(RirTypes, StructType) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* t_str = b.make_type(TypeKind::Str);
    FieldDef fields[2] = {{lit("id"), t_str}, {lit("role"), t_str}};
    auto* sd = b.create_struct(lit("User"), fields, 2);

    CHECK(sd->name.eq(lit("User")));
    CHECK_EQ(sd->field_count, 2u);
    CHECK(sd->fields()[0].name.eq(lit("id")));
    CHECK(sd->fields()[1].name.eq(lit("role")));

    ctx.destroy();
}

// ── Builder Tests ───────────────────────────────────────────────────

TEST(RirBuilder, CreateFunctionAndBlocks) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("handle_get_users_id"), lit("/users/:id"), 1);
    REQUIRE(fn != nullptr);
    CHECK(fn->name.eq(lit("handle_get_users_id")));
    CHECK_EQ(ctx.mod.func_count, 1u);

    auto entry = b.create_block(fn, lit("entry"));
    auto blk1 = b.create_block(fn, lit("block_check"));

    CHECK_EQ(entry.id, 0u);
    CHECK_EQ(blk1.id, 1u);
    CHECK_EQ(fn->block_count, 2u);

    ctx.destroy();
}

TEST(RirBuilder, ConstantInstructions) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto v0 = b.emit_const_str(lit("hello"), {1, 0});
    auto v1 = b.emit_const_i32(42, {2, 0});
    auto v2 = b.emit_const_bool(true, {3, 0});
    auto v3 = b.emit_const_duration(300, {4, 0});  // 5 minutes

    CHECK_EQ(v0.id, 0u);
    CHECK_EQ(v1.id, 1u);
    CHECK_EQ(v2.id, 2u);
    CHECK_EQ(v3.id, 3u);

    CHECK_EQ(fn->value_count, 4u);
    CHECK_EQ(fn->entry()->inst_count, 4u);

    // Verify instruction data.
    auto& inst0 = fn->entry()->insts[0];
    CHECK_EQ(static_cast<u8>(inst0.op), static_cast<u8>(Opcode::ConstStr));
    CHECK(inst0.imm.str_val.eq(lit("hello")));
    CHECK_EQ(inst0.loc.line, 1u);

    auto& inst1 = fn->entry()->insts[1];
    CHECK_EQ(inst1.imm.i32_val, 42);

    ctx.destroy();
}

TEST(RirBuilder, RequestAccess) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto v_hdr = b.emit_req_header(lit("Authorization"), {1, 0});
    auto v_param = b.emit_req_param(lit("id"), {2, 0});
    auto v_method = b.emit_req_method({3, 0});

    // req.header returns Optional(str).
    CHECK_EQ(static_cast<u8>(fn->values[v_hdr.id].type->kind), static_cast<u8>(TypeKind::Optional));
    CHECK_EQ(static_cast<u8>(fn->values[v_hdr.id].type->inner->kind),
             static_cast<u8>(TypeKind::Str));

    // req.param returns str.
    CHECK_EQ(static_cast<u8>(fn->values[v_param.id].type->kind), static_cast<u8>(TypeKind::Str));

    // req.method returns Method.
    CHECK_EQ(static_cast<u8>(fn->values[v_method.id].type->kind),
             static_cast<u8>(TypeKind::Method));

    ctx.destroy();
}

TEST(RirBuilder, BinaryOperations) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto v0 = b.emit_const_str(lit("Bearer "));
    auto v1 = b.emit_req_header(lit("Authorization"));
    auto v2 = b.emit_str_has_prefix(v1, v0);

    // str.has_prefix returns bool.
    CHECK_EQ(static_cast<u8>(fn->values[v2.id].type->kind), static_cast<u8>(TypeKind::Bool));

    // Check operands.
    auto& inst = fn->entry()->insts[2];
    CHECK_EQ(inst.operand_count, 2u);
    CHECK_EQ(inst.operands[0].id, v1.id);
    CHECK_EQ(inst.operands[1].id, v0.id);

    ctx.destroy();
}

TEST(RirBuilder, Terminators) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    auto then_blk = b.create_block(fn, lit("then"));
    auto else_blk = b.create_block(fn, lit("else"));

    // Entry block: br %cond, then, else
    b.set_insert_point(fn, entry);
    auto cond = b.emit_const_bool(true);
    b.emit_br(cond, then_blk, else_blk);

    // Then block: ret.status 200
    b.set_insert_point(fn, then_blk);
    b.emit_ret_status(200);

    // Else block: ret.status 404
    b.set_insert_point(fn, else_blk);
    b.emit_ret_status(404);

    // Verify entry terminator.
    auto* term = fn->entry()->terminator();
    REQUIRE(term != nullptr);
    CHECK_EQ(static_cast<u8>(term->op), static_cast<u8>(Opcode::Br));
    CHECK(term->is_terminator());
    CHECK_EQ(term->imm.block_targets[0].id, then_blk.id);
    CHECK_EQ(term->imm.block_targets[1].id, else_blk.id);

    ctx.destroy();
}

TEST(RirBuilder, YieldCountsStates) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    CHECK_EQ(fn->yield_count, 0u);

    b.emit_yield_http_get(lit("http://auth/verify"), kNoValue);
    CHECK_EQ(fn->yield_count, 1u);

    // Each yield adds a state machine boundary.
    // states = yield_count + 1.

    ctx.destroy();
}

TEST(RirBuilder, VariadicStrInterpolate) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    // Build 5 parts (exceeds kMaxInlineOperands = 3).
    ValueId parts[5];
    parts[0] = b.emit_const_str(lit("GET"));
    parts[1] = b.emit_const_str(lit(" "));
    parts[2] = b.emit_const_str(lit("/users"));
    parts[3] = b.emit_const_str(lit(" "));
    parts[4] = b.emit_const_str(lit("HTTP/1.1"));

    auto result = b.emit_str_interpolate(parts, 5);
    CHECK(result != kNoValue);

    // Verify operand access works for both inline and overflow.
    auto& inst = fn->entry()->insts[5];  // 5 consts + 1 interpolate
    CHECK_EQ(inst.operand_count, 5u);
    CHECK_EQ(inst.operand(0).id, parts[0].id);
    CHECK_EQ(inst.operand(1).id, parts[1].id);
    CHECK_EQ(inst.operand(2).id, parts[2].id);
    CHECK_EQ(inst.operand(3).id, parts[3].id);  // via extra_operands
    CHECK_EQ(inst.operand(4).id, parts[4].id);  // via extra_operands

    ctx.destroy();
}

// ── Integration Test: DESIGN.md auth example ────────────────────────
// Reconstructs the inlined auth + rateLimit + proxy handler from §11.2.3.

TEST(RirIntegration, AuthHandlerFromDesignDoc) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("handle_get_users_id"), lit("/users/:id"), 1);

    // Create all blocks.
    auto entry = b.create_block(fn, lit("entry"));
    auto blk_check_prefix = b.create_block(fn, lit("block_check_prefix"));
    auto blk_decode_jwt = b.create_block(fn, lit("block_decode_jwt"));
    auto blk_check_role = b.create_block(fn, lit("block_check_role"));
    auto blk_auth_ok = b.create_block(fn, lit("block_auth_ok"));
    auto blk_proxy = b.create_block(fn, lit("block_proxy"));
    auto blk_reject_401 = b.create_block(fn, lit("block_reject_401"));
    auto blk_reject_403 = b.create_block(fn, lit("block_reject_403"));
    auto blk_reject_429 = b.create_block(fn, lit("block_reject_429"));

    // entry: check token exists
    b.set_insert_point(fn, entry);
    auto token = b.emit_req_header(lit("Authorization"), {42, 0});
    auto is_nil = b.emit_opt_is_nil(token, {42, 0});
    b.emit_br(is_nil, blk_reject_401, blk_check_prefix, {42, 0});

    // block_check_prefix: check "Bearer " prefix
    b.set_insert_point(fn, blk_check_prefix);
    auto bearer = b.emit_const_str(lit("Bearer "));
    auto has_pfx = b.emit_str_has_prefix(token, bearer, {43, 0});
    b.emit_br(has_pfx, blk_decode_jwt, blk_reject_401, {43, 0});

    // block_decode_jwt: decode JWT
    b.set_insert_point(fn, blk_decode_jwt);
    auto raw = b.emit_str_trim_prefix(token, bearer, {44, 0});
    auto secret = b.emit_const_str(lit("env(JWT_SECRET)"), {44, 0});

    // call.jwt_decode is an extern call.
    auto* t_str = b.make_type(TypeKind::Str);
    auto* t_opt = b.make_type(TypeKind::Optional, t_str);
    ValueId jwt_args[2] = {raw, secret};
    auto claims = b.emit_call_extern(lit("jwt_decode"), jwt_args, 2, t_opt, {45, 0});
    auto claims_nil = b.emit_opt_is_nil(claims, {45, 0});
    b.emit_br(claims_nil, blk_reject_401, blk_check_role, {45, 0});

    // block_check_role: verify role == "user"
    b.set_insert_point(fn, blk_check_role);
    auto role = b.emit_struct_field(claims, lit("role"), t_str, {46, 0});
    auto role_lit = b.emit_const_str(lit("user"), {46, 0});
    auto role_ok = b.emit_cmp(Opcode::CmpEq, role, role_lit, {46, 0});
    b.emit_br(role_ok, blk_auth_ok, blk_reject_403, {46, 0});

    // block_auth_ok: set headers, then rate limit
    b.set_insert_point(fn, blk_auth_ok);
    auto sub = b.emit_struct_field(claims, lit("sub"), t_str);
    b.emit_req_set_header(lit("X-User-ID"), sub);
    auto remote_addr = b.emit_req_remote_addr();
    auto count = b.emit_counter_incr(remote_addr, 60);  // 1m window
    auto limit = b.emit_const_i32(100);
    auto over = b.emit_cmp(Opcode::CmpGt, count, limit);
    b.emit_br(over, blk_reject_429, blk_proxy);

    // block_proxy: set X-User-ID from param, proxy
    b.set_insert_point(fn, blk_proxy);
    auto id = b.emit_req_param(lit("id"));
    b.emit_req_set_header(lit("X-User-ID"), id);
    // For simplicity, use ret.proxy with a placeholder upstream.
    auto upstream = b.emit_const_str(lit("users"));
    b.emit_ret_proxy(upstream);

    // Rejection blocks.
    b.set_insert_point(fn, blk_reject_401);
    b.emit_ret_status(401);

    b.set_insert_point(fn, blk_reject_403);
    b.emit_ret_status(403);

    b.set_insert_point(fn, blk_reject_429);
    b.emit_ret_status(429);

    // ── Verify structure ──
    CHECK_EQ(fn->block_count, 9u);
    CHECK_EQ(fn->yield_count, 0u);  // all sync in this example

    // Verify entry block structure.
    CHECK_EQ(fn->entry()->inst_count, 3u);  // req.header, opt.is_nil, br
    auto* term = fn->entry()->terminator();
    CHECK(term->is_terminator());
    CHECK_EQ(static_cast<u8>(term->op), static_cast<u8>(Opcode::Br));

    // Verify reject blocks have exactly 1 instruction (ret.status).
    CHECK_EQ(fn->blocks[blk_reject_401.id].inst_count, 1u);
    CHECK_EQ(fn->blocks[blk_reject_403.id].inst_count, 1u);
    CHECK_EQ(fn->blocks[blk_reject_429.id].inst_count, 1u);

    // ── Print the IR ──
    char print_buf[4096];
    PrintBuf pb;
    pb.init(print_buf, sizeof(print_buf), -1);  // in-memory only, no stdout noise
    print_function(pb, *fn);

    ctx.destroy();
}

// ── Printer Tests ───────────────────────────────────────────────────

TEST(RirPrinter, OpcodeNames) {
    char buf_data[256];
    PrintBuf buf;
    buf.init(buf_data, sizeof(buf_data), -1);  // fd=-1, won't flush to real fd

    print_opcode(buf, Opcode::ReqHeader);
    // Verify buffer contains "req.header".
    CHECK_EQ(buf.len, 10u);
    Str got0 = {buf.data, buf.len};
    CHECK(got0.eq(lit("req.header")));

    buf.len = 0;
    print_opcode(buf, Opcode::CmpEq);
    Str got1 = {buf.data, buf.len};
    CHECK(got1.eq(lit("cmp.eq")));

    buf.len = 0;
    print_opcode(buf, Opcode::YieldHttpGet);
    Str got2 = {buf.data, buf.len};
    CHECK(got2.eq(lit("yield.http_get")));
}

TEST(RirPrinter, TypeNames) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    char buf_data[256];
    PrintBuf buf;
    buf.init(buf_data, sizeof(buf_data), -1);

    auto* t_str = b.make_type(TypeKind::Str);
    print_type(buf, t_str);
    Str got_str = {buf.data, buf.len};
    CHECK(got_str.eq(lit("str")));

    buf.len = 0;
    auto* t_opt = b.make_type(TypeKind::Optional, t_str);
    print_type(buf, t_opt);
    Str got_opt = {buf.data, buf.len};
    CHECK(got_opt.eq(lit("Optional(str)")));

    ctx.destroy();
}

TEST(RirBuilder, InstructionIsTerminator) {
    // Verify terminator classification.
    Instruction inst{};

    inst.op = Opcode::ConstI32;
    CHECK(!inst.is_terminator());
    CHECK(!inst.is_yield());

    inst.op = Opcode::Br;
    CHECK(inst.is_terminator());
    CHECK(!inst.is_yield());

    inst.op = Opcode::YieldHttpGet;
    CHECK(inst.is_terminator());
    CHECK(inst.is_yield());
}

TEST(RirBuilder, ValueIdSentinel) {
    CHECK_EQ(kNoValue.id, 0xFFFFFFFFu);
    ValueId v{0};
    CHECK(v != kNoValue);
    ValueId v2{0};
    CHECK(v == v2);
}

// ── Optional operations test ────────────────────────────────────────

TEST(RirBuilder, OptionalOps) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto hdr = b.emit_req_header(lit("X-Token"));
    auto is_nil = b.emit_opt_is_nil(hdr);

    CHECK_EQ(static_cast<u8>(fn->values[is_nil.id].type->kind), static_cast<u8>(TypeKind::Bool));

    auto* t_str = b.make_type(TypeKind::Str);
    auto unwrapped = b.emit_opt_unwrap(hdr, t_str);
    CHECK_EQ(static_cast<u8>(fn->values[unwrapped.id].type->kind), static_cast<u8>(TypeKind::Str));

    ctx.destroy();
}

// ── Domain operations ───────────────────────────────────────────────

TEST(RirBuilder, DomainOps) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto now = b.emit_time_now();
    CHECK_EQ(static_cast<u8>(fn->values[now.id].type->kind), static_cast<u8>(TypeKind::Time));

    auto prev = b.emit_time_now();
    auto diff = b.emit_time_diff(now, prev);
    CHECK_EQ(static_cast<u8>(fn->values[diff.id].type->kind), static_cast<u8>(TypeKind::Duration));

    auto ip = b.emit_req_remote_addr();
    auto in_cidr = b.emit_ip_in_cidr(ip, lit("10.0.0.0/8"));
    CHECK_EQ(static_cast<u8>(fn->values[in_cidr.id].type->kind), static_cast<u8>(TypeKind::Bool));

    ctx.destroy();
}

// ── Multiple functions in a module ──────────────────────────────────

int main(int argc, char** argv) {
    return rout::test::run_all(argc, argv);
}

// ── P1: Capacity growth under overflow ──────────────────────────────
// Verifies that emitting more than the initial capacity (32 insts, 256
// values) grows the backing storage instead of producing phantom SSA defs.

TEST(RirBuilder, InstructionOverflow) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("big_fn"), lit("/big"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    // Emit 40 instructions (exceeds initial kInitInsts = 32).
    for (u32 i = 0; i < 40; i++) {
        auto vid = b.emit_const_i32(static_cast<i32>(i));
        // Every emitted value must have a valid ID (not kNoValue).
        CHECK(vid != kNoValue);
    }

    // value_count and inst_count must agree: each const produces one
    // value and one instruction.
    CHECK_EQ(fn->value_count, 40u);
    CHECK_EQ(fn->entry()->inst_count, 40u);

    // Verify the last value's defining instruction is reachable.
    ValueId last = {fn->value_count - 1};
    CHECK_EQ(fn->entry()->insts[39].result, last);
    CHECK_EQ(fn->entry()->insts[39].imm.i32_val, 39);

    ctx.destroy();
}

TEST(RirBuilder, ValueOverflow) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("huge_fn"), lit("/huge"), 1);

    // We need to emit 260+ values across multiple blocks to exceed
    // the initial 256 value cap.
    // Use 9 blocks of 32 insts each = 288 values.
    // Labels must have stable storage (not stack temporaries).
    const char* labels[] = {
        "blk_0", "blk_1", "blk_2", "blk_3", "blk_4", "blk_5", "blk_6", "blk_7", "blk_8"};
    for (u32 blk_idx = 0; blk_idx < 9; blk_idx++) {
        auto bid = b.create_block(fn, lit(labels[blk_idx]));
        b.set_insert_point(fn, bid);

        for (u32 i = 0; i < 32; i++) {
            auto vid = b.emit_const_i32(static_cast<i32>(blk_idx * 32 + i));
            CHECK(vid != kNoValue);
        }
    }

    // All 288 values must be present.
    CHECK_EQ(fn->value_count, 288u);

    // Spot-check: value 257 (past initial 256 cap) is valid.
    CHECK(fn->values[257].type != nullptr);

    ctx.destroy();
}

// ── P2: Headerless yield.http_get ───────────────────────────────────
// Passing kNoValue for headers must produce operand_count == 0, not a
// bogus %4294967295 reference.

TEST(RirBuilder, YieldHttpGetNoHeaders) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto vid = b.emit_yield_http_get(lit("http://auth/verify"), kNoValue);
    CHECK(vid != kNoValue);
    CHECK_EQ(fn->yield_count, 1u);

    // The instruction must have 0 operands, not 1 with kNoValue.
    auto& inst = fn->entry()->insts[0];
    CHECK_EQ(static_cast<u8>(inst.op), static_cast<u8>(Opcode::YieldHttpGet));
    CHECK_EQ(inst.operand_count, 0u);

    // Verify printer doesn't emit a bogus operand.
    char print_data[512];
    PrintBuf pb;
    pb.init(print_data, sizeof(print_data), -1);
    print_instruction(pb, inst, *fn);

    // The output should NOT contain "%4294967295".
    Str output = {pb.data, pb.len};
    bool found_bogus = false;
    Str bogus = lit("%4294967295");
    for (u32 i = 0; i + bogus.len <= output.len; i++) {
        if (output.slice(i, i + bogus.len).eq(bogus)) {
            found_bogus = true;
            break;
        }
    }
    CHECK(!found_bogus);

    ctx.destroy();
}

TEST(RirBuilder, YieldHttpGetWithHeaders) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn = b.create_function(lit("test_fn"), lit("/test"), 1);
    auto entry = b.create_block(fn, lit("entry"));
    b.set_insert_point(fn, entry);

    auto headers = b.emit_const_str(lit("Bearer xyz"));
    auto vid = b.emit_yield_http_get(lit("http://auth/verify"), headers);
    CHECK(vid != kNoValue);

    // With a real headers value, operand_count should be 1.
    auto& inst = fn->entry()->insts[1];
    CHECK_EQ(inst.operand_count, 1u);
    CHECK_EQ(inst.operands[0].id, headers.id);

    ctx.destroy();
}

TEST(RirModule, MultipleFunctions) {
    TestContext ctx;
    REQUIRE(ctx.init());

    Builder b;
    b.init(&ctx.mod);

    auto* fn1 = b.create_function(lit("handle_get_users"), lit("/users"), 1);
    auto* fn2 = b.create_function(lit("handle_post_orders"), lit("/orders"), 2);

    CHECK_EQ(ctx.mod.func_count, 2u);
    CHECK(fn1->name.eq(lit("handle_get_users")));
    CHECK(fn2->name.eq(lit("handle_post_orders")));

    // Each function has independent blocks/values.
    auto e1 = b.create_block(fn1, lit("entry"));
    auto e2 = b.create_block(fn2, lit("entry"));
    CHECK_EQ(e1.id, 0u);
    CHECK_EQ(e2.id, 0u);  // independent numbering per function

    ctx.destroy();
}
