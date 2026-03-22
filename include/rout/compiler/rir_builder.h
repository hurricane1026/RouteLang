#pragma once

#include "rout/compiler/rir.h"

namespace rout {
namespace rir {

// ── RIR Builder ─────────────────────────────────────────────────────
// Stateful builder for constructing RIR functions. All memory is
// allocated from the module's arena — no malloc, no stdlib.
//
// All fallible operations return Expected<T, RirError>. Use the TRY()
// macro for ergonomic error propagation:
//
//   Str name = {.ptr = "handler", .len = 7};
//   auto* fn = TRY(b.create_function(name, route, method));
//   auto entry = TRY(b.create_block(fn, label));
//   b.set_insert_point(fn, entry);
//   auto v0 = TRY(b.emit_const_str(prefix));

// Convenience aliases.
template <typename T>
using Result = core::Expected<T, RirError>;
using VoidResult = core::Expected<void, RirError>;

inline auto err(RirError e) {
    return core::make_unexpected(e);
}

struct Builder {
    Module* mod;

    // Current insert point — stored as index to survive grow_blocks().
    Function* cur_func;
    BlockId cur_block_id;
    Block* cur_block;

    // Interned primitive types — allocated once, reused across all emissions.
    static constexpr u32 kTypeKindCount = static_cast<u32>(TypeKind::Array) + 1;
    const Type* type_cache[kTypeKindCount];

    void init(Module* m) {
        mod = m;
        cur_func = nullptr;
        cur_block_id = kNoBlock;
        cur_block = nullptr;
        for (u32 i = 0; i < kTypeKindCount; i++) type_cache[i] = nullptr;
    }

    // ── Module-level ────────────────────────────────────────────────

    Result<StructDef*> create_struct(Str name, const FieldDef* fields, u32 count) {
        // Check capacity before allocating to avoid wasting arena space.
        if (!mod->struct_defs || mod->struct_count >= mod->struct_cap) {
            return err(RirError::CapacityFull);
        }
        auto* arena = mod->arena;
        u64 size = sizeof(StructDef) + sizeof(FieldDef) * count;
        auto* sd = static_cast<StructDef*>(arena->alloc(size));
        if (!sd) return err(RirError::OutOfMemory);
        sd->name = name;
        sd->field_count = count;
        for (u32 i = 0; i < count; i++) {
            sd->fields()[i] = fields[i];
        }
        mod->struct_defs[mod->struct_count++] = sd;
        return sd;
    }

    Result<const Type*> make_type(TypeKind kind,
                                  const Type* inner = nullptr,
                                  StructDef* sd = nullptr) {
        // Validate composite type metadata.
        if ((kind == TypeKind::Optional || kind == TypeKind::Array) && !inner) {
            return err(RirError::InvalidState);
        }
        if (kind == TypeKind::Struct && !sd) return err(RirError::InvalidState);
        // Return cached primitive type if available (no inner/struct_def).
        auto idx = static_cast<u32>(kind);
        if (!inner && !sd && idx < kTypeKindCount && type_cache[idx]) {
            return type_cache[idx];
        }
        auto* t = mod->arena->alloc_t<Type>();
        if (!t) return err(RirError::OutOfMemory);
        t->kind = kind;
        t->inner = inner;
        t->struct_def = sd;
        // Cache primitive types for reuse.
        if (!inner && !sd && idx < kTypeKindCount) {
            type_cache[idx] = t;
        }
        return static_cast<const Type*>(t);
    }

    // ── Function / Block ────────────────────────────────────────────

    Result<Function*> create_function(Str name, Str route_pattern, u8 http_method) {
        auto* arena = mod->arena;
        if (mod->func_count >= mod->func_cap) return err(RirError::CapacityFull);

        static constexpr u32 kInitBlocks = 32;
        static constexpr u32 kInitValues = 256;
        auto* blocks = arena->alloc_array<Block>(kInitBlocks);
        auto* values = arena->alloc_array<Value>(kInitValues);
        if (!blocks || !values) return err(RirError::OutOfMemory);

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

    Result<BlockId> create_block(Function* fn, Str label) {
        if (fn->block_count >= fn->block_cap) {
            TRY_VOID(grow_blocks(fn));
        }

        auto* arena = mod->arena;
        static constexpr u32 kInitInsts = 32;
        auto* insts = arena->alloc_array<Instruction>(kInitInsts);
        if (!insts) return err(RirError::OutOfMemory);

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
            cur_block_id = kNoBlock;
            cur_block = nullptr;
            return;
        }
        cur_func = fn;
        cur_block_id = block;
        cur_block = &fn->blocks[block.id];
    }

