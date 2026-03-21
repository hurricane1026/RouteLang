#pragma once

#include "rout/compiler/rir.h"

namespace rout {
namespace rir {

// ── RIR Builder ─────────────────────────────────────────────────────
// Stateful builder for constructing RIR functions. All memory is
// allocated from the module's arena — no malloc, no stdlib.
//
// Usage:
//   Builder b;
//   b.init(&module);
//   auto* fn = b.create_function(name, pattern, method);
//   auto entry = b.create_block(fn, "entry");
//   b.set_insert_point(fn, entry);
//   auto v0 = b.emit_const_str("Bearer ");
//   auto v1 = b.emit_req_header("Authorization", loc);
//   ...
//   b.emit_br(cond, then_block, else_block, loc);

// Sentinel for invalid block IDs (parallel to kNoValue for values).
static constexpr BlockId kNoBlock = {0xFFFFFFFF};

struct Builder {
    Module* mod;

    // Current insert point.
    Function* cur_func;
    Block* cur_block;

    void init(Module* m) {
        mod = m;
        cur_func = nullptr;
        cur_block = nullptr;
    }

    // ── Module-level ────────────────────────────────────────────────

    // Create and register a struct type definition in the module.
    StructDef* create_struct(Str name, const FieldDef* fields, u32 count) {
        auto* arena = mod->arena;
        u64 size = sizeof(StructDef) + sizeof(FieldDef) * count;
        auto* sd = static_cast<StructDef*>(arena->alloc(size));
        if (!sd) return nullptr;
        sd->name = name;
        sd->field_count = count;
        for (u32 i = 0; i < count; i++) {
            sd->fields()[i] = fields[i];
        }
        // Register in module if capacity allows.
        if (mod->struct_defs && mod->struct_count < 64) {
            mod->struct_defs[mod->struct_count++] = sd;
        }
        return sd;
    }

    // Intern a type in the arena. Returns nullptr on OOM.
    const Type* make_type(TypeKind kind, const Type* inner = nullptr, StructDef* sd = nullptr) {
        auto* t = mod->arena->alloc_t<Type>();
        if (!t) return nullptr;
        t->kind = kind;
        t->inner = inner;
        t->struct_def = sd;
        return t;
    }

    // ── Function / Block ────────────────────────────────────────────

    Function* create_function(Str name, Str route_pattern, u8 http_method) {
        auto* arena = mod->arena;
        if (mod->func_count >= mod->func_cap) return nullptr;

        static constexpr u32 kInitBlocks = 32;
        static constexpr u32 kInitValues = 256;
        auto* blocks = arena->alloc_array<Block>(kInitBlocks);
        auto* values = arena->alloc_array<Value>(kInitValues);
        if (!blocks || !values) return nullptr;

        auto* fn = &mod->functions[mod->func_count++];
        fn->name = name;
        fn->route_pattern = route_pattern;
        fn->http_method = http_method;
        fn->yield_count = 0;
        fn->blocks = blocks;
        fn->block_count = 0;
        fn->block_cap = kInitBlocks;
        fn->values = values;
        fn->value_count = 0;
        fn->value_cap = kInitValues;

        return fn;
    }

    BlockId create_block(Function* fn, Str label) {
        // Grow block array if needed.
        if (fn->block_count >= fn->block_cap) {
            if (!grow_blocks(fn)) return kNoBlock;
        }

        auto* arena = mod->arena;
        static constexpr u32 kInitInsts = 32;
        auto* insts = arena->alloc_array<Instruction>(kInitInsts);
        if (!insts) return kNoBlock;

        BlockId bid = {fn->block_count};
        auto* blk = &fn->blocks[fn->block_count++];
        blk->id = bid;
        blk->label = label;
        blk->insts = insts;
        blk->inst_count = 0;
        blk->inst_cap = kInitInsts;

        return bid;
    }

    void set_insert_point(Function* fn, BlockId block) {
        if (!fn || block.id >= fn->block_count) {
            cur_func = nullptr;
            cur_block = nullptr;
            return;
        }
        cur_func = fn;
        cur_block = &fn->blocks[block.id];
    }

    // ── Capacity growth ───────────────────────────────────────────

