#pragma once

#include "rut/common/types.h"
#include "rut/compiler/diagnostic.h"

namespace rut {

inline constexpr u32 kMaxMirTupleSlots = 10;

enum class MirTerminatorKind : u8 {
    Branch,
    ReturnStatus,
    ForwardUpstream,
};

enum class MirValueKind : u8 {
    BoolConst,
    IntConst,
    StrConst,
    Tuple,
    TupleSlot,
    VariantCase,
    IfElse,
    StructInit,
    Field,
    ReqHeader,
    Nil,
    Error,
    LocalRef,
    Eq,
    Lt,
    Gt,
    Or,
    NoError,
    HasValue,
    ValueOf,
    MissingOf,
    MatchPayload,
};

enum class MirTypeKind : u8 {
    Unknown,
    Bool,
    I32,
    Str,
    Variant,
    Tuple,
    Struct,
};

struct MirTypeShape {
    MirTypeKind type = MirTypeKind::Unknown;
    bool is_concrete = false;
    bool carrier_ready = false;
    u32 generic_index = 0xffffffffu;
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    u32 tuple_elem_shape_indices[kMaxMirTupleSlots]{};
};

struct MirVariant {
    static constexpr u32 kMaxTypeParams = 4;
    struct CaseDecl {
        Str name{};
        MirTypeKind payload_type = MirTypeKind::Unknown;
        bool has_payload = false;
        u32 payload_shape_index = 0xffffffffu;
        u32 payload_variant_index = 0xffffffffu;
        u32 payload_struct_index = 0xffffffffu;
        u32 payload_tuple_len = 0;
        MirTypeKind payload_tuple_types[kMaxMirTupleSlots]{};
        u32 payload_tuple_variant_indices[kMaxMirTupleSlots]{};
        u32 payload_tuple_struct_indices[kMaxMirTupleSlots]{};
    };

    Span span{};
    Str name{};
    FixedVec<Str, kMaxTypeParams> type_params;
    u32 template_variant_index = 0xffffffffu;
    u32 instance_type_arg_count = 0;
    MirTypeKind instance_type_args[kMaxTypeParams]{};
    u32 instance_generic_indices[kMaxTypeParams]{};
    u32 instance_shape_indices[kMaxTypeParams]{};
    static constexpr u32 kMaxCases = 16;
    FixedVec<CaseDecl, kMaxCases> cases;
};

struct MirStruct {
    static constexpr u32 kMaxTypeParams = 4;
    struct FieldDecl {
        Str name{};
        Str type_name{};
        MirTypeKind type = MirTypeKind::Unknown;
        u32 shape_index = 0xffffffffu;
        bool is_error_type = false;
        u32 variant_index = 0xffffffffu;
        u32 struct_index = 0xffffffffu;
        u32 tuple_len = 0;
        MirTypeKind tuple_types[kMaxMirTupleSlots]{};
        u32 tuple_variant_indices[kMaxMirTupleSlots]{};
        u32 tuple_struct_indices[kMaxMirTupleSlots]{};
    };

    Span span{};
    Str name{};
    bool conforms_error = false;
    FixedVec<Str, kMaxTypeParams> type_params;
    u32 template_struct_index = 0xffffffffu;
    u32 instance_type_arg_count = 0;
    MirTypeKind instance_type_args[kMaxTypeParams]{};
    u32 instance_generic_indices[kMaxTypeParams]{};
    u32 instance_shape_indices[kMaxTypeParams]{};
    static constexpr u32 kMaxFields = 8;
    FixedVec<FieldDecl, kMaxFields> fields;
};

struct MirValue {
    struct FieldInit {
        Str name{};
        MirValue* value = nullptr;
    };

    MirValueKind kind = MirValueKind::BoolConst;
    MirTypeKind type = MirTypeKind::Unknown;
    u32 shape_index = 0xffffffffu;
    bool may_nil = false;
    bool may_error = false;
    bool bool_value = false;
    i32 int_value = 0;
    Str str_value{};
    Str msg{};
    u32 local_index = 0;
    u32 variant_index = 0;
    u32 struct_index = 0xffffffffu;
    u32 case_index = 0;
    u32 tuple_len = 0;
    MirTypeKind tuple_types[kMaxMirTupleSlots]{};
    u32 tuple_variant_indices[kMaxMirTupleSlots]{};
    u32 tuple_struct_indices[kMaxMirTupleSlots]{};
    u32 error_struct_index = 0xffffffffu;
    u32 error_variant_index = 0xffffffffu;
    u32 error_case_index = 0xffffffffu;
    MirValue* lhs = nullptr;
    MirValue* rhs = nullptr;
    static constexpr u32 kMaxFieldInits = 8;
    static constexpr u32 kMaxArgs = 8;
    FixedVec<FieldInit, kMaxFieldInits> field_inits;
    FixedVec<MirValue*, kMaxArgs> args;
};

struct MirLocal {
    Span span{};
    Str name{};
    u32 ref_index = 0;
    MirTypeKind type = MirTypeKind::Bool;
    u32 shape_index = 0xffffffffu;
    bool may_nil = false;
    bool may_error = false;
    u32 variant_index = 0;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    MirTypeKind tuple_types[kMaxMirTupleSlots]{};
    u32 tuple_variant_indices[kMaxMirTupleSlots]{};
    u32 tuple_struct_indices[kMaxMirTupleSlots]{};
    u32 error_struct_index = 0xffffffffu;
    u32 error_variant_index = 0xffffffffu;
    MirValue init{};
};

enum class MirTerminatorSourceKind : u8 {
    Literal,
    LocalRef,
};

struct MirTerminator {
    MirTerminatorKind kind = MirTerminatorKind::ReturnStatus;
    Span span{};
    MirTerminatorSourceKind source_kind = MirTerminatorSourceKind::Literal;
    i32 status_code = 0;
    u32 local_ref_index = 0xffffffffu;
    u32 upstream_index = 0;
    bool use_cmp = false;
    MirValue cond{};
    MirValue lhs{};
    MirValue rhs{};
    u32 then_block = 0;
    u32 else_block = 0;
};

struct MirBlock {
    Str label{};
    MirTerminator term{};
};

struct MirFunction {
    Span span{};
    u8 method = 0;
    Str path{};
    Str name{};
    static constexpr u32 kMaxLocals = 16;
    static constexpr u32 kMaxBlocks = 16;
    static constexpr u32 kMaxValues = 64;
    FixedVec<MirValue, kMaxValues> values;
    FixedVec<MirLocal, kMaxLocals> locals;
    FixedVec<MirBlock, kMaxBlocks> blocks;
    u32 error_variant_index = 0xffffffffu;