    // ── Capacity growth ───────────────────────────────────────────

    // Upper bound for doubling: cap * 2 stays within u32, with extra headroom.
    static constexpr u32 kMaxCap = 0x3FFFFFFFu;

    VoidResult grow_values(Function* fn) {
        if (fn->value_cap > kMaxCap) return err(RirError::CapacityFull);
        u32 new_cap = fn->value_cap * 2;
        auto* new_vals = mod->arena->alloc_array<Value>(new_cap);
        if (!new_vals) return err(RirError::OutOfMemory);
        for (u32 i = 0; i < fn->value_count; i++) {
            new_vals[i] = fn->values[i];
        }
        fn->values = new_vals;
        fn->value_cap = new_cap;
        return {};
    }

    VoidResult grow_insts(Block* blk) {
        if (blk->inst_cap > kMaxCap) return err(RirError::CapacityFull);
        u32 new_cap = blk->inst_cap * 2;
        auto* new_insts = mod->arena->alloc_array<Instruction>(new_cap);
        if (!new_insts) return err(RirError::OutOfMemory);
        for (u32 i = 0; i < blk->inst_count; i++) {
            new_insts[i] = blk->insts[i];
        }
        blk->insts = new_insts;
        blk->inst_cap = new_cap;
        return {};
    }

    VoidResult grow_blocks(Function* fn) {
        if (fn->block_cap > kMaxCap) return err(RirError::CapacityFull);
        u32 new_cap = fn->block_cap * 2;
        auto* new_blocks = mod->arena->alloc_array<Block>(new_cap);
        if (!new_blocks) return err(RirError::OutOfMemory);
        for (u32 i = 0; i < fn->block_count; i++) {
            new_blocks[i] = fn->blocks[i];
        }
        fn->blocks = new_blocks;
        fn->block_cap = new_cap;
        if (cur_func == fn && cur_block_id.id < fn->block_count) {
            cur_block = &fn->blocks[cur_block_id.id];
        }
        return {};
    }

    // Check that a ValueId is a valid SSA reference in the current function.
    // Note: cannot detect cross-function misuse (ValueId is a bare index by
    // design — see rir.h comment). Callers must not mix IDs across functions.
    bool valid_val(ValueId v) const {
        return v != kNoValue && cur_func && v.id < cur_func->value_count;
    }

    // Check that a value has a specific type kind (for operand type enforcement).
    bool val_has_type(ValueId v, TypeKind kind) const {
        if (!valid_val(v)) return false;
        auto* ty = cur_func->values[v.id].type;
        return ty && ty->kind == kind;
    }

    // ── Instruction emission ────────────────────────────────────────

    struct EmitResult {
        Instruction* inst;
        ValueId vid;
    };

    Result<EmitResult> emit(Opcode op, const Type* result_type, SourceLoc loc) {
        if (!cur_block || !cur_func) return err(RirError::InvalidState);

        // Refuse to emit into a block that already has a terminator.
        if (cur_block->inst_count > 0) {
            auto& last = cur_block->insts[cur_block->inst_count - 1];
            if (last.is_terminator()) return err(RirError::InvalidState);
        }

        if (cur_block->inst_count >= cur_block->inst_cap) {
            TRY_VOID(grow_insts(cur_block));
        }

        ValueId vid = kNoValue;
        if (result_type) {
            if (cur_func->value_count >= cur_func->value_cap) {
                TRY_VOID(grow_values(cur_func));
            }
            vid = {cur_func->value_count};
            auto* v = &cur_func->values[cur_func->value_count++];
            v->type = result_type;
            v->def_block = cur_block_id;
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
        return EmitResult{inst, vid};
    }

    // Roll back a previously committed emit() — used when a post-emit
    // step (e.g., set_operands) fails, to keep the IR consistent.
    void rollback_emit(const EmitResult& r) {
        if (cur_block && cur_block->inst_count > 0) cur_block->inst_count--;
        if (r.vid != kNoValue && cur_func && cur_func->value_count > 0) cur_func->value_count--;
    }

    // ── Helpers for variadic operand storage ────────────────────────

    VoidResult set_operands(Instruction* inst, const ValueId* ops, u32 count) {
        if (count > 0 && !ops) return err(RirError::InvalidState);
        for (u32 i = 0; i < count; i++) {
            if (!valid_val(ops[i])) return err(RirError::InvalidState);
        }
        if (count <= kMaxInlineOperands) {
            for (u32 i = 0; i < count; i++) inst->operands[i] = ops[i];
            inst->operand_count = count;
            return {};
        }
        u32 extra = count - kMaxInlineOperands;
        auto* extra_ops = mod->arena->alloc_array<ValueId>(extra);
        if (!extra_ops) return err(RirError::OutOfMemory);
        for (u32 i = 0; i < kMaxInlineOperands; i++) {
            inst->operands[i] = ops[i];
        }
        for (u32 i = 0; i < extra; i++) {
            extra_ops[i] = ops[kMaxInlineOperands + i];
        }
        inst->extra_operands = extra_ops;
        inst->operand_count = count;
        return {};
    }

    // ── Constants ───────────────────────────────────────────────────

    Result<ValueId> emit_const_str(Str val, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Str));
        auto [inst, vid] = TRY(emit(Opcode::ConstStr, ty, loc));
        inst->imm.str_val = val;
        return vid;
    }

