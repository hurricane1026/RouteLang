#include "rut/compiler/mir_build.h"

namespace rut {

namespace {

static Str entry_label() {
    return {"entry", 5};
}

static Str then_label() {
    return {"then", 4};
}

static Str else_label() {
    return {"else", 4};
}

static Str cont_label() {
    return {"cont", 4};
}

static Str fail_label() {
    return {"fail", 4};
}

static Str match_test_label() {
    return {"match_test", 10};
}

static Str match_case_label() {
    return {"match_case", 10};
}

static Str match_default_label() {
    return {"match_default", 13};
}

static MirTypeKind mir_type_kind(HirTypeKind kind) {
    return kind == HirTypeKind::Bool      ? MirTypeKind::Bool
           : kind == HirTypeKind::I32     ? MirTypeKind::I32
           : kind == HirTypeKind::Str     ? MirTypeKind::Str
           : kind == HirTypeKind::Method  ? MirTypeKind::Method
           : kind == HirTypeKind::Variant ? MirTypeKind::Variant
           : kind == HirTypeKind::Tuple   ? MirTypeKind::Tuple
           : kind == HirTypeKind::Struct  ? MirTypeKind::Struct
                                          : MirTypeKind::Unknown;
}

static bool expand_hir_flat_shape(const HirModule& module,
                                  u32 shape_index,
                                  MirTypeKind* type,
                                  u32* variant_index,
                                  u32* struct_index,
                                  u32* tuple_len,
                                  MirTypeKind* tuple_types,
                                  u32* tuple_variant_indices,
                                  u32* tuple_struct_indices) {
    if (shape_index == 0xffffffffu || shape_index >= module.type_shapes.len) return false;
    const auto& shape = module.type_shapes[shape_index];
    *type = mir_type_kind(shape.type);
    *variant_index = shape.variant_index;
    *struct_index = shape.struct_index;
    *tuple_len = shape.tuple_len;
    if (shape.type != HirTypeKind::Tuple) return true;
    for (u32 i = 0; i < shape.tuple_len; i++) {
        const u32 elem_index = shape.tuple_elem_shape_indices[i];
        if (elem_index >= module.type_shapes.len) return false;
        const auto& elem = module.type_shapes[elem_index];
        if (elem.type == HirTypeKind::Tuple) return false;
        tuple_types[i] = mir_type_kind(elem.type);
        tuple_variant_indices[i] = elem.variant_index;
        tuple_struct_indices[i] = elem.struct_index;
    }
    return true;
}

static void apply_expr_shape_if_available(const HirModule& module,
                                          const HirExpr& expr,
                                          MirValue* out) {
    expand_hir_flat_shape(module,
                          expr.shape_index,
                          &out->type,
                          &out->variant_index,
                          &out->struct_index,
                          &out->tuple_len,
                          out->tuple_types,
                          out->tuple_variant_indices,
                          out->tuple_struct_indices);
}

static bool shape_carrier_ready(const MirModule& mir,
                                u32 shape_index,
                                const bool* struct_ready,
                                const bool* variant_ready) {
    if (shape_index >= mir.type_shapes.len) return false;
    const auto& shape = mir.type_shapes[shape_index];
    if (!shape.is_concrete) return false;
    if (shape.type == MirTypeKind::Bool || shape.type == MirTypeKind::I32 ||
        shape.type == MirTypeKind::Str || shape.type == MirTypeKind::Method)
        return true;
    if (shape.type == MirTypeKind::Struct)
        return shape.struct_index < mir.structs.len && struct_ready[shape.struct_index];
    if (shape.type == MirTypeKind::Variant)
        return shape.variant_index < mir.variants.len && variant_ready[shape.variant_index];
    if (shape.type != MirTypeKind::Tuple) return false;
    for (u32 i = 0; i < shape.tuple_len; i++) {
        if (!shape_carrier_ready(
                mir, shape.tuple_elem_shape_indices[i], struct_ready, variant_ready))
            return false;
    }
    return true;
}

static bool shape_slot_carrier_ready(const MirModule& mir,
                                     u32 shape_index,
                                     const bool* struct_ready,
                                     const bool* variant_ready) {
    if (shape_index >= mir.type_shapes.len) return false;
    const auto& shape = mir.type_shapes[shape_index];
    if (!shape.is_concrete) return false;
    if (shape.type == MirTypeKind::Method) return false;
    if (shape.type == MirTypeKind::Bool || shape.type == MirTypeKind::I32 ||
        shape.type == MirTypeKind::Str)
        return true;
    if (shape.type == MirTypeKind::Struct)
        return shape.struct_index < mir.structs.len && struct_ready[shape.struct_index];
    if (shape.type == MirTypeKind::Variant)
        return shape.variant_index < mir.variants.len && variant_ready[shape.variant_index];
    if (shape.type != MirTypeKind::Tuple) return false;
    for (u32 i = 0; i < shape.tuple_len; i++) {
        if (!shape_carrier_ready(
                mir, shape.tuple_elem_shape_indices[i], struct_ready, variant_ready))
            return false;
    }
    return true;
}

static bool instance_arg_concrete(const MirModule& mir, MirTypeKind type, u32 shape_index) {
    if (shape_index != 0xffffffffu) {
        if (shape_index >= mir.type_shapes.len) return false;
        return mir.type_shapes[shape_index].is_concrete;
    }
    return type != MirTypeKind::Unknown;
}

static bool instance_fully_concrete(const MirModule& mir,
                                    u32 arg_count,
                                    const MirTypeKind* arg_types,
                                    const u32* shape_indices) {
    for (u32 ai = 0; ai < arg_count; ai++) {
        if (!instance_arg_concrete(mir, arg_types[ai], shape_indices[ai])) return false;
    }
    return true;
}

static bool is_open_generic_struct_decl(const MirStruct& st) {
    return st.type_params.len != 0 && st.template_struct_index == 0xffffffffu;
}

static bool is_open_generic_variant_decl(const MirVariant& variant) {
    return variant.type_params.len != 0 && variant.template_variant_index == 0xffffffffu;
}

static bool field_carrier_ready(const MirModule& mir,
                                const MirStruct::FieldDecl& field,
                                const bool* struct_ready,
                                const bool* variant_ready) {
    if (field.is_error_type) return true;
    if (field.shape_index != 0xffffffffu)
        return shape_slot_carrier_ready(mir, field.shape_index, struct_ready, variant_ready);
    // Note: Method is intentionally omitted here (and in the variant
    // payload analog below). lower_rir's struct-field and variant-
    // payload builders don't yet have a Method carrier — a
    // Method-typed struct field / payload would lower to an Optional
    // <I32> slot and fail in emit_struct_create. Method as a plain
    // value is fine (shape_carrier_ready accepts it) because it's
    // lowered as a bare i8; these per-field helpers only run when
    // there's no shape index to delegate to, so until someone wires
    // Method into the carrier builders, reporting it as "ready"
    // here would mislead the lowering pass. Today's surface DSL
    // can't declare Method-typed struct fields anyway.
    if (field.type == MirTypeKind::Bool || field.type == MirTypeKind::I32 ||
        field.type == MirTypeKind::Str)
        return true;
    if (field.type == MirTypeKind::Struct)
        return field.struct_index < mir.structs.len && struct_ready[field.struct_index];
    if (field.type == MirTypeKind::Variant)
        return field.variant_index < mir.variants.len && variant_ready[field.variant_index];
    return false;
}

static bool variant_payload_carrier_ready(const MirModule& mir,
                                          const MirVariant::CaseDecl& c,
                                          const bool* struct_ready,
                                          const bool* variant_ready) {
    if (!c.has_payload) return true;
    if (c.payload_shape_index != 0xffffffffu)
        return shape_slot_carrier_ready(mir, c.payload_shape_index, struct_ready, variant_ready);
    // See field_carrier_ready: Method payloads have no lower_rir
    // carrier yet, so don't claim they're ready.
    if (c.payload_type == MirTypeKind::Bool || c.payload_type == MirTypeKind::I32 ||
        c.payload_type == MirTypeKind::Str)
        return true;
    if (c.payload_type == MirTypeKind::Struct)
        return c.payload_struct_index < mir.structs.len && struct_ready[c.payload_struct_index];
    if (c.payload_type == MirTypeKind::Variant)
        return c.payload_variant_index < mir.variants.len && variant_ready[c.payload_variant_index];
    return false;
}

// Context for MIR for-loop unrolling (Phase 4b). When lowering the body of
// a HirForLoop iteration, the caller passes a non-null ctx so that any
// LocalRef to the loop variable's ref_index is replaced with the current
// iteration's element MirValue. External callers (route-level guards /
// terminator / let init) pass nullptr: route-level code cannot reference
// the loop variable because analyze clears its name after the body (see
// analyze.cc:10123-10137), so substitution is never needed there.
struct ForLoopCtx {
    u32 loop_var_ref_index;    // matches HirExpr::local_index on LocalRef to loop var
    MirValue current_element;  // value to substitute at that LocalRef
};

static FrontendResult<MirValue> mir_value(const HirExpr& expr,
                                          const HirModule& module,
                                          MirFunction* fn,
                                          const ForLoopCtx* ctx = nullptr) {
    MirValue v{};
    v.shape_index = expr.shape_index;
    v.may_nil = expr.may_nil;
    v.may_error = expr.may_error;
    if (expr.kind == HirExprKind::BoolLit) {
        v.kind = MirValueKind::BoolConst;
        v.type = MirTypeKind::Bool;
        v.bool_value = expr.bool_value;
        return v;
    }
    if (expr.kind == HirExprKind::IntLit) {
        v.kind = MirValueKind::IntConst;
        v.type = MirTypeKind::I32;
        v.int_value = expr.int_value;
        return v;
    }
    if (expr.kind == HirExprKind::StrLit) {
        v.kind = MirValueKind::StrConst;
        v.type = MirTypeKind::Str;
        v.str_value = expr.str_value;
        return v;
    }
    if (expr.kind == HirExprKind::Tuple) {
        v.kind = MirValueKind::Tuple;
        v.type = MirTypeKind::Tuple;
        v.tuple_len = expr.tuple_len;
        for (u32 i = 0; i < expr.tuple_len; i++) {
            v.tuple_types[i] = mir_type_kind(expr.tuple_types[i]);
            v.tuple_variant_indices[i] = expr.tuple_variant_indices[i];
            v.tuple_struct_indices[i] = expr.tuple_struct_indices[i];
        }
        expand_hir_flat_shape(module,
                              expr.shape_index,
                              &v.type,
                              &v.variant_index,
                              &v.struct_index,
                              &v.tuple_len,
                              v.tuple_types,
                              v.tuple_variant_indices,
                              v.tuple_struct_indices);
        for (u32 i = 0; i < expr.args.len; i++) {
            auto elem = mir_value(*expr.args[i], module, fn, ctx);
            if (!elem) return core::make_unexpected(elem.error());
            if (!fn->values.push(elem.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            if (!v.args.push(&fn->values[fn->values.len - 1]))
                return frontend_error(FrontendError::TooManyItems, expr.span);
        }
        return v;
    }
    if (expr.kind == HirExprKind::StructInit) {
        v.kind = MirValueKind::StructInit;
        v.type = MirTypeKind::Struct;
        v.struct_index = expr.struct_index;
        v.str_value = expr.str_value;
        apply_expr_shape_if_available(module, expr, &v);
        for (u32 i = 0; i < expr.field_inits.len; i++) {
            auto field_value = mir_value(*expr.field_inits[i].value, module, fn, ctx);
            if (!field_value) return core::make_unexpected(field_value.error());
            if (!fn->values.push(field_value.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            MirValue::FieldInit field_init{};
            field_init.name = expr.field_inits[i].name;
            field_init.value = &fn->values[fn->values.len - 1];
            if (!v.field_inits.push(field_init))
                return frontend_error(FrontendError::TooManyItems, expr.span);
        }
        return v;
    }
    if (expr.kind == HirExprKind::TupleSlot) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::TupleSlot;
        v.type = mir_type_kind(expr.type);
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.int_value = expr.int_value;
        v.lhs = &fn->values[fn->values.len - 1];
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::VariantCase) {
        v.kind = MirValueKind::VariantCase;
        v.type = MirTypeKind::Variant;
        v.variant_index = expr.variant_index;
        v.case_index = expr.case_index;
        v.error_variant_index = expr.error_variant_index;
        v.error_case_index = expr.error_case_index;
        v.int_value = expr.int_value;
        apply_expr_shape_if_available(module, expr, &v);
        if (expr.lhs != nullptr) {
            auto payload = mir_value(*expr.lhs, module, fn, ctx);
            if (!payload) return core::make_unexpected(payload.error());
            if (!fn->values.push(payload.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            v.lhs = &fn->values[fn->values.len - 1];
        }
        return v;
    }
    if (expr.kind == HirExprKind::Field) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::Field;
        v.type = mir_type_kind(expr.type);
        v.str_value = expr.str_value;
        v.lhs = &fn->values[fn->values.len - 1];
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.tuple_len = expr.tuple_len;
        for (u32 i = 0; i < expr.tuple_len; i++) {
            v.tuple_types[i] = mir_type_kind(expr.tuple_types[i]);
            v.tuple_variant_indices[i] = expr.tuple_variant_indices[i];
            v.tuple_struct_indices[i] = expr.tuple_struct_indices[i];
        }
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        return v;
    }
    if (expr.kind == HirExprKind::ReqHeader) {
        v.kind = MirValueKind::ReqHeader;
        v.type = MirTypeKind::Str;
        v.may_nil = true;
        v.str_value = expr.str_value;
        return v;
    }
    if (expr.kind == HirExprKind::ConstMethod) {
        v.kind = MirValueKind::ConstMethod;
        v.type = MirTypeKind::Method;
        v.int_value = expr.int_value;
        return v;
    }
    if (expr.kind == HirExprKind::ReqMethod) {
        v.kind = MirValueKind::ReqMethod;
        v.type = MirTypeKind::Method;
        return v;
    }
    if (expr.kind == HirExprKind::Nil) {
        v.kind = MirValueKind::Nil;
        v.type = MirTypeKind::Unknown;
        return v;
    }
    if (expr.kind == HirExprKind::Error) {
        v.kind = MirValueKind::Error;
        v.type = MirTypeKind::Unknown;
        v.int_value = expr.int_value;
        v.msg = expr.msg;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        v.error_case_index = expr.error_case_index;
        v.str_value = expr.str_value;
        for (u32 i = 0; i < expr.field_inits.len; i++) {
            auto field_value = mir_value(*expr.field_inits[i].value, module, fn, ctx);
            if (!field_value) return core::make_unexpected(field_value.error());
            if (!fn->values.push(field_value.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            MirValue::FieldInit field_init{};
            field_init.name = expr.field_inits[i].name;
            field_init.value = &fn->values[fn->values.len - 1];
            if (!v.field_inits.push(field_init))
                return frontend_error(FrontendError::TooManyItems, expr.span);
        }
        return v;
    }
    if (expr.kind == HirExprKind::Eq || expr.kind == HirExprKind::Lt ||
        expr.kind == HirExprKind::Gt) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        auto rhs = mir_value(*expr.rhs, module, fn, ctx);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* lhs_ptr = &fn->values[fn->values.len - 1];
        if (!fn->values.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* rhs_ptr = &fn->values[fn->values.len - 1];
        v.kind = expr.kind == HirExprKind::Eq
                     ? MirValueKind::Eq
                     : (expr.kind == HirExprKind::Lt ? MirValueKind::Lt : MirValueKind::Gt);
        v.type = MirTypeKind::Bool;
        v.lhs = lhs_ptr;
        v.rhs = rhs_ptr;
        v.error_variant_index = expr.error_variant_index;
        return v;
    }
    if (expr.kind == HirExprKind::IfElse) {
        auto cond = mir_value(*expr.lhs, module, fn, ctx);
        if (!cond) return core::make_unexpected(cond.error());
        auto then_v = mir_value(*expr.rhs, module, fn, ctx);
        if (!then_v) return core::make_unexpected(then_v.error());
        auto else_v = mir_value(*expr.args[0], module, fn, ctx);
        if (!else_v) return core::make_unexpected(else_v.error());
        if (!fn->values.push(cond.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* cond_ptr = &fn->values[fn->values.len - 1];
        if (!fn->values.push(then_v.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* then_ptr = &fn->values[fn->values.len - 1];
        if (!fn->values.push(else_v.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* else_ptr = &fn->values[fn->values.len - 1];
        v.kind = MirValueKind::IfElse;
        v.type = mir_type_kind(expr.type);
        v.lhs = cond_ptr;
        v.rhs = then_ptr;
        if (!v.args.push(else_ptr)) return frontend_error(FrontendError::TooManyItems, expr.span);
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::Or) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        auto rhs = mir_value(*expr.rhs, module, fn, ctx);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* lhs_ptr = &fn->values[fn->values.len - 1];
        if (!fn->values.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        MirValue* rhs_ptr = &fn->values[fn->values.len - 1];
        v.kind = MirValueKind::Or;
        v.type = mir_type_kind(expr.type);
        v.lhs = lhs_ptr;
        v.rhs = rhs_ptr;
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.error_variant_index = expr.error_variant_index;
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::NoError) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::NoError;
        v.type = MirTypeKind::Bool;
        v.lhs = &fn->values[fn->values.len - 1];
        v.error_variant_index = expr.error_variant_index;
        return v;
    }
    if (expr.kind == HirExprKind::HasValue) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::HasValue;
        v.type = MirTypeKind::Bool;
        v.lhs = &fn->values[fn->values.len - 1];
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        return v;
    }
    if (expr.kind == HirExprKind::ValueOf) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::ValueOf;
        v.type = mir_type_kind(expr.type);
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        v.lhs = &fn->values[fn->values.len - 1];
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::MissingOf) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::MissingOf;
        v.type = mir_type_kind(expr.type);
        v.may_nil = expr.may_nil;
        v.may_error = expr.may_error;
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        v.lhs = &fn->values[fn->values.len - 1];
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::MatchPayload) {
        auto lhs = mir_value(*expr.lhs, module, fn, ctx);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->values.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        v.kind = MirValueKind::MatchPayload;
        v.type = mir_type_kind(expr.type);
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.case_index = expr.case_index;
        v.lhs = &fn->values[fn->values.len - 1];
        v.error_variant_index = expr.error_variant_index;
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    if (expr.kind == HirExprKind::LocalRef) {
        // For-loop unroll (Phase 4b): when lowering a body expression for
        // iteration j, ctx carries the loop var's ref_index and the element
        // MirValue for that iteration. A LocalRef whose local_index matches
        // the loop var is replaced with the element directly; the element
        // already carries its own shape / may_nil / may_error from having
        // been lowered via mir_value() on the ArrayLit element expr, so no
        // metadata merge is needed.
        //
        // LocalRefs to other locals (outer `let` bindings referenced from
        // inside the loop body, e.g. `guard n > threshold`) fall through
        // to the normal LocalRef lowering below — that path is exercised
        // whenever ctx is null (route-level lowering) or the local_index
        // doesn't match. The loop-var ref_index sentinel is validated once
        // in the unroll driver, not here, to keep this hot path branchless
        // for non-loop-var refs.
        if (ctx != nullptr && expr.local_index == ctx->loop_var_ref_index) {
            return ctx->current_element;
        }
        v.kind = MirValueKind::LocalRef;
        v.type = mir_type_kind(expr.type);
        v.variant_index = expr.variant_index;
        v.struct_index = expr.struct_index;
        v.local_index = expr.local_index;
        v.error_struct_index = expr.error_struct_index;
        v.error_variant_index = expr.error_variant_index;
        apply_expr_shape_if_available(module, expr, &v);
        return v;
    }
    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
}

}  // namespace

FrontendResult<MirModule*> build_mir(const HirModule& module) {
    auto* mir = new MirModule{};

    for (u32 i = 0; i < module.type_shapes.len; i++) {
        MirTypeShape shape{};
        shape.type = mir_type_kind(module.type_shapes[i].type);
        shape.is_concrete = module.type_shapes[i].is_concrete;
        shape.generic_index = module.type_shapes[i].generic_index;
        shape.variant_index = module.type_shapes[i].variant_index;
        shape.struct_index = module.type_shapes[i].struct_index;
        shape.tuple_len = module.type_shapes[i].tuple_len;
        for (u32 ti = 0; ti < shape.tuple_len; ti++) {
            shape.tuple_elem_shape_indices[ti] = module.type_shapes[i].tuple_elem_shape_indices[ti];
        }
        if (!mir->type_shapes.push(shape)) return frontend_error(FrontendError::TooManyItems, {});
    }
    for (u32 i = 0; i < module.upstreams.len; i++) {
        MirUpstream up{};
        up.span = module.upstreams[i].span;
        up.name = module.upstreams[i].name;
        up.id = module.upstreams[i].id;
        up.has_address = module.upstreams[i].has_address;
        up.ip = module.upstreams[i].ip;
        up.port = module.upstreams[i].port;
        if (!mir->upstreams.push(up)) return frontend_error(FrontendError::TooManyItems, up.span);
    }

    for (u32 i = 0; i < module.structs.len; i++) {
        MirStruct st{};
        st.span = module.structs[i].span;
        st.name = module.structs[i].name;
        st.conforms_error = module.structs[i].conforms_error;
        st.type_params = module.structs[i].type_params;
        st.template_struct_index = module.structs[i].template_struct_index;
        st.instance_type_arg_count = module.structs[i].instance_type_arg_count;
        for (u32 ai = 0; ai < st.instance_type_arg_count; ai++) {
            st.instance_type_args[ai] = mir_type_kind(module.structs[i].instance_type_args[ai]);
            st.instance_generic_indices[ai] = module.structs[i].instance_generic_indices[ai];
            st.instance_shape_indices[ai] = module.structs[i].instance_shape_indices[ai];
        }
        for (u32 fi = 0; fi < module.structs[i].fields.len; fi++) {
            MirStruct::FieldDecl field{};
            field.name = module.structs[i].fields[fi].name;
            field.type_name = module.structs[i].fields[fi].type_name;
            field.type = mir_type_kind(module.structs[i].fields[fi].type);
            field.shape_index = module.structs[i].fields[fi].shape_index;
            field.is_error_type = module.structs[i].fields[fi].is_error_type;
            field.variant_index = module.structs[i].fields[fi].variant_index;
            field.struct_index = module.structs[i].fields[fi].struct_index;
            field.tuple_len = module.structs[i].fields[fi].tuple_len;
            for (u32 ti = 0; ti < field.tuple_len; ti++) {
                field.tuple_types[ti] = mir_type_kind(module.structs[i].fields[fi].tuple_types[ti]);
                field.tuple_variant_indices[ti] =
                    module.structs[i].fields[fi].tuple_variant_indices[ti];
                field.tuple_struct_indices[ti] =
                    module.structs[i].fields[fi].tuple_struct_indices[ti];
            }
            if (!st.fields.push(field)) return frontend_error(FrontendError::TooManyItems, st.span);
        }
        if (!mir->structs.push(st)) return frontend_error(FrontendError::TooManyItems, st.span);
    }

    for (u32 i = 0; i < module.variants.len; i++) {
        MirVariant variant{};
        variant.span = module.variants[i].span;
        variant.name = module.variants[i].name;
        variant.type_params = module.variants[i].type_params;
        variant.template_variant_index = module.variants[i].template_variant_index;
        variant.instance_type_arg_count = module.variants[i].instance_type_arg_count;
        for (u32 ai = 0; ai < variant.instance_type_arg_count; ai++) {
            variant.instance_type_args[ai] =
                mir_type_kind(module.variants[i].instance_type_args[ai]);
            variant.instance_generic_indices[ai] = module.variants[i].instance_generic_indices[ai];
            variant.instance_shape_indices[ai] = module.variants[i].instance_shape_indices[ai];
        }
        for (u32 ci = 0; ci < module.variants[i].cases.len; ci++) {
            MirVariant::CaseDecl case_decl{};
            case_decl.name = module.variants[i].cases[ci].name;
            case_decl.has_payload = module.variants[i].cases[ci].has_payload;
            case_decl.payload_type = mir_type_kind(module.variants[i].cases[ci].payload_type);
            case_decl.payload_shape_index = module.variants[i].cases[ci].payload_shape_index;
            case_decl.payload_variant_index = module.variants[i].cases[ci].payload_variant_index;
            case_decl.payload_struct_index = module.variants[i].cases[ci].payload_struct_index;
            case_decl.payload_tuple_len = module.variants[i].cases[ci].payload_tuple_len;
            for (u32 ti = 0; ti < case_decl.payload_tuple_len; ti++) {
                case_decl.payload_tuple_types[ti] =
                    mir_type_kind(module.variants[i].cases[ci].payload_tuple_types[ti]);
                case_decl.payload_tuple_variant_indices[ti] =
                    module.variants[i].cases[ci].payload_tuple_variant_indices[ti];
                case_decl.payload_tuple_struct_indices[ti] =
                    module.variants[i].cases[ci].payload_tuple_struct_indices[ti];
            }
            if (!variant.cases.push(case_decl))
                return frontend_error(FrontendError::TooManyItems, variant.span);
        }
        if (!mir->variants.push(variant))
            return frontend_error(FrontendError::TooManyItems, variant.span);
    }

    bool struct_ready[MirModule::kMaxStructs]{};
    bool variant_ready[MirModule::kMaxVariants]{};
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 i = 0; i < mir->structs.len; i++) {
            if (struct_ready[i]) continue;
            const auto& st = mir->structs[i];
            bool ready = true;
            if (is_open_generic_struct_decl(st)) ready = false;
            if (ready && !instance_fully_concrete(*mir,
                                                  st.instance_type_arg_count,
                                                  st.instance_type_args,
                                                  st.instance_shape_indices))
                ready = false;
            for (u32 fi = 0; ready && fi < st.fields.len; fi++) {
                if (!field_carrier_ready(*mir, st.fields[fi], struct_ready, variant_ready))
                    ready = false;
            }
            if (ready) {
                struct_ready[i] = true;
                changed = true;
            }
        }
        for (u32 i = 0; i < mir->variants.len; i++) {
            if (variant_ready[i]) continue;
            const auto& variant = mir->variants[i];
            bool ready = true;
            if (is_open_generic_variant_decl(variant)) ready = false;
            if (ready && !instance_fully_concrete(*mir,
                                                  variant.instance_type_arg_count,
                                                  variant.instance_type_args,
                                                  variant.instance_shape_indices))
                ready = false;
            for (u32 ci = 0; ready && ci < variant.cases.len; ci++) {
                if (!variant_payload_carrier_ready(
                        *mir, variant.cases[ci], struct_ready, variant_ready))
                    ready = false;
            }
            if (ready) {
                variant_ready[i] = true;
                changed = true;
            }
        }
    }
    for (u32 i = 0; i < mir->type_shapes.len; i++) {
        mir->type_shapes[i].carrier_ready =
            shape_carrier_ready(*mir, i, struct_ready, variant_ready);
    }

    for (u32 i = 0; i < module.routes.len; i++) {
        MirFunction fn{};
        fn.span = module.routes[i].span;
        fn.method = module.routes[i].method;
        fn.path = module.routes[i].path;
        fn.name = {"route", 5};
        fn.error_variant_index = module.routes[i].error_variant_index;

        // Propagate wait(ms) list 1:1. Codegen will turn each into a yield
        // boundary in the generated state machine.
        for (u32 wi = 0; wi < module.routes[i].waits.len; wi++) {
            MirFunction::Wait w{};
            w.span = module.routes[i].waits[wi].span;
            w.ms = module.routes[i].waits[wi].ms;
            if (!fn.waits.push(w)) return frontend_error(FrontendError::TooManyItems, w.span);
        }

        for (u32 li = 0; li < module.routes[i].locals.len; li++) {
            if (module.routes[i].locals[li].type == HirTypeKind::Tuple) continue;
            MirLocal local{};
            local.span = module.routes[i].locals[li].span;
            local.name = module.routes[i].locals[li].name;
            local.ref_index = module.routes[i].locals[li].ref_index;
            local.type = mir_type_kind(module.routes[i].locals[li].type);
            local.shape_index = module.routes[i].locals[li].shape_index;
            local.may_nil = module.routes[i].locals[li].may_nil;
            local.may_error = module.routes[i].locals[li].may_error;
            local.variant_index = module.routes[i].locals[li].variant_index;
            local.struct_index = module.routes[i].locals[li].struct_index;
            local.tuple_len = module.routes[i].locals[li].tuple_len;
            for (u32 ti = 0; ti < local.tuple_len; ti++) {
                local.tuple_types[ti] = mir_type_kind(module.routes[i].locals[li].tuple_types[ti]);
                local.tuple_variant_indices[ti] =
                    module.routes[i].locals[li].tuple_variant_indices[ti];
                local.tuple_struct_indices[ti] =
                    module.routes[i].locals[li].tuple_struct_indices[ti];
            }
            local.error_struct_index = module.routes[i].locals[li].error_struct_index;
            local.error_variant_index = module.routes[i].locals[li].error_variant_index;
            auto init = mir_value(module.routes[i].locals[li].init, module, &fn);
            if (!init) return core::make_unexpected(init.error());
            local.init = init.value();
            if (!fn.locals.push(local))
                return frontend_error(FrontendError::TooManyItems, local.span);
        }

        auto set_term_from_hir = [](MirTerminator* out, const HirTerminator& term) {
            out->span = term.span;
            out->status_code = term.status_code;
            out->upstream_index = term.upstream_index;
            out->kind = term.kind == HirTerminatorKind::ReturnStatus
                            ? MirTerminatorKind::ReturnStatus
                            : MirTerminatorKind::ForwardUpstream;
            out->source_kind = term.source_kind == HirTerminatorSourceKind::LocalRef
                                   ? MirTerminatorSourceKind::LocalRef
                                   : MirTerminatorSourceKind::Literal;
            out->local_ref_index = term.local_ref_index;
            out->response_body = term.response_body;
            out->response_headers.len = 0;
            // Both HIR and MIR cap at 16 headers per terminator, so a
            // straight copy cannot truncate. Static-assert the cap
            // equality here so a future tweak on either side trips
            // the build instead of silently dropping headers.
            static_assert(HirTerminator::kMaxHeaders == MirTerminator::kMaxHeaders,
                          "HIR/MIR header cap must stay in lockstep or a different "
                          "copy strategy (returning an error) is required.");
            for (u32 i = 0; i < term.response_headers.len; i++) {
                const auto& p = term.response_headers[i];
                const bool ok = out->response_headers.push({p.key, p.value});
                // Unreachable given the static_assert above; use a
                // builtin trap in debug/release rather than `(void)ok`
                // so a regression surfaces immediately instead of
                // silently shipping a truncated header set.
                if (!ok) __builtin_trap();
            }
        };
        auto guard_fail_block_count = [&](const HirGuard& guard) -> u32 {
            if (guard.fail_kind == HirGuard::FailKind::Term) return 1;
            u32 non_wildcard = 0;
            for (u32 ai = 0; ai < guard.fail_match_count; ai++) {
                // one test block per non-wildcard arm, one case/default block per arm
                // fail_match arms live in route storage to keep HirGuard compact
                const auto& arm = module.guard_match_arms[guard.fail_match_start + ai];
                if (!arm.is_wildcard) non_wildcard++;
            }
            return non_wildcard + guard.fail_match_count;
        };
        auto emit_guard_fail = [&](const HirGuard& guard) -> FrontendResult<void> {
            if (guard.fail_kind == HirGuard::FailKind::Term) {
                MirBlock fail_block{};
                fail_block.label = fail_label();
                set_term_from_hir(&fail_block.term, guard.fail_term);
                if (!fn.blocks.push(fail_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);
                return {};
            }

            if (guard.fail_kind == HirGuard::FailKind::Body) {
                MirBlock fail_block{};
                fail_block.label = fail_label();
                if (guard.fail_body.body_kind == HirGuardBody::BodyKind::If) {
                    fail_block.term.kind = MirTerminatorKind::Branch;
                    fail_block.term.span = guard.fail_body.cond.span;
                    auto cond = mir_value(guard.fail_body.cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    fail_block.term.cond = cond.value();
                    const u32 then_index = fn.blocks.len + 1;
                    const u32 else_index = fn.blocks.len + 2;
                    fail_block.term.then_block = then_index;
                    fail_block.term.else_block = else_index;
                    if (!fn.blocks.push(fail_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);

                    MirBlock then_block{};
                    then_block.label = then_label();
                    set_term_from_hir(&then_block.term, guard.fail_body.then_term);
                    if (!fn.blocks.push(then_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);

                    MirBlock else_block{};
                    else_block.label = else_label();
                    set_term_from_hir(&else_block.term, guard.fail_body.else_term);
                    if (!fn.blocks.push(else_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                } else {
                    set_term_from_hir(&fail_block.term, guard.fail_body.direct_term);
                    if (!fn.blocks.push(fail_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }
                return {};
            }

            u32 test_index[HirGuard::kMaxFailMatchArms]{};
            u32 case_index[HirGuard::kMaxFailMatchArms]{};
            u32 cursor = fn.blocks.len;
            u32 default_index = 0xffffffffu;
            for (u32 ai = 0; ai < guard.fail_match_count; ai++) {
                const auto& arm = module.guard_match_arms[guard.fail_match_start + ai];
                if (!arm.is_wildcard) test_index[ai] = cursor++;
            }
            for (u32 ai = 0; ai < guard.fail_match_count; ai++) {
                const auto& arm = module.guard_match_arms[guard.fail_match_start + ai];
                case_index[ai] = cursor++;
                if (arm.is_wildcard) default_index = case_index[ai];
            }

            auto subject = mir_value(guard.fail_match_expr, module, &fn);
            if (!subject) return core::make_unexpected(subject.error());
            for (u32 ai = 0; ai < guard.fail_match_count; ai++) {
                const auto& arm = module.guard_match_arms[guard.fail_match_start + ai];
                if (arm.is_wildcard) continue;
                MirBlock test_block{};
                test_block.label = match_test_label();
                auto arm_pattern = mir_value(arm.pattern, module, &fn);
                if (!arm_pattern) return core::make_unexpected(arm_pattern.error());
                test_block.term.kind = MirTerminatorKind::Branch;
                test_block.term.use_cmp = true;
                test_block.term.span = arm.span;
                test_block.term.lhs = subject.value();
                test_block.term.rhs = arm_pattern.value();
                test_block.term.then_block = case_index[ai];
                u32 next_test = default_index;
                for (u32 next = ai + 1; next < guard.fail_match_count; next++) {
                    if (!module.guard_match_arms[guard.fail_match_start + next].is_wildcard) {
                        next_test = test_index[next];
                        break;
                    }
                }
                test_block.term.else_block = next_test;
                if (!fn.blocks.push(test_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);
            }

            for (u32 ai = 0; ai < guard.fail_match_count; ai++) {
                const auto& arm = module.guard_match_arms[guard.fail_match_start + ai];
                MirBlock case_block{};
                case_block.label = arm.is_wildcard ? match_default_label() : match_case_label();
                set_term_from_hir(&case_block.term, arm.direct_term);
                if (!fn.blocks.push(case_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);
            }
            return {};
        };

        // Phase 4b Scope A for-loop unroll. A for-loop compiles to a flat
        // chain of N × M virtual guards (N = iter_expr.args.len, M =
        // body.guards.len), each branching to the next on pass and to its
        // own fail block on failure. Pass of the final virtual guard falls
        // into the route's direct terminator. This pass handles only the
        // canonical allowlist-shaped pattern; other shapes remain rejected
        // here until Phase 4c/d generalize the emission.
        //
        // Scope A preconditions (checked below): exactly one for-loop, no
        // sibling route-level guards, route control is Direct, body has at
        // least one guard and no body terminator. Rejected shapes (route
        // guards mixed with for-loop, body terminator, if/match route
        // control, multiple for-loops) get FrontendError::UnsupportedSyntax
        // pointing at the for-loop span.
        if (module.routes[i].for_loops.len != 0) {
            const auto& fl = module.routes[i].for_loops[0];
            const bool scope_a = module.routes[i].for_loops.len == 1 &&
                                 module.routes[i].guards.len == 0 &&
                                 module.routes[i].control.kind == HirControlKind::Direct &&
                                 !fl.body.has_term && fl.body.guards.len != 0;
            if (!scope_a || fl.loop_var_ref_index == 0xffffffffu) {
                return frontend_error(FrontendError::UnsupportedSyntax, fl.span);
            }

            // Block budget pre-check. The unroll produces 2T+1 blocks
            // (T virtual guards + 1 body + T fail blocks, T = N × M).
            // MirFunction caps at kMaxBlocks, so worst-case shapes like
            // N=8,M=1 or N=4,M=2 would push past the cap mid-emission.
            // Reject up-front with a deterministic error at the for-loop
            // span instead of a TooManyItems buried inside a later push.
            {
                const u64 total_guards =
                    static_cast<u64>(fl.iter_expr.args.len) * fl.body.guards.len;
                if (2 * total_guards + 1 > MirFunction::kMaxBlocks) {
                    return frontend_error(FrontendError::TooManyItems, fl.span);
                }
            }

            // Unroll N × M virtual guards. Each entry pairs a body-guard
            // pointer with the ForLoopCtx that mir_value uses to substitute
            // the loop variable's LocalRef on the condition expression.
            // Phase 3b body guards are restricted to fail_kind == Term, so
            // emit_guard_fail pushes exactly one fail block per guard.
            struct UnrolledGuard {
                const HirGuard* guard;
                ForLoopCtx ctx;
            };
            constexpr u32 kMaxUnrolled = HirExpr::kMaxArgs * HirForLoopBody::kMaxGuards;
            FixedVec<UnrolledGuard, kMaxUnrolled> expanded{};
            for (u32 ai = 0; ai < fl.iter_expr.args.len; ai++) {
                auto elem = mir_value(*fl.iter_expr.args[ai], module, &fn);
                if (!elem) return core::make_unexpected(elem.error());
                for (u32 gi = 0; gi < fl.body.guards.len; gi++) {
                    UnrolledGuard u{};
                    u.guard = &fl.body.guards[gi];
                    u.ctx.loop_var_ref_index = fl.loop_var_ref_index;
                    u.ctx.current_element = elem.value();
                    if (!expanded.push(u))
                        return frontend_error(FrontendError::TooManyItems, fl.span);
                }
            }

            const u32 total = expanded.len;
            const u32 body_index = total;
            u32 guard_fail_index[kMaxUnrolled]{};
            u32 fail_cursor = body_index + 1;
            for (u32 gi = 0; gi < total; gi++) {
                guard_fail_index[gi] = fail_cursor++;
            }

            // Entry block: test the first virtual guard's condition.
            MirBlock entry_block{};
            entry_block.label = entry_label();
            entry_block.term.kind = MirTerminatorKind::Branch;
            entry_block.term.span = expanded[0].guard->span;
            auto cond0 = mir_value(expanded[0].guard->cond, module, &fn, &expanded[0].ctx);
            if (!cond0) return core::make_unexpected(cond0.error());
            entry_block.term.cond = cond0.value();
            entry_block.term.then_block = total > 1 ? 1 : body_index;
            entry_block.term.else_block = guard_fail_index[0];
            if (!fn.blocks.push(entry_block))
                return frontend_error(FrontendError::TooManyItems, fn.span);

            // Subsequent virtual-guard blocks.
            for (u32 gi = 1; gi < total; gi++) {
                MirBlock guard_block{};
                guard_block.label = cont_label();
                guard_block.term.kind = MirTerminatorKind::Branch;
                guard_block.term.span = expanded[gi].guard->span;
                auto cond = mir_value(expanded[gi].guard->cond, module, &fn, &expanded[gi].ctx);
                if (!cond) return core::make_unexpected(cond.error());
                guard_block.term.cond = cond.value();
                guard_block.term.then_block = gi + 1 < total ? gi + 1 : body_index;
                guard_block.term.else_block = guard_fail_index[gi];
                if (!fn.blocks.push(guard_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);
            }

            // Body block: the route's direct terminator.
            MirBlock body_block{};
            body_block.label = cont_label();
            set_term_from_hir(&body_block.term, module.routes[i].control.direct_term);
            if (!fn.blocks.push(body_block))
                return frontend_error(FrontendError::TooManyItems, fn.span);

            // Fail blocks, one per virtual guard.
            for (u32 gi = 0; gi < total; gi++) {
                auto emitted = emit_guard_fail(*expanded[gi].guard);
                if (!emitted) return core::make_unexpected(emitted.error());
            }

            if (!mir->functions.push(fn))
                return frontend_error(FrontendError::TooManyItems, fn.span);
            continue;
        }

        MirBlock block{};
        block.label = entry_label();
        if (module.routes[i].guards.len != 0) {
            const u32 guard_count = module.routes[i].guards.len;

            if (module.routes[i].control.kind == HirControlKind::Direct) {
                const u32 body_index = guard_count;
                u32 guard_fail_index[HirRoute::kMaxGuards]{};
                u32 fail_cursor = body_index + 1;
                for (u32 gi = 0; gi < guard_count; gi++) {
                    guard_fail_index[gi] = fail_cursor;
                    fail_cursor += guard_fail_block_count(module.routes[i].guards[gi]);
                }
                const auto& guard0 = module.routes[i].guards[0];
                block.term.kind = MirTerminatorKind::Branch;
                block.term.span = guard0.span;
                auto cond0 = mir_value(guard0.cond, module, &fn);
                if (!cond0) return core::make_unexpected(cond0.error());
                block.term.cond = cond0.value();
                block.term.then_block = guard_count > 1 ? 1 : body_index;
                block.term.else_block = guard_fail_index[0];
                if (!fn.blocks.push(block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                for (u32 gi = 1; gi < guard_count; gi++) {
                    MirBlock guard_block{};
                    guard_block.label = cont_label();
                    const auto& guard = module.routes[i].guards[gi];
                    guard_block.term.kind = MirTerminatorKind::Branch;
                    guard_block.term.span = guard.span;
                    auto cond = mir_value(guard.cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    guard_block.term.cond = cond.value();
                    guard_block.term.then_block = gi + 1 < guard_count ? gi + 1 : body_index;
                    guard_block.term.else_block = guard_fail_index[gi];
                    if (!fn.blocks.push(guard_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }

                MirBlock cont_block{};
                cont_block.label = cont_label();
                set_term_from_hir(&cont_block.term, module.routes[i].control.direct_term);
                if (!fn.blocks.push(cont_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                for (u32 gi = 0; gi < guard_count; gi++) {
                    auto emitted = emit_guard_fail(module.routes[i].guards[gi]);
                    if (!emitted) return core::make_unexpected(emitted.error());
                }
            } else if (module.routes[i].control.kind == HirControlKind::If) {
                const u32 body_index = guard_count;
                const u32 then_index = body_index + 1;
                const u32 else_index = body_index + 2;
                u32 guard_fail_index[HirRoute::kMaxGuards]{};
                u32 fail_cursor = body_index + 3;
                for (u32 gi = 0; gi < guard_count; gi++) {
                    guard_fail_index[gi] = fail_cursor;
                    fail_cursor += guard_fail_block_count(module.routes[i].guards[gi]);
                }
                const auto& guard0 = module.routes[i].guards[0];
                block.term.kind = MirTerminatorKind::Branch;
                block.term.span = guard0.span;
                auto cond0 = mir_value(guard0.cond, module, &fn);
                if (!cond0) return core::make_unexpected(cond0.error());
                block.term.cond = cond0.value();
                block.term.then_block = guard_count > 1 ? 1 : body_index;
                block.term.else_block = guard_fail_index[0];
                if (!fn.blocks.push(block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                for (u32 gi = 1; gi < guard_count; gi++) {
                    MirBlock guard_block{};
                    guard_block.label = cont_label();
                    const auto& guard = module.routes[i].guards[gi];
                    guard_block.term.kind = MirTerminatorKind::Branch;
                    guard_block.term.span = guard.span;
                    auto cond = mir_value(guard.cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    guard_block.term.cond = cond.value();
                    guard_block.term.then_block = gi + 1 < guard_count ? gi + 1 : body_index;
                    guard_block.term.else_block = guard_fail_index[gi];
                    if (!fn.blocks.push(guard_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }

                MirBlock cont_block{};
                cont_block.label = cont_label();
                cont_block.term.kind = MirTerminatorKind::Branch;
                cont_block.term.span = module.routes[i].control.cond.span;
                auto if_cond = mir_value(module.routes[i].control.cond, module, &fn);
                if (!if_cond) return core::make_unexpected(if_cond.error());
                cont_block.term.cond = if_cond.value();
                cont_block.term.then_block = then_index;
                cont_block.term.else_block = else_index;
                if (!fn.blocks.push(cont_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                MirBlock then_block{};
                then_block.label = then_label();
                set_term_from_hir(&then_block.term, module.routes[i].control.then_term);
                if (!fn.blocks.push(then_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                MirBlock else_block{};
                else_block.label = else_label();
                set_term_from_hir(&else_block.term, module.routes[i].control.else_term);
                if (!fn.blocks.push(else_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                for (u32 gi = 0; gi < guard_count; gi++) {
                    auto emitted = emit_guard_fail(module.routes[i].guards[gi]);
                    if (!emitted) return core::make_unexpected(emitted.error());
                }
            } else if (module.routes[i].control.kind == HirControlKind::Match) {
                const u32 arm_count = module.routes[i].control.match_arms.len;
                const u32 test_count = arm_count - 1;
                u32 arm_block_index[HirControl::kMaxMatchArms]{};
                u32 arm_body_index[HirControl::kMaxMatchArms]{};
                u32 arm_then_index[HirControl::kMaxMatchArms]{};
                u32 arm_else_index[HirControl::kMaxMatchArms]{};
                u32 arm_guard_index[HirControl::kMaxMatchArms][HirMatchArm::kMaxPreludeGuards]{};
                u32 arm_guard_fail_index[HirControl::kMaxMatchArms]
                                        [HirMatchArm::kMaxPreludeGuards]{};
                u32 next_index = guard_count + test_count;
                for (u32 ai = 0; ai < arm_count; ai++) {
                    const auto& arm = module.routes[i].control.match_arms[ai];
                    arm_block_index[ai] = next_index++;
                    if (arm.guards.len != 0) {
                        for (u32 gi = 1; gi < arm.guards.len; gi++)
                            arm_guard_index[ai][gi] = next_index++;
                        arm_body_index[ai] = next_index++;
                        for (u32 gi = 0; gi < arm.guards.len; gi++) {
                            arm_guard_fail_index[ai][gi] = next_index;
                            next_index += guard_fail_block_count(arm.guards[gi]);
                        }
                    } else {
                        arm_body_index[ai] = arm_block_index[ai];
                    }
                    if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        arm_then_index[ai] = next_index++;
                        arm_else_index[ai] = next_index++;
                    }
                }
                u32 guard_fail_index[HirRoute::kMaxGuards]{};
                u32 fail_cursor = next_index;
                for (u32 gi = 0; gi < guard_count; gi++) {
                    guard_fail_index[gi] = fail_cursor;
                    fail_cursor += guard_fail_block_count(module.routes[i].guards[gi]);
                }
                const auto& guard0 = module.routes[i].guards[0];
                block.term.kind = MirTerminatorKind::Branch;
                block.term.span = guard0.span;
                auto cond0 = mir_value(guard0.cond, module, &fn);
                if (!cond0) return core::make_unexpected(cond0.error());
                block.term.cond = cond0.value();
                block.term.then_block =
                    guard_count > 1 ? 1 : (test_count > 0 ? guard_count : arm_block_index[0]);
                block.term.else_block = guard_fail_index[0];
                if (!fn.blocks.push(block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);

                for (u32 gi = 1; gi < guard_count; gi++) {
                    MirBlock guard_block{};
                    guard_block.label = cont_label();
                    const auto& guard = module.routes[i].guards[gi];
                    guard_block.term.kind = MirTerminatorKind::Branch;
                    guard_block.term.span = guard.span;
                    auto cond = mir_value(guard.cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    guard_block.term.cond = cond.value();
                    guard_block.term.then_block =
                        gi + 1 < guard_count ? gi + 1
                                             : (test_count > 0 ? guard_count : arm_block_index[0]);
                    guard_block.term.else_block = guard_fail_index[gi];
                    if (!fn.blocks.push(guard_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }

                auto subject = mir_value(module.routes[i].control.match_expr, module, &fn);
                if (!subject) return core::make_unexpected(subject.error());
                for (u32 ai = 0; ai < test_count; ai++) {
                    MirBlock test_block{};
                    test_block.label = ai == 0 ? cont_label() : match_test_label();
                    const auto& arm = module.routes[i].control.match_arms[ai];
                    auto arm_pattern = mir_value(arm.pattern, module, &fn);
                    if (!arm_pattern) return core::make_unexpected(arm_pattern.error());
                    test_block.term.kind = MirTerminatorKind::Branch;
                    test_block.term.use_cmp = true;
                    test_block.term.span = arm.span;
                    test_block.term.lhs = subject.value();
                    test_block.term.rhs = arm_pattern.value();
                    test_block.term.then_block = arm_block_index[ai];
                    test_block.term.else_block =
                        ai + 1 < test_count ? (ai + 2) : arm_block_index[test_count];
                    if (!fn.blocks.push(test_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }

                for (u32 ai = 0; ai < arm_count; ai++) {
                    MirBlock case_block{};
                    const auto& arm = module.routes[i].control.match_arms[ai];
                    case_block.label = arm.is_wildcard ? match_default_label() : match_case_label();
                    if (arm.guards.len != 0) {
                        auto cond = mir_value(arm.guards[0].cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        case_block.term.kind = MirTerminatorKind::Branch;
                        case_block.term.span = arm.guards[0].span;
                        case_block.term.cond = cond.value();
                        case_block.term.then_block =
                            arm.guards.len > 1 ? arm_guard_index[ai][1] : arm_body_index[ai];
                        case_block.term.else_block = arm_guard_fail_index[ai][0];
                    } else if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        case_block.term.kind = MirTerminatorKind::Branch;
                        case_block.term.span = arm.cond.span;
                        auto cond = mir_value(arm.cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        case_block.term.cond = cond.value();
                        case_block.term.then_block = arm_then_index[ai];
                        case_block.term.else_block = arm_else_index[ai];
                    } else {
                        set_term_from_hir(&case_block.term, arm.direct_term);
                    }
                    if (!fn.blocks.push(case_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                    for (u32 gi = 1; gi < arm.guards.len; gi++) {
                        MirBlock guard_block{};
                        guard_block.label = cont_label();
                        auto cond = mir_value(arm.guards[gi].cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        guard_block.term.kind = MirTerminatorKind::Branch;
                        guard_block.term.span = arm.guards[gi].span;
                        guard_block.term.cond = cond.value();
                        guard_block.term.then_block = gi + 1 < arm.guards.len
                                                          ? arm_guard_index[ai][gi + 1]
                                                          : arm_body_index[ai];
                        guard_block.term.else_block = arm_guard_fail_index[ai][gi];
                        if (!fn.blocks.push(guard_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                    }
                    if (arm.guards.len != 0) {
                        MirBlock body_block{};
                        body_block.label = cont_label();
                        if (arm.body_kind == HirMatchArm::BodyKind::If) {
                            body_block.term.kind = MirTerminatorKind::Branch;
                            body_block.term.span = arm.cond.span;
                            auto cond = mir_value(arm.cond, module, &fn);
                            if (!cond) return core::make_unexpected(cond.error());
                            body_block.term.cond = cond.value();
                            body_block.term.then_block = arm_then_index[ai];
                            body_block.term.else_block = arm_else_index[ai];
                        } else {
                            set_term_from_hir(&body_block.term, arm.direct_term);
                        }
                        if (!fn.blocks.push(body_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                        for (u32 gi = 0; gi < arm.guards.len; gi++) {
                            auto emitted = emit_guard_fail(arm.guards[gi]);
                            if (!emitted) return core::make_unexpected(emitted.error());
                        }
                    }
                    if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        MirBlock then_block{};
                        then_block.label = then_label();
                        set_term_from_hir(&then_block.term, arm.then_term);
                        if (!fn.blocks.push(then_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                        MirBlock else_block{};
                        else_block.label = else_label();
                        set_term_from_hir(&else_block.term, arm.else_term);
                        if (!fn.blocks.push(else_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                    }
                }

                for (u32 gi = 0; gi < guard_count; gi++) {
                    auto emitted = emit_guard_fail(module.routes[i].guards[gi]);
                    if (!emitted) return core::make_unexpected(emitted.error());
                }
            } else {
                return frontend_error(FrontendError::UnsupportedSyntax, fn.span);
            }
        } else if (module.routes[i].control.kind == HirControlKind::If) {
            block.term.kind = MirTerminatorKind::Branch;
            block.term.span = module.routes[i].control.cond.span;
            auto cond = mir_value(module.routes[i].control.cond, module, &fn);
            if (!cond) return core::make_unexpected(cond.error());
            block.term.cond = cond.value();
            block.term.then_block = 1;
            block.term.else_block = 2;
            if (!fn.blocks.push(block)) return frontend_error(FrontendError::TooManyItems, fn.span);

            MirBlock then_block{};
            then_block.label = then_label();
            set_term_from_hir(&then_block.term, module.routes[i].control.then_term);
            if (!fn.blocks.push(then_block))
                return frontend_error(FrontendError::TooManyItems, fn.span);

            MirBlock else_block{};
            else_block.label = else_label();
            set_term_from_hir(&else_block.term, module.routes[i].control.else_term);
            if (!fn.blocks.push(else_block))
                return frontend_error(FrontendError::TooManyItems, fn.span);
        } else if (module.routes[i].control.kind == HirControlKind::Match) {
            const u32 arm_count = module.routes[i].control.match_arms.len;
            const u32 test_count = arm_count - 1;
            u32 arm_block_index[HirControl::kMaxMatchArms]{};
            u32 arm_body_index[HirControl::kMaxMatchArms]{};
            u32 arm_then_index[HirControl::kMaxMatchArms]{};
            u32 arm_else_index[HirControl::kMaxMatchArms]{};
            u32 arm_guard_index[HirControl::kMaxMatchArms][HirMatchArm::kMaxPreludeGuards]{};
            u32 arm_guard_fail_index[HirControl::kMaxMatchArms][HirMatchArm::kMaxPreludeGuards]{};
            u32 next_index = test_count;
            for (u32 ai = 0; ai < arm_count; ai++) {
                const auto& arm = module.routes[i].control.match_arms[ai];
                arm_block_index[ai] = next_index++;
                if (arm.guards.len != 0) {
                    for (u32 gi = 1; gi < arm.guards.len; gi++)
                        arm_guard_index[ai][gi] = next_index++;
                    arm_body_index[ai] = next_index++;
                    for (u32 gi = 0; gi < arm.guards.len; gi++) {
                        arm_guard_fail_index[ai][gi] = next_index;
                        next_index += guard_fail_block_count(arm.guards[gi]);
                    }
                } else {
                    arm_body_index[ai] = arm_block_index[ai];
                }
                if (arm.body_kind == HirMatchArm::BodyKind::If) {
                    arm_then_index[ai] = next_index++;
                    arm_else_index[ai] = next_index++;
                }
            }
            auto subject = mir_value(module.routes[i].control.match_expr, module, &fn);
            if (!subject) return core::make_unexpected(subject.error());
            if (test_count == 0) {
                MirBlock case_block{};
                const auto& arm = module.routes[i].control.match_arms[0];
                case_block.label = arm.is_wildcard ? match_default_label() : match_case_label();
                if (arm.guards.len != 0) {
                    auto cond = mir_value(arm.guards[0].cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    case_block.term.kind = MirTerminatorKind::Branch;
                    case_block.term.span = arm.guards[0].span;
                    case_block.term.cond = cond.value();
                    case_block.term.then_block =
                        arm.guards.len > 1 ? arm_guard_index[0][1] : arm_body_index[0];
                    case_block.term.else_block = arm_guard_fail_index[0][0];
                } else if (arm.body_kind == HirMatchArm::BodyKind::If) {
                    case_block.term.kind = MirTerminatorKind::Branch;
                    case_block.term.span = arm.cond.span;
                    auto cond = mir_value(arm.cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    case_block.term.cond = cond.value();
                    case_block.term.then_block = arm_then_index[0];
                    case_block.term.else_block = arm_else_index[0];
                } else {
                    set_term_from_hir(&case_block.term, arm.direct_term);
                }
                if (!fn.blocks.push(case_block))
                    return frontend_error(FrontendError::TooManyItems, fn.span);
                for (u32 gi = 1; gi < arm.guards.len; gi++) {
                    MirBlock guard_block{};
                    guard_block.label = cont_label();
                    auto cond = mir_value(arm.guards[gi].cond, module, &fn);
                    if (!cond) return core::make_unexpected(cond.error());
                    guard_block.term.kind = MirTerminatorKind::Branch;
                    guard_block.term.span = arm.guards[gi].span;
                    guard_block.term.cond = cond.value();
                    guard_block.term.then_block =
                        gi + 1 < arm.guards.len ? arm_guard_index[0][gi + 1] : arm_body_index[0];
                    guard_block.term.else_block = arm_guard_fail_index[0][gi];
                    if (!fn.blocks.push(guard_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }
                if (arm.guards.len != 0) {
                    MirBlock body_block{};
                    body_block.label = cont_label();
                    if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        body_block.term.kind = MirTerminatorKind::Branch;
                        body_block.term.span = arm.cond.span;
                        auto cond = mir_value(arm.cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        body_block.term.cond = cond.value();
                        body_block.term.then_block = arm_then_index[0];
                        body_block.term.else_block = arm_else_index[0];
                    } else {
                        set_term_from_hir(&body_block.term, arm.direct_term);
                    }
                    if (!fn.blocks.push(body_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                    for (u32 gi = 0; gi < arm.guards.len; gi++) {
                        auto emitted = emit_guard_fail(arm.guards[gi]);
                        if (!emitted) return core::make_unexpected(emitted.error());
                    }
                }
                if (arm.body_kind == HirMatchArm::BodyKind::If) {
                    MirBlock then_block{};
                    then_block.label = then_label();
                    set_term_from_hir(&then_block.term, arm.then_term);
                    if (!fn.blocks.push(then_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                    MirBlock else_block{};
                    else_block.label = else_label();
                    set_term_from_hir(&else_block.term, arm.else_term);
                    if (!fn.blocks.push(else_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }
            } else {
                for (u32 ai = 0; ai < test_count; ai++) {
                    MirBlock test_block{};
                    test_block.label = match_test_label();
                    const auto& arm = module.routes[i].control.match_arms[ai];
                    auto arm_pattern = mir_value(arm.pattern, module, &fn);
                    if (!arm_pattern) return core::make_unexpected(arm_pattern.error());
                    test_block.term.kind = MirTerminatorKind::Branch;
                    test_block.term.use_cmp = true;
                    test_block.term.span = arm.span;
                    test_block.term.lhs = subject.value();
                    test_block.term.rhs = arm_pattern.value();
                    test_block.term.then_block = arm_block_index[ai];
                    test_block.term.else_block =
                        ai + 1 < test_count ? ai + 1 : arm_block_index[test_count];
                    if (!fn.blocks.push(test_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                }

                for (u32 ai = 0; ai < arm_count; ai++) {
                    MirBlock case_block{};
                    const auto& arm = module.routes[i].control.match_arms[ai];
                    case_block.label = arm.is_wildcard ? match_default_label() : match_case_label();
                    if (arm.guards.len != 0) {
                        auto cond = mir_value(arm.guards[0].cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        case_block.term.kind = MirTerminatorKind::Branch;
                        case_block.term.span = arm.guards[0].span;
                        case_block.term.cond = cond.value();
                        case_block.term.then_block =
                            arm.guards.len > 1 ? arm_guard_index[ai][1] : arm_body_index[ai];
                        case_block.term.else_block = arm_guard_fail_index[ai][0];
                    } else if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        case_block.term.kind = MirTerminatorKind::Branch;
                        case_block.term.span = arm.cond.span;
                        auto cond = mir_value(arm.cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        case_block.term.cond = cond.value();
                        case_block.term.then_block = arm_then_index[ai];
                        case_block.term.else_block = arm_else_index[ai];
                    } else {
                        set_term_from_hir(&case_block.term, arm.direct_term);
                    }
                    if (!fn.blocks.push(case_block))
                        return frontend_error(FrontendError::TooManyItems, fn.span);
                    for (u32 gi = 1; gi < arm.guards.len; gi++) {
                        MirBlock guard_block{};
                        guard_block.label = cont_label();
                        auto cond = mir_value(arm.guards[gi].cond, module, &fn);
                        if (!cond) return core::make_unexpected(cond.error());
                        guard_block.term.kind = MirTerminatorKind::Branch;
                        guard_block.term.span = arm.guards[gi].span;
                        guard_block.term.cond = cond.value();
                        guard_block.term.then_block = gi + 1 < arm.guards.len
                                                          ? arm_guard_index[ai][gi + 1]
                                                          : arm_body_index[ai];
                        guard_block.term.else_block = arm_guard_fail_index[ai][gi];
                        if (!fn.blocks.push(guard_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                    }
                    if (arm.guards.len != 0) {
                        MirBlock body_block{};
                        body_block.label = cont_label();
                        if (arm.body_kind == HirMatchArm::BodyKind::If) {
                            body_block.term.kind = MirTerminatorKind::Branch;
                            body_block.term.span = arm.cond.span;
                            auto cond = mir_value(arm.cond, module, &fn);
                            if (!cond) return core::make_unexpected(cond.error());
                            body_block.term.cond = cond.value();
                            body_block.term.then_block = arm_then_index[ai];
                            body_block.term.else_block = arm_else_index[ai];
                        } else {
                            set_term_from_hir(&body_block.term, arm.direct_term);
                        }
                        if (!fn.blocks.push(body_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                        for (u32 gi = 0; gi < arm.guards.len; gi++) {
                            auto emitted = emit_guard_fail(arm.guards[gi]);
                            if (!emitted) return core::make_unexpected(emitted.error());
                        }
                    }
                    if (arm.body_kind == HirMatchArm::BodyKind::If) {
                        MirBlock then_block{};
                        then_block.label = then_label();
                        set_term_from_hir(&then_block.term, arm.then_term);
                        if (!fn.blocks.push(then_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                        MirBlock else_block{};
                        else_block.label = else_label();
                        set_term_from_hir(&else_block.term, arm.else_term);
                        if (!fn.blocks.push(else_block))
                            return frontend_error(FrontendError::TooManyItems, fn.span);
                    }
                }
            }
        } else {
            set_term_from_hir(&block.term, module.routes[i].control.direct_term);
            if (!fn.blocks.push(block)) return frontend_error(FrontendError::TooManyItems, fn.span);
        }
        if (!mir->functions.push(fn)) return frontend_error(FrontendError::TooManyItems, fn.span);
    }

    return mir;
}

}  // namespace rut