    bool grow_values(Function* fn) {
        u32 new_cap = fn->value_cap * 2;
        auto* new_vals = mod->arena->alloc_array<Value>(new_cap);
        if (!new_vals) return false;
        for (u32 i = 0; i < fn->value_count; i++) {
            new_vals[i] = fn->values[i];
        }
        fn->values = new_vals;
        fn->value_cap = new_cap;
        return true;
    }

    bool grow_insts(Block* blk) {
        u32 new_cap = blk->inst_cap * 2;
        auto* new_insts = mod->arena->alloc_array<Instruction>(new_cap);
        if (!new_insts) return false;
        for (u32 i = 0; i < blk->inst_count; i++) {
            new_insts[i] = blk->insts[i];
        }
        blk->insts = new_insts;
        blk->inst_cap = new_cap;
        return true;
    }

    bool grow_blocks(Function* fn) {
        u32 new_cap = fn->block_cap * 2;
        auto* new_blocks = mod->arena->alloc_array<Block>(new_cap);
        if (!new_blocks) return false;
        for (u32 i = 0; i < fn->block_count; i++) {
            new_blocks[i] = fn->blocks[i];
        }
        fn->blocks = new_blocks;
        fn->block_cap = new_cap;
        return true;
    }

    // ── Instruction emission ────────────────────────────────────────
    // Atomically reserves both a value slot (if needed) and an
    // instruction slot. Refuses to emit into an already-terminated
    // block. If capacity cannot be satisfied the builder emits
    // nothing and returns nullptr + kNoValue.

    struct EmitResult {
        Instruction* inst;
        ValueId vid;
    };

    EmitResult emit(Opcode op, const Type* result_type, SourceLoc loc) {
        if (!cur_block || !cur_func) return {nullptr, kNoValue};

        // Refuse to emit into a block that already has a terminator.
        if (cur_block->inst_count > 0) {
            auto& last = cur_block->insts[cur_block->inst_count - 1];
            if (last.is_terminator()) return {nullptr, kNoValue};
        }

        // Grow instruction array if needed.
        if (cur_block->inst_count >= cur_block->inst_cap) {
            if (!grow_insts(cur_block)) return {nullptr, kNoValue};
        }

        // Grow value array if producing a value.
        ValueId vid = kNoValue;
        if (result_type) {
            if (cur_func->value_count >= cur_func->value_cap) {
                if (!grow_values(cur_func)) return {nullptr, kNoValue};
            }
            vid = {cur_func->value_count};
            auto* v = &cur_func->values[cur_func->value_count++];
            v->type = result_type;
            v->def_inst = cur_block->inst_count;
        }

        auto* inst = &cur_block->insts[cur_block->inst_count++];
        inst->op = op;
        inst->result = vid;
        inst->loc = loc;
        inst->operand_count = 0;
        inst->extra_operands = nullptr;
        for (u32 i = 0; i < sizeof(inst->imm); i++) {
            reinterpret_cast<u8*>(&inst->imm)[i] = 0;
        }
        return {inst, vid};
    }

    // ── Helpers for variadic operand storage ────────────────────────

    bool set_operands(Instruction* inst, const ValueId* ops, u32 count) {
        inst->operand_count = count;
        if (count <= kMaxInlineOperands) {
            for (u32 i = 0; i < count; i++) inst->operands[i] = ops[i];
            return true;
        }
        for (u32 i = 0; i < kMaxInlineOperands; i++) {
            inst->operands[i] = ops[i];
        }
        u32 extra = count - kMaxInlineOperands;
        inst->extra_operands = mod->arena->alloc_array<ValueId>(extra);
        if (!inst->extra_operands) return false;
        for (u32 i = 0; i < extra; i++) {
            inst->extra_operands[i] = ops[kMaxInlineOperands + i];
        }
        return true;
    }

    // ── Constants ───────────────────────────────────────────────────

    ValueId emit_const_str(Str val, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstStr, make_type(TypeKind::Str), loc);
        if (inst) inst->imm.str_val = val;
        return vid;
    }

    ValueId emit_const_i32(i32 val, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstI32, make_type(TypeKind::I32), loc);
        if (inst) inst->imm.i32_val = val;
        return vid;
    }

