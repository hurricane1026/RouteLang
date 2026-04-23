#pragma once

#include "rut/common/types.h"
#include "rut/compiler/diagnostic.h"
#include <deque>
#include <string>

namespace rut {

enum class HirProtocolKind : u8 {
    Custom,
    Error,
    Eq,
    Ord,
};

struct HirUpstream {
    Span span{};
    Str name{};
    u16 id = 0;
    // Address declared in the DSL via `upstream X at "..."` or
    // `upstream X { host: "...", port: N }`. `has_address == false`
    // means the runtime must bind this upstream via add_upstream();
    // analyze doesn't force an address on every upstream, to stay
    // compatible with test configs that register upstreams manually.
    // ip is in host byte order (RouteConfig::add_upstream expects
    // the same).
    bool has_address = false;
    u32 ip = 0;
    u16 port = 0;
};
struct HirImport {
    Span span{};
    Str path{};
    bool selective = false;
    bool has_namespace_alias = false;
    Str namespace_alias{};
    bool has_package_decl = false;
    bool same_package = false;
    Str package_name{};
};

struct HirAlias {
    static constexpr u32 kMaxTargetParts = 8;
    Span span{};
    Str name{};
    FixedVec<Str, kMaxTargetParts> target_parts;
};

enum class HirExprKind : u8 {
    BoolLit,
    IntLit,
    StrLit,
    Tuple,
    // Array literal: elements stored in `args`. analyze rejects heterogeneous
    // arrays and empty `[]` without a type annotation (Rutlang has no
    // push/append so size + element type must be compile-time known). The
    // result's HirTypeShape carries array_len + array_elem_shape_index.
    ArrayLit,
    TupleSlot,
    VariantCase,
    IfElse,
    Call,
    StructInit,
    Field,
    ReqHeader,
    // HTTP method literal — int_value holds the HttpMethod enum
    // value (0=GET, 1=POST, …, matching rut/runtime/http_parser.h).
    ConstMethod,
    // Read of the parsed request method from the current connection.
    ReqMethod,
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
    ProtocolCall,
};

enum class HirTypeKind : u8 {
    Unknown,
    Bool,
    I32,
    Str,
    Generic,
    Variant,
    Tuple,
    Struct,
    Method,
    // Homogeneous fixed-size sequence. Carrier for `for x in <arr>` iteration.
    // Complete type info lives in HirTypeShape via (array_len,
    // array_elem_shape_index) — composite-type host structures (HirLocal,
    // HirExpr, etc.) reference it through their existing `shape_index`
    // rather than mirroring inline fields.
    Array,
};

inline constexpr u32 kMaxTupleSlots = 10;

struct HirTypeShape {
    HirTypeKind type = HirTypeKind::Unknown;
    bool is_concrete = false;
    u32 generic_index = 0xffffffffu;
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    u32 tuple_elem_shape_indices[kMaxTupleSlots]{};
    // Array-typed shape: array_elem_shape_index points at another
    // HirTypeShape in HirModule::type_shapes describing the element type.
    // Length is a *value* property (lives on HirExpr.array_len, carried
    // forward to MIR for unroll) — not a type property, so two arrays of
    // the same element type with different lengths still share one shape.
    // Sentinel: type != Array → array_elem_shape_index = 0xffffffffu.
    u32 array_elem_shape_index = 0xffffffffu;
};

struct HirProtocol {
    static constexpr u32 kMaxMethods = 8;
    struct MethodDecl {
        struct ParamDecl {
            Str type_name{};
            HirTypeKind type = HirTypeKind::Unknown;
            u32 generic_index = 0xffffffffu;
            u32 variant_index = 0xffffffffu;
            u32 struct_index = 0xffffffffu;
            u32 tuple_len = 0;
            HirTypeKind tuple_types[kMaxTupleSlots]{};
            u32 tuple_variant_indices[kMaxTupleSlots]{};
            u32 tuple_struct_indices[kMaxTupleSlots]{};
            u32 shape_index = 0xffffffffu;
        };
        Str name{};
        FixedVec<ParamDecl, 8> params;
        bool has_return_type = false;
        Str return_type_name{};
        HirTypeKind return_type = HirTypeKind::Unknown;
        u32 return_generic_index = 0xffffffffu;
        u32 return_variant_index = 0xffffffffu;
        u32 return_struct_index = 0xffffffffu;
        u32 return_tuple_len = 0;
        HirTypeKind return_tuple_types[kMaxTupleSlots]{};
        u32 return_tuple_variant_indices[kMaxTupleSlots]{};
        u32 return_tuple_struct_indices[kMaxTupleSlots]{};
        u32 return_shape_index = 0xffffffffu;
        bool return_may_nil = false;
        bool return_may_error = false;
        u32 return_error_struct_index = 0xffffffffu;
        u32 return_error_variant_index = 0xffffffffu;
        u32 function_index = 0xffffffffu;
    };
    Span span{};
    Str name{};
    HirProtocolKind kind = HirProtocolKind::Custom;
    FixedVec<MethodDecl, kMaxMethods> methods;
};

struct HirConformance {
    Span span{};
    u32 protocol_index = 0xffffffffu;
    HirTypeKind type = HirTypeKind::Unknown;
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
    bool is_generic_template = false;
};

struct HirVariant {
    static constexpr u32 kMaxTypeParams = 4;
    static constexpr u32 kMaxGenericProtocols = 4;
    struct TypeArgRef {
        HirTypeKind type = HirTypeKind::Unknown;
        u32 generic_index = 0xffffffffu;
        u32 variant_index = 0xffffffffu;
        u32 struct_index = 0xffffffffu;
        u32 tuple_len = 0;
        HirTypeKind tuple_types[kMaxTupleSlots]{};
        u32 tuple_variant_indices[kMaxTupleSlots]{};
        u32 tuple_struct_indices[kMaxTupleSlots]{};
        u32 shape_index = 0xffffffffu;
    };
    struct CaseDecl {
        Str name{};
        HirTypeKind payload_type = HirTypeKind::Unknown;
        bool has_payload = false;
        u32 payload_generic_index = 0xffffffffu;
        bool payload_generic_has_error_constraint = false;
        bool payload_generic_has_eq_constraint = false;
        bool payload_generic_has_ord_constraint = false;
        u32 payload_generic_protocol_index = 0xffffffffu;
        u32 payload_generic_protocol_count = 0;
        u32 payload_generic_protocol_indices[kMaxGenericProtocols]{};
        u32 payload_variant_index = 0xffffffffu;
        u32 payload_struct_index = 0xffffffffu;
        u32 payload_tuple_len = 0;
        HirTypeKind payload_tuple_types[kMaxTupleSlots]{};
        u32 payload_tuple_variant_indices[kMaxTupleSlots]{};
        u32 payload_tuple_struct_indices[kMaxTupleSlots]{};
        u32 payload_template_variant_index = 0xffffffffu;
        u32 payload_template_struct_index = 0xffffffffu;
        u32 payload_type_arg_count = 0;
        TypeArgRef payload_type_args[kMaxTypeParams]{};
        u32 payload_shape_index = 0xffffffffu;
    };