    Result<ValueId> emit_const_i32(i32 val, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::I32));
        auto [inst, vid] = TRY(emit(Opcode::ConstI32, ty, loc));
        inst->imm.i32_val = val;
        return vid;
    }

    Result<ValueId> emit_const_i64(i64 val, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::I64));
        auto [inst, vid] = TRY(emit(Opcode::ConstI64, ty, loc));
        inst->imm.i64_val = val;
        return vid;
    }

    Result<ValueId> emit_const_bool(bool val, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Bool));
        auto [inst, vid] = TRY(emit(Opcode::ConstBool, ty, loc));
        inst->imm.bool_val = val;
        return vid;
    }

    Result<ValueId> emit_const_duration(i64 seconds, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Duration));
        auto [inst, vid] = TRY(emit(Opcode::ConstDuration, ty, loc));
        inst->imm.i64_val = seconds;
        return vid;
    }

    Result<ValueId> emit_const_bytesize(i64 bytes, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::ByteSize));
        auto [inst, vid] = TRY(emit(Opcode::ConstByteSize, ty, loc));
        inst->imm.i64_val = bytes;
        return vid;
    }

    Result<ValueId> emit_const_method(u8 method, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Method));
        auto [inst, vid] = TRY(emit(Opcode::ConstMethod, ty, loc));
        inst->imm.method_val = method;
        return vid;
    }

    Result<ValueId> emit_const_status(i32 code, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::StatusCode));
        auto [inst, vid] = TRY(emit(Opcode::ConstStatus, ty, loc));
        inst->imm.i32_val = code;
        return vid;
    }

    // ── Request access ──────────────────────────────────────────────

    Result<ValueId> emit_req_header(Str name, SourceLoc loc = {}) {
        auto* inner = TRY(make_type(TypeKind::Str));
        auto* ty = TRY(make_type(TypeKind::Optional, inner));
        auto [inst, vid] = TRY(emit(Opcode::ReqHeader, ty, loc));
        inst->imm.str_val = name;
        return vid;
    }

    Result<ValueId> emit_req_param(Str name, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Str));
        auto [inst, vid] = TRY(emit(Opcode::ReqParam, ty, loc));
        inst->imm.str_val = name;
        return vid;
    }

    Result<ValueId> emit_req_method(SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Method));
        return TRY(emit(Opcode::ReqMethod, ty, loc)).vid;
    }

    Result<ValueId> emit_req_path(SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Str));
        return TRY(emit(Opcode::ReqPath, ty, loc)).vid;
    }

    Result<ValueId> emit_req_remote_addr(SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::IP));
        return TRY(emit(Opcode::ReqRemoteAddr, ty, loc)).vid;
    }

    Result<ValueId> emit_req_content_length(SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::ByteSize));
        return TRY(emit(Opcode::ReqContentLength, ty, loc)).vid;
    }

    Result<ValueId> emit_req_cookie(Str name, SourceLoc loc = {}) {
        auto* inner = TRY(make_type(TypeKind::Str));
        auto* ty = TRY(make_type(TypeKind::Optional, inner));
        auto [inst, vid] = TRY(emit(Opcode::ReqCookie, ty, loc));
        inst->imm.str_val = name;
        return vid;
    }

    // ── Request mutation ────────────────────────────────────────────

    VoidResult emit_req_set_header(Str name, ValueId val, SourceLoc loc = {}) {
        if (!valid_val(val)) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::ReqSetHeader, nullptr, loc));
        r.inst->imm.str_val = name;
        r.inst->operands[0] = val;
        r.inst->operand_count = 1;
        return {};
    }

    VoidResult emit_req_set_path(ValueId path, SourceLoc loc = {}) {
        if (!valid_val(path)) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::ReqSetPath, nullptr, loc));
        r.inst->operands[0] = path;
        r.inst->operand_count = 1;
        return {};
    }

    // ── String operations ───────────────────────────────────────────

    Result<ValueId> emit_str_has_prefix(ValueId str, ValueId prefix, SourceLoc loc = {}) {
        if (!val_has_type(str, TypeKind::Str) || !val_has_type(prefix, TypeKind::Str))
            return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Bool));
        auto [inst, vid] = TRY(emit(Opcode::StrHasPrefix, ty, loc));
        inst->operands[0] = str;
        inst->operands[1] = prefix;
        inst->operand_count = 2;
        return vid;
    }

    Result<ValueId> emit_str_trim_prefix(ValueId str, ValueId prefix, SourceLoc loc = {}) {
        if (!val_has_type(str, TypeKind::Str) || !val_has_type(prefix, TypeKind::Str))
            return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Str));
        auto [inst, vid] = TRY(emit(Opcode::StrTrimPrefix, ty, loc));
        inst->operands[0] = str;
        inst->operands[1] = prefix;
        inst->operand_count = 2;
        return vid;
    }

    Result<ValueId> emit_str_interpolate(const ValueId* parts, u32 count, SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Str));
        auto r = TRY(emit(Opcode::StrInterpolate, ty, loc));
        auto ops = set_operands(r.inst, parts, count);
        if (!ops) {
            rollback_emit(r);
            return err(ops.error());
        }
        return r.vid;
    }

    // ── Comparisons ─────────────────────────────────────────────────

    static bool is_cmp_opcode(Opcode op) {
        switch (op) {
            case Opcode::CmpEq:
            case Opcode::CmpNe:
            case Opcode::CmpLt:
            case Opcode::CmpGt:
            case Opcode::CmpLe:
            case Opcode::CmpGe:
                return true;
            default:
                return false;
        }
    }

    Result<ValueId> emit_cmp(Opcode cmp_op, ValueId lhs, ValueId rhs, SourceLoc loc = {}) {
        if (!is_cmp_opcode(cmp_op) || !valid_val(lhs) || !valid_val(rhs))
            return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Bool));
        auto [inst, vid] = TRY(emit(cmp_op, ty, loc));
        inst->operands[0] = lhs;
        inst->operands[1] = rhs;
        inst->operand_count = 2;
        return vid;
    }

    // ── Domain operations ───────────────────────────────────────────

    Result<ValueId> emit_time_now(SourceLoc loc = {}) {
        auto* ty = TRY(make_type(TypeKind::Time));
        return TRY(emit(Opcode::TimeNow, ty, loc)).vid;
    }

    Result<ValueId> emit_time_diff(ValueId a, ValueId b, SourceLoc loc = {}) {
        if (!valid_val(a) || !valid_val(b)) return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Duration));
        auto [inst, vid] = TRY(emit(Opcode::TimeDiff, ty, loc));
        inst->operands[0] = a;
        inst->operands[1] = b;
        inst->operand_count = 2;
        return vid;
    }

    Result<ValueId> emit_ip_in_cidr(ValueId ip, Str cidr_lit, SourceLoc loc = {}) {
        if (!valid_val(ip)) return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Bool));
        auto [inst, vid] = TRY(emit(Opcode::IpInCidr, ty, loc));
        inst->operands[0] = ip;
        inst->operand_count = 1;
        inst->imm.str_val = cidr_lit;
        return vid;
    }

    // ── Optional operations ─────────────────────────────────────────

    Result<ValueId> emit_opt_is_nil(ValueId opt, SourceLoc loc = {}) {
        if (!valid_val(opt)) return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Bool));
        auto [inst, vid] = TRY(emit(Opcode::OptIsNil, ty, loc));
        inst->operands[0] = opt;
        inst->operand_count = 1;
        return vid;
    }

    Result<ValueId> emit_opt_unwrap(ValueId opt, const Type* inner_type, SourceLoc loc = {}) {
        if (!valid_val(opt) || !inner_type) return err(RirError::InvalidState);
        // Verify operand is Optional and inner_type matches the payload.
        auto* opt_ty = cur_func->values[opt.id].type;
        if (!opt_ty || opt_ty->kind != TypeKind::Optional || opt_ty->inner != inner_type)
            return err(RirError::InvalidState);
        auto [inst, vid] = TRY(emit(Opcode::OptUnwrap, inner_type, loc));
        inst->operands[0] = opt;
        inst->operand_count = 1;
        return vid;
    }

    // ── Struct operations ───────────────────────────────────────────

    Result<ValueId> emit_struct_field(ValueId s,
                                      Str field_name,
                                      const Type* field_type,
                                      SourceLoc loc = {}) {
        if (!val_has_type(s, TypeKind::Struct) || !field_type) return err(RirError::InvalidState);
        auto [inst, vid] = TRY(emit(Opcode::StructField, field_type, loc));
        inst->operands[0] = s;
        inst->operand_count = 1;
        inst->imm.struct_ref.name = field_name;
        inst->imm.struct_ref.type = field_type;
        return vid;
    }

    Result<ValueId> emit_body_parse(const Type* target_type, SourceLoc loc = {}) {
        if (!target_type) return err(RirError::InvalidState);
        auto [inst, vid] = TRY(emit(Opcode::BodyParse, target_type, loc));
        inst->imm.struct_ref.type = target_type;
        return vid;
    }

    // ── Counter ─────────────────────────────────────────────────────

    Result<ValueId> emit_counter_incr(ValueId key, i64 window_seconds, SourceLoc loc = {}) {
        if (!valid_val(key)) return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::I32));
        auto [inst, vid] = TRY(emit(Opcode::CounterIncr, ty, loc));
        inst->operands[0] = key;
        inst->operand_count = 1;
        inst->imm.i64_val = window_seconds;
        return vid;
    }

    // ── External calls ──────────────────────────────────────────────

    Result<ValueId> emit_call_extern(Str func_name,
                                     const ValueId* args,
                                     u32 arg_count,
                                     const Type* return_type,
                                     SourceLoc loc = {}) {
        if (!return_type) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::CallExtern, return_type, loc));
        r.inst->imm.extern_name = func_name;
        auto ops = set_operands(r.inst, args, arg_count);
        if (!ops) {
            rollback_emit(r);
            return err(ops.error());
        }
        return r.vid;
    }

    // ── Terminators ─────────────────────────────────────────────────

    VoidResult emit_br(ValueId cond, BlockId then_blk, BlockId else_blk, SourceLoc loc = {}) {
        if (!cur_func || !val_has_type(cond, TypeKind::Bool)) return err(RirError::InvalidState);
        if (then_blk.id >= cur_func->block_count || else_blk.id >= cur_func->block_count) {
            return err(RirError::InvalidState);
        }
        auto r = TRY(emit(Opcode::Br, nullptr, loc));
        r.inst->operands[0] = cond;
        r.inst->operand_count = 1;
        r.inst->imm.block_targets[0] = then_blk;
        r.inst->imm.block_targets[1] = else_blk;
        return {};
    }

    VoidResult emit_jmp(BlockId target, SourceLoc loc = {}) {
        if (!cur_func) return err(RirError::InvalidState);
        if (target.id >= cur_func->block_count) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::Jmp, nullptr, loc));
        r.inst->imm.block_targets[0] = target;
        return {};
    }

    VoidResult emit_ret_status(i32 code, SourceLoc loc = {}) {
        auto r = TRY(emit(Opcode::RetStatus, nullptr, loc));
        r.inst->imm.i32_val = code;
        return {};
    }

    VoidResult emit_ret_proxy(ValueId upstream, SourceLoc loc = {}) {
        if (!valid_val(upstream)) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::RetProxy, nullptr, loc));
        r.inst->operands[0] = upstream;
        r.inst->operand_count = 1;
        return {};
    }

    // ── Yields (I/O suspend points) ────────────────────────────────

    Result<ValueId> emit_yield_http_get(Str url, ValueId headers, SourceLoc loc = {}) {
        // Validate headers before emit() to avoid phantom instructions.
        if (headers != kNoValue && !valid_val(headers)) return err(RirError::InvalidState);
        auto* ty = TRY(make_type(TypeKind::Str));
        auto [inst, vid] = TRY(emit(Opcode::YieldHttpGet, ty, loc));
        inst->imm.str_val = url;
        if (headers != kNoValue) {
            inst->operands[0] = headers;
            inst->operand_count = 1;
        }
        cur_func->yield_count++;
        return vid;
    }

    Result<ValueId> emit_yield_extern(
        Str name, const ValueId* args, u32 arg_count, const Type* return_type, SourceLoc loc = {}) {
        if (!return_type) return err(RirError::InvalidState);
        auto r = TRY(emit(Opcode::YieldExtern, return_type, loc));
        r.inst->imm.extern_name = name;
        auto ops = set_operands(r.inst, args, arg_count);
        if (!ops) {
            rollback_emit(r);
            return err(ops.error());
        }
        cur_func->yield_count++;
        return r.vid;
    }
};

}  // namespace rir
}  // namespace rout