    ValueId emit_const_i64(i64 val, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstI64, make_type(TypeKind::I64), loc);
        if (inst) inst->imm.i64_val = val;
        return vid;
    }

    ValueId emit_const_bool(bool val, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstBool, make_type(TypeKind::Bool), loc);
        if (inst) inst->imm.bool_val = val;
        return vid;
    }

    ValueId emit_const_duration(i64 seconds, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstDuration, make_type(TypeKind::Duration), loc);
        if (inst) inst->imm.i64_val = seconds;
        return vid;
    }

    ValueId emit_const_bytesize(i64 bytes, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstByteSize, make_type(TypeKind::ByteSize), loc);
        if (inst) inst->imm.i64_val = bytes;
        return vid;
    }

    ValueId emit_const_method(u8 method, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstMethod, make_type(TypeKind::Method), loc);
        if (inst) inst->imm.method_val = method;
        return vid;
    }

    ValueId emit_const_status(i32 code, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ConstStatus, make_type(TypeKind::StatusCode), loc);
        if (inst) inst->imm.i32_val = code;
        return vid;
    }

    // ── Request access ──────────────────────────────────────────────

    ValueId emit_req_header(Str name, SourceLoc loc = {}) {
        auto* opt_str = make_type(TypeKind::Optional, make_type(TypeKind::Str));
        auto [inst, vid] = emit(Opcode::ReqHeader, opt_str, loc);
        if (inst) inst->imm.str_val = name;
        return vid;
    }

    ValueId emit_req_param(Str name, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ReqParam, make_type(TypeKind::Str), loc);
        if (inst) inst->imm.str_val = name;
        return vid;
    }

    ValueId emit_req_method(SourceLoc loc = {}) {
        return emit(Opcode::ReqMethod, make_type(TypeKind::Method), loc).vid;
    }

    ValueId emit_req_path(SourceLoc loc = {}) {
        return emit(Opcode::ReqPath, make_type(TypeKind::Str), loc).vid;
    }

    ValueId emit_req_remote_addr(SourceLoc loc = {}) {
        return emit(Opcode::ReqRemoteAddr, make_type(TypeKind::IP), loc).vid;
    }

    ValueId emit_req_content_length(SourceLoc loc = {}) {
        return emit(Opcode::ReqContentLength, make_type(TypeKind::ByteSize), loc).vid;
    }

    ValueId emit_req_cookie(Str name, SourceLoc loc = {}) {
        auto* opt_str = make_type(TypeKind::Optional, make_type(TypeKind::Str));
        auto [inst, vid] = emit(Opcode::ReqCookie, opt_str, loc);
        if (inst) inst->imm.str_val = name;
        return vid;
    }

    // ── Request mutation ────────────────────────────────────────────

    void emit_req_set_header(Str name, ValueId val, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ReqSetHeader, nullptr, loc);
        if (inst) {
            inst->imm.str_val = name;
            inst->operands[0] = val;
            inst->operand_count = 1;
        }
    }

    void emit_req_set_path(ValueId path, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::ReqSetPath, nullptr, loc);
        if (inst) {
            inst->operands[0] = path;
            inst->operand_count = 1;
        }
    }

    // ── String operations ───────────────────────────────────────────

    ValueId emit_str_has_prefix(ValueId str, ValueId prefix, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::StrHasPrefix, make_type(TypeKind::Bool), loc);
        if (inst) {
            inst->operands[0] = str;
            inst->operands[1] = prefix;
            inst->operand_count = 2;
        }
        return vid;
    }

    ValueId emit_str_trim_prefix(ValueId str, ValueId prefix, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::StrTrimPrefix, make_type(TypeKind::Str), loc);
        if (inst) {
            inst->operands[0] = str;
            inst->operands[1] = prefix;
            inst->operand_count = 2;
        }
        return vid;
    }

    ValueId emit_str_interpolate(const ValueId* parts, u32 count, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::StrInterpolate, make_type(TypeKind::Str), loc);
        if (inst) set_operands(inst, parts, count);
        return vid;
    }

    // ── Comparisons ─────────────────────────────────────────────────

    ValueId emit_cmp(Opcode cmp_op, ValueId lhs, ValueId rhs, SourceLoc loc = {}) {
        auto [inst, vid] = emit(cmp_op, make_type(TypeKind::Bool), loc);
        if (inst) {
            inst->operands[0] = lhs;
            inst->operands[1] = rhs;
            inst->operand_count = 2;
        }
        return vid;
    }

    // ── Domain operations ───────────────────────────────────────────

    ValueId emit_time_now(SourceLoc loc = {}) {
        return emit(Opcode::TimeNow, make_type(TypeKind::Time), loc).vid;
    }

    ValueId emit_time_diff(ValueId a, ValueId b, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::TimeDiff, make_type(TypeKind::Duration), loc);
        if (inst) {
            inst->operands[0] = a;
            inst->operands[1] = b;
            inst->operand_count = 2;
        }
        return vid;
    }

    ValueId emit_ip_in_cidr(ValueId ip, Str cidr_lit, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::IpInCidr, make_type(TypeKind::Bool), loc);
        if (inst) {
            inst->operands[0] = ip;
            inst->operand_count = 1;
            inst->imm.str_val = cidr_lit;
        }
        return vid;
    }

    // ── Optional operations ─────────────────────────────────────────

    ValueId emit_opt_is_nil(ValueId opt, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::OptIsNil, make_type(TypeKind::Bool), loc);
        if (inst) {
            inst->operands[0] = opt;
            inst->operand_count = 1;
        }
        return vid;
    }

    ValueId emit_opt_unwrap(ValueId opt, const Type* inner_type, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::OptUnwrap, inner_type, loc);
        if (inst) {
            inst->operands[0] = opt;
            inst->operand_count = 1;
        }
        return vid;
    }

    // ── Struct operations ───────────────────────────────────────────

    ValueId emit_struct_field(ValueId s,
                              Str field_name,
                              const Type* field_type,
                              SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::StructField, field_type, loc);
        if (inst) {
            inst->operands[0] = s;
            inst->operand_count = 1;
            inst->imm.struct_ref.name = field_name;
            inst->imm.struct_ref.type = field_type;
        }
        return vid;
    }

    ValueId emit_body_parse(const Type* target_type, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::BodyParse, target_type, loc);
        if (inst) inst->imm.struct_ref.type = target_type;
        return vid;
    }

    // ── Counter ─────────────────────────────────────────────────────

    ValueId emit_counter_incr(ValueId key, i64 window_seconds, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::CounterIncr, make_type(TypeKind::I32), loc);
        if (inst) {
            inst->operands[0] = key;
            inst->operand_count = 1;
            inst->imm.i64_val = window_seconds;
        }
        return vid;
    }

    // ── External calls ──────────────────────────────────────────────

    ValueId emit_call_extern(Str func_name,
                             const ValueId* args,
                             u32 arg_count,
                             const Type* return_type,
                             SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::CallExtern, return_type, loc);
        if (!inst) return vid;
        inst->imm.extern_name = func_name;
        set_operands(inst, args, arg_count);
        return vid;
    }

    // ── Terminators ─────────────────────────────────────────────────

    void emit_br(ValueId cond, BlockId then_blk, BlockId else_blk, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::Br, nullptr, loc);
        if (inst) {
            inst->operands[0] = cond;
            inst->operand_count = 1;
            inst->imm.block_targets[0] = then_blk;
            inst->imm.block_targets[1] = else_blk;
        }
    }

    void emit_jmp(BlockId target, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::Jmp, nullptr, loc);
        if (inst) inst->imm.block_targets[0] = target;
    }

    void emit_ret_status(i32 code, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::RetStatus, nullptr, loc);
        if (inst) inst->imm.i32_val = code;
    }

    void emit_ret_proxy(ValueId upstream, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::RetProxy, nullptr, loc);
        if (inst) {
            inst->operands[0] = upstream;
            inst->operand_count = 1;
        }
    }

    // ── Yields (I/O suspend points) ────────────────────────────────

    ValueId emit_yield_http_get(Str url, ValueId headers, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::YieldHttpGet, make_type(TypeKind::Str), loc);
        if (inst) {
            inst->imm.str_val = url;
            // Only store headers operand when actually provided.
            if (headers != kNoValue) {
                inst->operands[0] = headers;
                inst->operand_count = 1;
            }
            cur_func->yield_count++;
        }
        return vid;
    }

    ValueId emit_yield_extern(
        Str name, const ValueId* args, u32 arg_count, const Type* return_type, SourceLoc loc = {}) {
        auto [inst, vid] = emit(Opcode::YieldExtern, return_type, loc);
        if (!inst) return vid;
        inst->imm.extern_name = name;
        set_operands(inst, args, arg_count);
        cur_func->yield_count++;
        return vid;
    }
};

}  // namespace rir
}  // namespace rout