    Span span{};
    Str name{};
    FixedVec<Str, kMaxTypeParams> type_params;
    u32 template_variant_index = 0xffffffffu;
    u32 instance_type_arg_count = 0;
    HirTypeKind instance_type_args[kMaxTypeParams]{};
    u32 instance_generic_indices[kMaxTypeParams]{};
    u32 instance_variant_indices[kMaxTypeParams]{};
    u32 instance_struct_indices[kMaxTypeParams]{};
    u32 instance_tuple_lens[kMaxTypeParams]{};
    HirTypeKind instance_tuple_types[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_tuple_variant_indices[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_tuple_struct_indices[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_shape_indices[kMaxTypeParams]{};
    static constexpr u32 kMaxCases = 16;
    FixedVec<CaseDecl, kMaxCases> cases;
};

struct HirStruct {
    static constexpr u32 kMaxTypeParams = 4;
    static constexpr u32 kMaxGenericProtocols = 4;
    using TypeArgRef = HirVariant::TypeArgRef;
    struct FieldDecl {
        Str name{};
        Str type_name{};
        HirTypeKind type = HirTypeKind::Unknown;
        bool is_error_type = false;
        u32 generic_index = 0xffffffffu;
        bool generic_has_error_constraint = false;
        bool generic_has_eq_constraint = false;
        bool generic_has_ord_constraint = false;
        u32 generic_protocol_index = 0xffffffffu;
        u32 generic_protocol_count = 0;
        u32 generic_protocol_indices[kMaxGenericProtocols]{};
        u32 variant_index = 0xffffffffu;
        u32 struct_index = 0xffffffffu;
        u32 tuple_len = 0;
        HirTypeKind tuple_types[kMaxTupleSlots]{};
        u32 tuple_variant_indices[kMaxTupleSlots]{};
        u32 tuple_struct_indices[kMaxTupleSlots]{};
        u32 template_variant_index = 0xffffffffu;
        u32 template_struct_index = 0xffffffffu;
        u32 type_arg_count = 0;
        TypeArgRef type_args[kMaxTypeParams]{};
        u32 shape_index = 0xffffffffu;
    };

    Span span{};
    Str name{};
    bool conforms_error = false;
    FixedVec<Str, kMaxTypeParams> type_params;
    u32 template_struct_index = 0xffffffffu;
    u32 instance_type_arg_count = 0;
    HirTypeKind instance_type_args[kMaxTypeParams]{};
    u32 instance_generic_indices[kMaxTypeParams]{};
    u32 instance_variant_indices[kMaxTypeParams]{};
    u32 instance_struct_indices[kMaxTypeParams]{};
    u32 instance_tuple_lens[kMaxTypeParams]{};
    HirTypeKind instance_tuple_types[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_tuple_variant_indices[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_tuple_struct_indices[kMaxTypeParams][kMaxTupleSlots]{};
    u32 instance_shape_indices[kMaxTypeParams]{};
    static constexpr u32 kMaxFields = 8;
    FixedVec<FieldDecl, kMaxFields> fields;
};

struct HirExpr {
    struct FieldInit {
        Str name{};
        HirExpr* value = nullptr;
    };

    HirExprKind kind = HirExprKind::BoolLit;
    HirTypeKind type = HirTypeKind::Unknown;
    Span span{};
    bool may_nil = false;
    bool may_error = false;
    bool bool_value = false;
    i32 int_value = 0;
    Str str_value{};
    Str msg{};
    u32 local_index = 0;
    u32 generic_index = 0xffffffffu;
    bool generic_has_error_constraint = false;
    bool generic_has_eq_constraint = false;
    bool generic_has_ord_constraint = false;
    static constexpr u32 kMaxGenericProtocols = 4;
    u32 generic_protocol_index = 0xffffffffu;
    u32 generic_protocol_count = 0;
    u32 generic_protocol_indices[kMaxGenericProtocols]{};
    u32 protocol_index = 0xffffffffu;
    u32 variant_index = 0;
    u32 struct_index = 0xffffffffu;
    u32 case_index = 0;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
    u32 shape_index = 0xffffffffu;
    u32 error_struct_index = 0xffffffffu;
    u32 error_variant_index = 0xffffffffu;
    u32 error_case_index = 0xffffffffu;
    // For HirExprKind::ArrayLit: compile-time-known element count. Elements
    // themselves live in `args`. For non-Array exprs: 0.
    u32 array_len = 0;
    HirExpr* lhs = nullptr;
    HirExpr* rhs = nullptr;
    static constexpr u32 kMaxFieldInits = 8;
    static constexpr u32 kMaxArgs = 32;  // shared w/ AstExpr::kMaxArgs (Phase 1)
    FixedVec<FieldInit, kMaxFieldInits> field_inits;
    FixedVec<HirExpr*, kMaxArgs> args;
};

struct HirFunction {
    static constexpr u32 kMaxTypeParams = 4;
    struct TypeParamDecl {
        static constexpr u32 kMaxConstraints = 4;
        Str name{};
        bool has_constraint = false;
        Str constraint{};
        HirProtocolKind constraint_kind = HirProtocolKind::Custom;
        bool has_error_constraint = false;
        bool has_eq_constraint = false;
        bool has_ord_constraint = false;
        u32 custom_protocol_count = 0;
        Str constraints[kMaxConstraints]{};
        HirProtocolKind constraint_kinds[kMaxConstraints]{};
        u32 custom_protocol_indices[kMaxConstraints]{};
    };
    struct ParamDecl {
        Str name{};
        HirTypeKind type = HirTypeKind::Unknown;
        u32 generic_index = 0xffffffffu;
        bool generic_has_error_constraint = false;
        bool generic_has_eq_constraint = false;
        bool generic_has_ord_constraint = false;
        u32 generic_protocol_index = 0xffffffffu;
        u32 generic_protocol_count = 0;
        u32 generic_protocol_indices[HirExpr::kMaxGenericProtocols]{};
        u32 template_variant_index = 0xffffffffu;
        u32 template_struct_index = 0xffffffffu;
        u32 type_arg_count = 0;
        HirVariant::TypeArgRef type_args[kMaxTypeParams]{};
        u32 variant_index = 0xffffffffu;
        u32 struct_index = 0xffffffffu;
        u32 tuple_len = 0;
        HirTypeKind tuple_types[kMaxTupleSlots]{};
        u32 tuple_variant_indices[kMaxTupleSlots]{};
        u32 tuple_struct_indices[kMaxTupleSlots]{};
        u32 shape_index = 0xffffffffu;
        bool has_underscore_label = false;
    };

    Span span{};
    Str name{};
    HirTypeKind return_type = HirTypeKind::Unknown;
    u32 return_generic_index = 0xffffffffu;
    u32 return_template_variant_index = 0xffffffffu;
    u32 return_template_struct_index = 0xffffffffu;
    u32 return_type_arg_count = 0;
    HirVariant::TypeArgRef return_type_args[kMaxTypeParams]{};
    u32 return_variant_index = 0xffffffffu;
    u32 return_struct_index = 0xffffffffu;
    u32 return_tuple_len = 0;
    HirTypeKind return_tuple_types[kMaxTupleSlots]{};
    u32 return_tuple_variant_indices[kMaxTupleSlots]{};
    u32 return_tuple_struct_indices[kMaxTupleSlots]{};
    u32 return_shape_index = 0xffffffffu;
    static constexpr u32 kMaxParams = 8;
    static constexpr u32 kMaxExprs = 64;
    FixedVec<TypeParamDecl, kMaxTypeParams> type_params;
    FixedVec<ParamDecl, kMaxParams> params;
    FixedVec<HirExpr, kMaxExprs> exprs;
    HirExpr body{};

    HirFunction() = default;
    HirFunction(const HirFunction& other)
        : span(other.span),
          name(other.name),
          return_type(other.return_type),
          return_generic_index(other.return_generic_index),
          return_template_variant_index(other.return_template_variant_index),
          return_template_struct_index(other.return_template_struct_index),
          return_type_arg_count(other.return_type_arg_count),
          return_variant_index(other.return_variant_index),
          return_struct_index(other.return_struct_index),
          return_tuple_len(other.return_tuple_len),
          return_shape_index(other.return_shape_index),
          type_params(other.type_params),
          params(other.params),
          exprs(other.exprs),
          body(other.body) {
        for (u32 i = 0; i < other.return_tuple_len; i++) {
            return_tuple_types[i] = other.return_tuple_types[i];
            return_tuple_variant_indices[i] = other.return_tuple_variant_indices[i];
            return_tuple_struct_indices[i] = other.return_tuple_struct_indices[i];
        }
        for (u32 i = 0; i < other.return_type_arg_count; i++) {
            return_type_args[i] = other.return_type_args[i];
        }
        rebase_from(other);
    }
    HirFunction& operator=(const HirFunction& other) {
        if (this == &other) return *this;
        span = other.span;
        name = other.name;
        return_type = other.return_type;
        return_generic_index = other.return_generic_index;
        return_template_variant_index = other.return_template_variant_index;
        return_template_struct_index = other.return_template_struct_index;
        return_type_arg_count = other.return_type_arg_count;
        return_variant_index = other.return_variant_index;
        return_struct_index = other.return_struct_index;
        return_tuple_len = other.return_tuple_len;
        return_shape_index = other.return_shape_index;
        for (u32 i = 0; i < other.return_tuple_len; i++) {
            return_tuple_types[i] = other.return_tuple_types[i];
            return_tuple_variant_indices[i] = other.return_tuple_variant_indices[i];
            return_tuple_struct_indices[i] = other.return_tuple_struct_indices[i];
        }
        for (u32 i = 0; i < other.return_type_arg_count; i++) {
            return_type_args[i] = other.return_type_args[i];
        }
        type_params = other.type_params;
        params = other.params;
        exprs = other.exprs;
        body = other.body;
        rebase_from(other);
        return *this;
    }
    HirFunction(HirFunction&& other) noexcept
        : span(other.span),
          name(other.name),
          return_type(other.return_type),
          return_generic_index(other.return_generic_index),
          return_template_variant_index(other.return_template_variant_index),
          return_template_struct_index(other.return_template_struct_index),
          return_type_arg_count(other.return_type_arg_count),
          return_variant_index(other.return_variant_index),
          return_struct_index(other.return_struct_index),
          return_tuple_len(other.return_tuple_len),
          return_shape_index(other.return_shape_index),
          type_params(other.type_params),
          params(other.params),
          exprs(other.exprs),
          body(other.body) {
        for (u32 i = 0; i < other.return_tuple_len; i++) {
            return_tuple_types[i] = other.return_tuple_types[i];
            return_tuple_variant_indices[i] = other.return_tuple_variant_indices[i];
            return_tuple_struct_indices[i] = other.return_tuple_struct_indices[i];
        }
        for (u32 i = 0; i < other.return_type_arg_count; i++) {
            return_type_args[i] = other.return_type_args[i];
        }
        rebase_from(other);
    }
    HirFunction& operator=(HirFunction&& other) noexcept {
        if (this == &other) return *this;
        span = other.span;
        name = other.name;
        return_type = other.return_type;
        return_generic_index = other.return_generic_index;
        return_template_variant_index = other.return_template_variant_index;
        return_template_struct_index = other.return_template_struct_index;
        return_type_arg_count = other.return_type_arg_count;
        return_variant_index = other.return_variant_index;
        return_struct_index = other.return_struct_index;
        return_tuple_len = other.return_tuple_len;
        return_shape_index = other.return_shape_index;
        for (u32 i = 0; i < other.return_tuple_len; i++) {
            return_tuple_types[i] = other.return_tuple_types[i];
            return_tuple_variant_indices[i] = other.return_tuple_variant_indices[i];
            return_tuple_struct_indices[i] = other.return_tuple_struct_indices[i];
        }
        for (u32 i = 0; i < other.return_type_arg_count; i++) {
            return_type_args[i] = other.return_type_args[i];
        }
        type_params = other.type_params;
        params = other.params;
        exprs = other.exprs;
        body = other.body;
        rebase_from(other);
        return *this;
    }

private:
    void rebase_expr_ptr(const HirFunction& other, HirExpr*& ptr) {
        if (ptr == nullptr) return;
        const auto begin = &other.exprs.data[0];
        const auto end = begin + other.exprs.len;
        if (ptr < begin || ptr >= end) return;
        const u32 index = static_cast<u32>(ptr - begin);
        ptr = &exprs.data[index];
    }

    void rebase_expr(HirExpr& expr, const HirFunction& other) {
        rebase_expr_ptr(other, expr.lhs);
        rebase_expr_ptr(other, expr.rhs);
        for (u32 i = 0; i < expr.field_inits.len; i++) {
            rebase_expr_ptr(other, expr.field_inits[i].value);
        }
        for (u32 i = 0; i < expr.args.len; i++) {
            rebase_expr_ptr(other, expr.args[i]);
        }
    }

    void rebase_from(const HirFunction& other) {
        for (u32 i = 0; i < exprs.len; i++) rebase_expr(exprs[i], other);
        rebase_expr(body, other);
    }
};

struct HirLocal {
    Span span{};
    Str name{};
    u32 ref_index = 0;
    HirTypeKind type = HirTypeKind::Unknown;
    u32 generic_index = 0xffffffffu;
    bool generic_has_error_constraint = false;
    bool generic_has_eq_constraint = false;
    bool generic_has_ord_constraint = false;
    u32 generic_protocol_index = 0xffffffffu;
    u32 generic_protocol_count = 0;
    u32 generic_protocol_indices[HirExpr::kMaxGenericProtocols]{};
    bool may_nil = false;
    bool may_error = false;
    u32 variant_index = 0;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
    u32 shape_index = 0xffffffffu;
    u32 error_struct_index = 0xffffffffu;
    u32 error_variant_index = 0xffffffffu;
    HirExpr init{};
};

enum class HirTerminatorKind : u8 {
    ReturnStatus,
    ForwardUpstream,
};

// Where the runtime status value comes from, when kind == ReturnStatus.
// Literal: status_code is the i32 to return (compile-time constant).
// LocalRef: read the value of route.locals[local_ref_index] at runtime.
enum class HirTerminatorSourceKind : u8 {
    Literal,
    LocalRef,
};

struct HirHeaderKV {
    Str key{};
    Str value{};
};

struct HirTerminator {
    HirTerminatorKind kind = HirTerminatorKind::ReturnStatus;
    Span span{};
    HirTerminatorSourceKind source_kind = HirTerminatorSourceKind::Literal;
    i32 status_code = 0;
    u32 local_ref_index = 0xffffffffu;
    u32 upstream_index = 0;
    // Optional response body literal (populated when the source was
    // `return response(N, body: "...")`). Sentinel by ptr, not len:
    //   ptr == nullptr   → no body kwarg, use default status-reason.
    //   ptr != nullptr   → explicit body (including `body: ""` which
    //                      keeps ptr non-null with len == 0).
    // analyze.cc::analyze_term only copies this when the Ast source
    // had `has_response_body == true`, so the sentinel is preserved
    // end-to-end.
    Str response_body{};
    // Optional response headers from `response(N, headers: {...})`.
    // Inline-stored so analyze doesn't need the AstFile handle, and
    // downstream passes don't need a module-level pool. len == 0
    // means "no kwarg" (parser rejects explicit empty dicts).
    static constexpr u32 kMaxHeaders = 16;
    FixedVec<HirHeaderKV, kMaxHeaders> response_headers;
};

struct HirGuardBody {
    enum class BodyKind : u8 {
        Direct,
        If,
    };

    BodyKind body_kind = BodyKind::Direct;
    HirExpr cond{};
    HirTerminator then_term{};
    HirTerminator else_term{};
    HirTerminator direct_term{};
};

struct HirGuardMatchArm {
    Span span{};
    bool is_wildcard = false;
    HirExpr pattern{};
    HirTerminator direct_term{};
};

struct HirGuard {
    enum class FailKind : u8 {
        Term,
        Match,
        Body,
    };

    static constexpr u32 kMaxFailMatchArms = 8;
    Span span{};
    HirExpr cond{};
    FailKind fail_kind = FailKind::Term;
    HirTerminator fail_term{};
    HirExpr fail_match_expr{};
    u32 fail_match_start = 0;
    u32 fail_match_count = 0;
    HirGuardBody fail_body{};
};

struct HirMatchArm {
    enum class BodyKind : u8 {
        Direct,
        If,
    };

    Span span{};
    bool is_wildcard = false;
    HirExpr pattern{};
    bool bind_payload = false;
    Str bind_name{};
    HirTypeKind bind_type = HirTypeKind::Unknown;
    u32 bind_variant_index = 0xffffffffu;
    u32 bind_struct_index = 0xffffffffu;
    u32 bind_tuple_len = 0;
    HirTypeKind bind_tuple_types[kMaxTupleSlots]{};
    u32 bind_tuple_variant_indices[kMaxTupleSlots]{};
    u32 bind_tuple_struct_indices[kMaxTupleSlots]{};
    BodyKind body_kind = BodyKind::Direct;
    static constexpr u32 kMaxPreludeGuards = 4;
    FixedVec<HirGuard, kMaxPreludeGuards> guards;
    HirExpr cond{};
    HirTerminator then_term{};
    HirTerminator else_term{};
    HirTerminator direct_term{};
};

enum class HirControlKind : u8 {
    Direct,
    If,
    Match,
};

struct HirControl {
    HirControlKind kind = HirControlKind::Direct;
    static constexpr u32 kMaxMatchArms = 8;
    HirExpr cond{};
    HirExpr match_expr{};
    FixedVec<HirMatchArm, kMaxMatchArms> match_arms;
    HirTerminator then_term{};
    HirTerminator else_term{};
    HirTerminator direct_term{};
};

// Body of a `for ... in` loop. Phase 3b MVP: body is `guard*` plus an
// optional terminator (return / forward). Lets, nested for-loops, and
// arbitrary if/match are deferred to later phases. All HirExpr* inside this
// body's guards/terminator point into the parent HirRoute::exprs pool — so
// HirRoute::rebase_from must walk this substructure too.
struct HirForLoopBody {
    static constexpr u32 kMaxGuards = 4;
    FixedVec<HirGuard, kMaxGuards> guards;
    HirTerminator term{};
    bool has_term = false;
};

struct HirForLoop {
    Span span{};
    // Iteration source. Must type-check as Array<T>; the element type T
    // binds the loop variable's HirTypeKind below. iter_expr's HirExpr*
    // subfields point into HirRoute::exprs, so they need rebase too.
    HirExpr iter_expr{};
    // Loop variable (e.g., `item` in `for item in xs`). Scope is the body
    // only; analyze pushes it into route.locals, analyzes the body, then
    // rolls back route.locals.len so the name doesn't leak.
    Str loop_var_name{};
    HirTypeKind loop_var_type = HirTypeKind::Unknown;
    u32 loop_var_variant_index = 0xffffffffu;
    u32 loop_var_struct_index = 0xffffffffu;
    u32 loop_var_shape_index = 0xffffffffu;
    HirForLoopBody body{};
};

struct HirRoute {
    struct DecoratorRef {
        Span span{};
        Str name{};
        u32 function_index = 0xffffffffu;  // resolved in analyze; 0xffffffffu = unresolved
    };
    struct Wait {
        Span span{};
        u32 ms = 0;  // duration in milliseconds; packed into the u32 yield
                     // payload (status_code + upstream_id) at codegen time.
                     // Parser caps at UINT32_MAX (~49 days).
    };

    Span span{};
    u8 method = 0;
    Str path{};
    static constexpr u32 kMaxLocals = 16;
    static constexpr u32 kMaxGuards = 8;
    static constexpr u32 kMaxExprs = 64;
    static constexpr u32 kMaxDecorators = 8;
    static constexpr u32 kMaxWaits = 4;
    static constexpr u32 kMaxForLoops = 4;
    FixedVec<HirExpr, kMaxExprs> exprs;
    FixedVec<HirLocal, kMaxLocals> locals;
    FixedVec<HirGuard, kMaxGuards> guards;
    FixedVec<DecoratorRef, kMaxDecorators> decorators;
    FixedVec<Wait, kMaxWaits> waits;
    FixedVec<HirForLoop, kMaxForLoops> for_loops;
    HirControl control{};
    u32 error_variant_index = 0xffffffffu;

    HirRoute() = default;
    HirRoute(const HirRoute& other)
        : span(other.span),
          method(other.method),
          path(other.path),
          exprs(other.exprs),
          locals(other.locals),
          guards(other.guards),
          decorators(other.decorators),
          waits(other.waits),
          for_loops(other.for_loops),
          control(other.control),
          error_variant_index(other.error_variant_index) {
        rebase_from(other);
    }
    HirRoute& operator=(const HirRoute& other) {
        if (this == &other) return *this;
        span = other.span;
        method = other.method;
        path = other.path;
        exprs = other.exprs;
        locals = other.locals;
        guards = other.guards;
        decorators = other.decorators;
        waits = other.waits;
        for_loops = other.for_loops;
        control = other.control;
        error_variant_index = other.error_variant_index;
        rebase_from(other);
        return *this;
    }
    HirRoute(HirRoute&& other) noexcept
        : span(other.span),
          method(other.method),
          path(other.path),
          exprs(other.exprs),
          locals(other.locals),
          guards(other.guards),
          decorators(other.decorators),
          waits(other.waits),
          for_loops(other.for_loops),
          control(other.control),
          error_variant_index(other.error_variant_index) {
        rebase_from(other);
    }
    HirRoute& operator=(HirRoute&& other) noexcept {
        if (this == &other) return *this;
        span = other.span;
        method = other.method;
        path = other.path;
        exprs = other.exprs;
        locals = other.locals;
        guards = other.guards;
        decorators = other.decorators;
        waits = other.waits;
        for_loops = other.for_loops;
        control = other.control;
        error_variant_index = other.error_variant_index;
        rebase_from(other);
        return *this;
    }

private:
    void rebase_expr_ptr(const HirRoute& other, HirExpr*& ptr) {
        if (ptr == nullptr) return;
        const auto begin = &other.exprs.data[0];
        const auto end = begin + other.exprs.len;
        if (ptr < begin || ptr >= end) return;
        const u32 index = static_cast<u32>(ptr - begin);
        ptr = &exprs.data[index];
    }

    void rebase_expr(HirExpr& expr, const HirRoute& other) {
        rebase_expr_ptr(other, expr.lhs);
        rebase_expr_ptr(other, expr.rhs);
        for (u32 i = 0; i < expr.field_inits.len; i++) {
            rebase_expr_ptr(other, expr.field_inits[i].value);
        }
        for (u32 i = 0; i < expr.args.len; i++) {
            rebase_expr_ptr(other, expr.args[i]);
        }
    }

    void rebase_from(const HirRoute& other) {
        for (u32 i = 0; i < exprs.len; i++) rebase_expr(exprs[i], other);
        for (u32 i = 0; i < locals.len; i++) rebase_expr(locals[i].init, other);
        for (u32 i = 0; i < guards.len; i++) {
            rebase_expr(guards[i].cond, other);
            rebase_expr(guards[i].fail_match_expr, other);
            rebase_expr(guards[i].fail_body.cond, other);
        }
        // For-loops: iter_expr and the body's guard conds all point into
        // `exprs`, so rebase them the same way as top-level guards.
        for (u32 i = 0; i < for_loops.len; i++) {
            rebase_expr(for_loops[i].iter_expr, other);
            for (u32 gi = 0; gi < for_loops[i].body.guards.len; gi++) {
                rebase_expr(for_loops[i].body.guards[gi].cond, other);
                rebase_expr(for_loops[i].body.guards[gi].fail_match_expr, other);
                rebase_expr(for_loops[i].body.guards[gi].fail_body.cond, other);
            }
        }
        rebase_expr(control.cond, other);
        rebase_expr(control.match_expr, other);
        for (u32 i = 0; i < control.match_arms.len; i++) {
            rebase_expr(control.match_arms[i].pattern, other);
            for (u32 gi = 0; gi < control.match_arms[i].guards.len; gi++) {
                rebase_expr(control.match_arms[i].guards[gi].cond, other);
            }
            rebase_expr(control.match_arms[i].cond, other);
        }
    }
};

struct HirImplMethod {
    Str name{};
    u32 function_index = 0xffffffffu;
};

struct HirImpl {
    static constexpr u32 kMaxMethods = 8;
    Span span{};
    u32 protocol_index = 0xffffffffu;
    HirTypeKind type = HirTypeKind::Unknown;
    u32 struct_index = 0xffffffffu;
    bool is_generic_template = false;
    FixedVec<HirImplMethod, kMaxMethods> methods;
};

struct HirModule {
    static constexpr u32 kMaxUpstreams = 32;
    static constexpr u32 kMaxImports = 64;
    static constexpr u32 kMaxAliases = 64;
    static constexpr u32 kMaxFunctions = 64;
    static constexpr u32 kMaxStructs = 64;
    static constexpr u32 kMaxVariants = 64;
    static constexpr u32 kMaxProtocols = 32;
    static constexpr u32 kMaxConformances = 64;
    static constexpr u32 kMaxImpls = 64;
    static constexpr u32 kMaxRoutes = 96;
    static constexpr u32 kMaxGuardMatchArms = 64;
    static constexpr u32 kMaxTypeShapes = 512;

    FixedVec<HirUpstream, kMaxUpstreams> upstreams;
    FixedVec<HirImport, kMaxImports> imports;
    FixedVec<HirAlias, kMaxAliases> aliases;
    FixedVec<HirFunction, kMaxFunctions> functions;
    FixedVec<HirStruct, kMaxStructs> structs;
    FixedVec<HirVariant, kMaxVariants> variants;
    FixedVec<HirProtocol, kMaxProtocols> protocols;
    FixedVec<HirConformance, kMaxConformances> conformances;
    FixedVec<HirImpl, kMaxImpls> impls;
    FixedVec<HirGuardMatchArm, kMaxGuardMatchArms> guard_match_arms;
    FixedVec<HirRoute, kMaxRoutes> routes;
    FixedVec<HirTypeShape, kMaxTypeShapes> type_shapes;
    std::deque<std::string> owned_strings;
    bool has_package_decl = false;
    Span package_span{};
    Str package_name{};

    HirModule() = default;
    HirModule(const HirModule& other)
        : upstreams(other.upstreams),
          imports(other.imports),
          aliases(other.aliases),
          functions(other.functions),
          structs(other.structs),
          variants(other.variants),
          protocols(other.protocols),
          conformances(other.conformances),
          impls(other.impls),
          guard_match_arms(other.guard_match_arms),
          routes(other.routes),
          type_shapes(other.type_shapes),
          owned_strings(other.owned_strings),
          has_package_decl(other.has_package_decl),
          package_span(other.package_span),
          package_name(other.package_name) {}
    HirModule& operator=(const HirModule& other) {
        if (this == &other) return *this;
        upstreams = other.upstreams;
        imports = other.imports;
        aliases = other.aliases;
        functions = other.functions;
        structs = other.structs;
        variants = other.variants;
        protocols = other.protocols;
        conformances = other.conformances;
        impls = other.impls;
        guard_match_arms = other.guard_match_arms;
        routes = other.routes;
        type_shapes = other.type_shapes;
        owned_strings = other.owned_strings;
        has_package_decl = other.has_package_decl;
        package_span = other.package_span;
        package_name = other.package_name;
        return *this;
    }
    HirModule(HirModule&& other) noexcept
        : upstreams(other.upstreams),
          imports(other.imports),
          aliases(other.aliases),
          functions(other.functions),
          structs(other.structs),
          variants(other.variants),
          protocols(other.protocols),
          conformances(other.conformances),
          impls(other.impls),
          guard_match_arms(other.guard_match_arms),
          routes(other.routes),
          type_shapes(other.type_shapes),
          owned_strings(other.owned_strings),
          has_package_decl(other.has_package_decl),
          package_span(other.package_span),
          package_name(other.package_name) {}
    HirModule& operator=(HirModule&& other) noexcept {
        if (this == &other) return *this;
        upstreams = other.upstreams;
        imports = other.imports;
        aliases = other.aliases;
        functions = other.functions;
        structs = other.structs;
        variants = other.variants;
        protocols = other.protocols;
        conformances = other.conformances;
        impls = other.impls;
        guard_match_arms = other.guard_match_arms;
        routes = other.routes;
        type_shapes = other.type_shapes;
        owned_strings = other.owned_strings;
        has_package_decl = other.has_package_decl;
        package_span = other.package_span;
        package_name = other.package_name;
        return *this;
    }
};

}  // namespace rut