    MirFunction() = default;
    MirFunction(const MirFunction& other)
        : span(other.span),
          method(other.method),
          path(other.path),
          name(other.name),
          values(other.values),
          locals(other.locals),
          blocks(other.blocks),
          error_variant_index(other.error_variant_index) {
        rebase_from(other);
    }
    MirFunction& operator=(const MirFunction& other) {
        if (this == &other) return *this;
        span = other.span;
        method = other.method;
        path = other.path;
        name = other.name;
        values = other.values;
        locals = other.locals;
        blocks = other.blocks;
        error_variant_index = other.error_variant_index;
        rebase_from(other);
        return *this;
    }
    MirFunction(MirFunction&& other) noexcept
        : span(other.span),
          method(other.method),
          path(other.path),
          name(other.name),
          values(other.values),
          locals(other.locals),
          blocks(other.blocks),
          error_variant_index(other.error_variant_index) {
        rebase_from(other);
    }
    MirFunction& operator=(MirFunction&& other) noexcept {
        if (this == &other) return *this;
        span = other.span;
        method = other.method;
        path = other.path;
        name = other.name;
        values = other.values;
        locals = other.locals;
        blocks = other.blocks;
        error_variant_index = other.error_variant_index;
        rebase_from(other);
        return *this;
    }

private:
    void rebase_value_ptr(const MirFunction& other, MirValue*& ptr) {
        if (ptr == nullptr) return;
        const auto begin = &other.values.data[0];
        const auto end = begin + other.values.len;
        if (ptr < begin || ptr >= end) return;
        const u32 index = static_cast<u32>(ptr - begin);
        ptr = &values.data[index];
    }

    void rebase_value(MirValue& value, const MirFunction& other) {
        rebase_value_ptr(other, value.lhs);
        rebase_value_ptr(other, value.rhs);
        for (u32 i = 0; i < value.field_inits.len; i++) {
            rebase_value_ptr(other, value.field_inits[i].value);
        }
        for (u32 i = 0; i < value.args.len; i++) {
            rebase_value_ptr(other, value.args[i]);
        }
    }

    void rebase_from(const MirFunction& other) {
        for (u32 i = 0; i < values.len; i++) rebase_value(values[i], other);
        for (u32 i = 0; i < locals.len; i++) rebase_value(locals[i].init, other);
        for (u32 i = 0; i < blocks.len; i++) {
            rebase_value(blocks[i].term.cond, other);
            rebase_value(blocks[i].term.lhs, other);
            rebase_value(blocks[i].term.rhs, other);
        }
    }
};

struct MirUpstream {
    Span span{};
    Str name{};
    u16 id = 0;
};

struct MirModule {
    static constexpr u32 kMaxUpstreams = 32;
    static constexpr u32 kMaxStructs = 64;
    static constexpr u32 kMaxVariants = 32;
    static constexpr u32 kMaxFunctions = 96;
    static constexpr u32 kMaxTypeShapes = 256;

    FixedVec<MirUpstream, kMaxUpstreams> upstreams;
    FixedVec<MirStruct, kMaxStructs> structs;
    FixedVec<MirVariant, kMaxVariants> variants;
    FixedVec<MirFunction, kMaxFunctions> functions;
    FixedVec<MirTypeShape, kMaxTypeShapes> type_shapes;

    MirModule() = default;
    MirModule(const MirModule& other)
        : upstreams(other.upstreams),
          structs(other.structs),
          variants(other.variants),
          functions(other.functions),
          type_shapes(other.type_shapes) {}
    MirModule& operator=(const MirModule& other) {
        if (this == &other) return *this;
        upstreams = other.upstreams;
        structs = other.structs;
        variants = other.variants;
        functions = other.functions;
        type_shapes = other.type_shapes;
        return *this;
    }
    MirModule(MirModule&& other) noexcept
        : upstreams(other.upstreams),
          structs(other.structs),
          variants(other.variants),
          functions(other.functions),
          type_shapes(other.type_shapes) {}
    MirModule& operator=(MirModule&& other) noexcept {
        if (this == &other) return *this;
        upstreams = other.upstreams;
        structs = other.structs;
        variants = other.variants;
        functions = other.functions;
        type_shapes = other.type_shapes;
        return *this;
    }
};

}  // namespace rut
