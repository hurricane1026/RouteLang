#include "rut/compiler/analyze.h"

#include "rut/compiler/lexer.h"
#include "rut/compiler/parser.h"
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rut {

static u32 g_import_analysis_counter = 0;
static std::deque<std::string> g_stable_generated_names;

namespace {

static Str intern_generated_name(const std::string& value) {
    g_stable_generated_names.push_back(value);
    const auto& kept = g_stable_generated_names.back();
    return {kept.c_str(), static_cast<u32>(kept.size())};
}

static std::string str_to_std_string(Str s) {
    return std::string(s.ptr, s.len);
}

static Str stash_owned_string(std::deque<std::string>& store, const std::string& value) {
    store.push_back(value);
    const auto& kept = store.back();
    return {kept.c_str(), static_cast<u32>(kept.size())};
}

static bool type_shape_is_simple_importable(const HirTypeKind type,
                                            u32 variant_index,
                                            u32 struct_index,
                                            u32 tuple_len,
                                            const HirTypeKind* tuple_types,
                                            const u32* tuple_variant_indices,
                                            const u32* tuple_struct_indices) {
    switch (type) {
        case HirTypeKind::Bool:
        case HirTypeKind::I32:
        case HirTypeKind::Str:
        case HirTypeKind::Generic:
        case HirTypeKind::Unknown:
            return true;
        case HirTypeKind::Struct:
            return struct_index != 0xffffffffu;
        case HirTypeKind::Variant:
            return variant_index != 0xffffffffu;
        case HirTypeKind::Tuple:
            for (u32 i = 0; i < tuple_len; i++) {
                if (tuple_types[i] == HirTypeKind::Struct && tuple_struct_indices[i] != 0xffffffffu)
                    continue;
                if (tuple_types[i] == HirTypeKind::Variant &&
                    tuple_variant_indices[i] != 0xffffffffu)
                    continue;
                if (!type_shape_is_simple_importable(tuple_types[i],
                                                     tuple_variant_indices[i],
                                                     tuple_struct_indices[i],
                                                     0,
                                                     nullptr,
                                                     nullptr,
                                                     nullptr))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

static bool read_text_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out = std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

static bool same_type_shape_node(const HirTypeShape& lhs, const HirTypeShape& rhs) {
    if (lhs.type != rhs.type) return false;
    if (lhs.generic_index != rhs.generic_index) return false;
    if (lhs.variant_index != rhs.variant_index) return false;
    if (lhs.struct_index != rhs.struct_index) return false;
    if (lhs.tuple_len != rhs.tuple_len) return false;
    for (u32 i = 0; i < lhs.tuple_len; i++) {
        if (lhs.tuple_elem_shape_indices[i] != rhs.tuple_elem_shape_indices[i]) return false;
    }
    return true;
}

static FrontendResult<u32> intern_hir_type_shape(HirModule* mod,
                                                 HirTypeKind type,
                                                 u32 generic_index,
                                                 u32 variant_index,
                                                 u32 struct_index,
                                                 u32 tuple_len,
                                                 const HirTypeKind* tuple_types,
                                                 const u32* tuple_variant_indices,
                                                 const u32* tuple_struct_indices,
                                                 Span span) {
    HirTypeShape shape{};
    shape.type = type;
    shape.generic_index = generic_index;
    shape.variant_index = variant_index;
    shape.struct_index = struct_index;
    shape.tuple_len = tuple_len;
    shape.is_concrete =
        type == HirTypeKind::Bool || type == HirTypeKind::I32 || type == HirTypeKind::Str;
    if (type == HirTypeKind::Variant) shape.is_concrete = variant_index != 0xffffffffu;
    if (type == HirTypeKind::Struct) shape.is_concrete = struct_index != 0xffffffffu;
    if (type == HirTypeKind::Tuple) {
        shape.is_concrete = true;
        for (u32 i = 0; i < tuple_len; i++) {
            const auto elem_type = tuple_types[i];
            const u32 elem_variant_index =
                elem_type == HirTypeKind::Variant ? tuple_variant_indices[i] : 0xffffffffu;
            const u32 elem_struct_index =
                elem_type == HirTypeKind::Struct ? tuple_struct_indices[i] : 0xffffffffu;
            auto elem_shape = intern_hir_type_shape(mod,
                                                    elem_type,
                                                    0xffffffffu,
                                                    elem_variant_index,
                                                    elem_struct_index,
                                                    0,
                                                    nullptr,
                                                    nullptr,
                                                    nullptr,
                                                    span);
            if (!elem_shape) return core::make_unexpected(elem_shape.error());
            shape.tuple_elem_shape_indices[i] = elem_shape.value();
            if (!mod->type_shapes[elem_shape.value()].is_concrete) shape.is_concrete = false;
        }
    }
    for (u32 i = 0; i < mod->type_shapes.len; i++) {
        if (same_type_shape_node(mod->type_shapes[i], shape)) return i;
    }
    if (!mod->type_shapes.push(shape)) return frontend_error(FrontendError::TooManyItems, span);
    return mod->type_shapes.len - 1;
}

struct ImportedModuleInfo {
    static constexpr u32 kMaxSelectedNames = AstImportDecl::kMaxSelectedNames;
    Span span{};
    Str path{};
    HirModule* module = nullptr;
    bool selective = false;
    bool has_namespace_alias = false;
    Str namespace_alias{};
    FixedVec<AstImportDecl::SelectedName, kMaxSelectedNames> selected_names;
};

enum class KnownValueState : u8 {
    Available,
    Nil,
    Error,
    Unknown,
};

struct MatchPayloadBinding {
    Str name{};
    HirTypeKind type = HirTypeKind::Unknown;
    u32 generic_index = 0xffffffffu;
    bool generic_has_error_constraint = false;
    bool generic_has_eq_constraint = false;
    bool generic_has_ord_constraint = false;
    u32 generic_protocol_index = 0xffffffffu;
    u32 generic_protocol_count = 0;
    u32 generic_protocol_indices[HirExpr::kMaxGenericProtocols]{};
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 shape_index = 0xffffffffu;
    u32 case_index = 0;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
    const HirExpr* subject = nullptr;
};

struct RouteNamedErrorCase {
    Str name{};
};

struct ConstValue {
    HirTypeKind type = HirTypeKind::Unknown;
    bool bool_value = false;
    i32 int_value = 0;
    u32 variant_index = 0;
    u32 case_index = 0;
};

struct KnownErrorCase {
    bool known = false;
    u32 variant_index = 0xffffffffu;
    u32 case_index = 0xffffffffu;
};

struct GenericBinding {
    bool bound = false;
    HirTypeKind type = HirTypeKind::Unknown;
    u32 generic_index = 0xffffffffu;
    bool generic_has_error_constraint = false;
    bool generic_has_eq_constraint = false;
    bool generic_has_ord_constraint = false;
    u32 generic_protocol_index = 0xffffffffu;
    u32 generic_protocol_count = 0;
    u32 generic_protocol_indices[HirExpr::kMaxGenericProtocols]{};
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 shape_index = 0xffffffffu;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
};

static FrontendResult<u32> instantiate_struct(HirModule* mod,
                                              u32 template_struct_index,
                                              const GenericBinding* bindings,
                                              u32 binding_count,
                                              Span span);

static FrontendResult<HirTypeKind> instantiate_deferred_named_type(
    const HirModule& mod,
    HirTypeKind declared_kind,
    u32 template_variant_index,
    u32 template_struct_index,
    const HirVariant::TypeArgRef* type_args,
    u32 type_arg_count,
    const GenericBinding* bindings,
    u32 binding_count,
    u32& out_variant_index,
    u32& out_struct_index,
    Span span);

static FrontendResult<void> fill_bound_binding_from_type_metadata(GenericBinding* binding,
                                                                  HirModule* mod,
                                                                  HirTypeKind type,
                                                                  u32 generic_index,
                                                                  u32 variant_index,
                                                                  u32 struct_index,
                                                                  u32 tuple_len,
                                                                  const HirTypeKind* tuple_types,
                                                                  const u32* tuple_variant_indices,
                                                                  const u32* tuple_struct_indices,
                                                                  u32 shape_index,
                                                                  Span span);

static bool conformance_matches_binding(const HirModule& mod,
                                        const HirConformance& c,
                                        const GenericBinding& binding) {
    if (c.protocol_index == 0xffffffffu) return false;
    if (c.type != binding.type) return false;
    if (c.type == HirTypeKind::Struct) {
        if (!c.is_generic_template) return c.struct_index == binding.struct_index;
        if (binding.struct_index >= mod.structs.len) return false;
        return mod.structs[binding.struct_index].template_struct_index == c.struct_index;
    }
    return c.struct_index == 0xffffffffu;
}

static bool generic_binding_satisfies_custom_protocol(const HirModule& mod,
                                                      const GenericBinding& binding,
                                                      u32 protocol_index) {
    for (u32 i = 0; i < mod.conformances.len; i++) {
        const auto& c = mod.conformances[i];
        if (c.protocol_index != protocol_index) continue;
        if (conformance_matches_binding(mod, c, binding)) return true;
    }
    return false;
}

static bool generic_binding_satisfies_error_constraint(const HirModule& mod,
                                                       const GenericBinding& binding) {
    if (binding.type == HirTypeKind::Struct && binding.struct_index < mod.structs.len)
        return mod.structs[binding.struct_index].conforms_error;
    return false;
}

static bool hir_type_shape_satisfies_eq_constraint(const HirModule& mod,
                                                   HirTypeKind type,
                                                   u32 variant_index,
                                                   u32 struct_index,
                                                   u32 tuple_len,
                                                   const HirTypeKind* tuple_types,
                                                   const u32* tuple_variant_indices,
                                                   const u32* tuple_struct_indices,
                                                   bool* struct_visiting,
                                                   bool* variant_visiting) {
    switch (type) {
        case HirTypeKind::Bool:
        case HirTypeKind::I32:
        case HirTypeKind::Str:
            return true;
        case HirTypeKind::Tuple:
            for (u32 i = 0; i < tuple_len; i++) {
                if (!hir_type_shape_satisfies_eq_constraint(mod,
                                                            tuple_types[i],
                                                            tuple_variant_indices[i],
                                                            tuple_struct_indices[i],
                                                            0,
                                                            nullptr,
                                                            nullptr,
                                                            nullptr,
                                                            struct_visiting,
                                                            variant_visiting))
                    return false;
            }
            return true;
        case HirTypeKind::Struct: {
            if (struct_index >= mod.structs.len) return false;
            if (struct_visiting[struct_index]) return true;
            struct_visiting[struct_index] = true;
            const auto& st = mod.structs[struct_index];
            for (u32 i = 0; i < st.fields.len; i++) {
                const auto& field = st.fields[i];
                const bool ok = field.is_error_type ||
                                hir_type_shape_satisfies_eq_constraint(mod,
                                                                       field.type,
                                                                       field.variant_index,
                                                                       field.struct_index,
                                                                       field.tuple_len,
                                                                       field.tuple_types,
                                                                       field.tuple_variant_indices,
                                                                       field.tuple_struct_indices,
                                                                       struct_visiting,
                                                                       variant_visiting);
                if (!ok) {
                    struct_visiting[struct_index] = false;
                    return false;
                }
            }
            struct_visiting[struct_index] = false;
            return true;
        }
        case HirTypeKind::Variant: {
            if (variant_index >= mod.variants.len) return false;
            if (variant_visiting[variant_index]) return true;
            variant_visiting[variant_index] = true;
            const auto& variant = mod.variants[variant_index];
            for (u32 i = 0; i < variant.cases.len; i++) {
                const auto& c = variant.cases[i];
                if (!c.has_payload) continue;
                if (!hir_type_shape_satisfies_eq_constraint(mod,
                                                            c.payload_type,
                                                            c.payload_variant_index,
                                                            c.payload_struct_index,
                                                            c.payload_tuple_len,
                                                            c.payload_tuple_types,
                                                            c.payload_tuple_variant_indices,
                                                            c.payload_tuple_struct_indices,
                                                            struct_visiting,
                                                            variant_visiting)) {
                    variant_visiting[variant_index] = false;
                    return false;
                }
            }
            variant_visiting[variant_index] = false;
            return true;
        }
        default:
            return false;
    }
}

static bool generic_binding_satisfies_eq_constraint(const HirModule& mod,
                                                    const GenericBinding& binding) {
    bool struct_visiting[HirModule::kMaxStructs]{};
    bool variant_visiting[HirModule::kMaxVariants]{};
    return hir_type_shape_satisfies_eq_constraint(mod,
                                                  binding.type,
                                                  binding.variant_index,
                                                  binding.struct_index,
                                                  binding.tuple_len,
                                                  binding.tuple_types,
                                                  binding.tuple_variant_indices,
                                                  binding.tuple_struct_indices,
                                                  struct_visiting,
                                                  variant_visiting);
}

static bool hir_type_shape_satisfies_ord_constraint(const HirModule& mod,
                                                    HirTypeKind type,
                                                    u32 variant_index,
                                                    u32 struct_index,
                                                    u32 tuple_len,
                                                    const HirTypeKind* tuple_types,
                                                    const u32* tuple_variant_indices,
                                                    const u32* tuple_struct_indices,
                                                    bool* struct_visiting,
                                                    bool* variant_visiting) {
    switch (type) {
        case HirTypeKind::I32:
        case HirTypeKind::Str:
            return true;
        case HirTypeKind::Tuple:
            for (u32 i = 0; i < tuple_len; i++) {
                if (!hir_type_shape_satisfies_ord_constraint(mod,
                                                             tuple_types[i],
                                                             tuple_variant_indices[i],
                                                             tuple_struct_indices[i],
                                                             0,
                                                             nullptr,
                                                             nullptr,
                                                             nullptr,
                                                             struct_visiting,
                                                             variant_visiting))
                    return false;
            }
            return true;
        case HirTypeKind::Struct: {
            if (struct_index >= mod.structs.len) return false;
            if (struct_visiting[struct_index]) return true;
            struct_visiting[struct_index] = true;
            const auto& st = mod.structs[struct_index];
            for (u32 i = 0; i < st.fields.len; i++) {
                const auto& field = st.fields[i];
                if (field.is_error_type) {
                    struct_visiting[struct_index] = false;
                    return false;
                }
                if (!hir_type_shape_satisfies_ord_constraint(mod,
                                                             field.type,
                                                             field.variant_index,
                                                             field.struct_index,
                                                             field.tuple_len,
                                                             field.tuple_types,
                                                             field.tuple_variant_indices,
                                                             field.tuple_struct_indices,
                                                             struct_visiting,
                                                             variant_visiting)) {
                    struct_visiting[struct_index] = false;
                    return false;
                }
            }
            struct_visiting[struct_index] = false;
            return true;
        }
        case HirTypeKind::Variant: {
            if (variant_index >= mod.variants.len) return false;
            if (variant_visiting[variant_index]) return true;
            variant_visiting[variant_index] = true;
            const auto& variant = mod.variants[variant_index];
            for (u32 i = 0; i < variant.cases.len; i++) {
                const auto& c = variant.cases[i];
                if (!c.has_payload) continue;
                if (!hir_type_shape_satisfies_ord_constraint(mod,
                                                             c.payload_type,
                                                             c.payload_variant_index,
                                                             c.payload_struct_index,
                                                             c.payload_tuple_len,
                                                             c.payload_tuple_types,
                                                             c.payload_tuple_variant_indices,
                                                             c.payload_tuple_struct_indices,
                                                             struct_visiting,
                                                             variant_visiting)) {
                    variant_visiting[variant_index] = false;
                    return false;
                }
            }
            variant_visiting[variant_index] = false;
            return true;
        }
        default:
            return false;
    }
}

static bool generic_binding_satisfies_ord_constraint(const HirModule& mod,
                                                     const GenericBinding& binding) {
    bool struct_visiting[HirModule::kMaxStructs]{};
    bool variant_visiting[HirModule::kMaxVariants]{};
    return hir_type_shape_satisfies_ord_constraint(mod,
                                                   binding.type,
                                                   binding.variant_index,
                                                   binding.struct_index,
                                                   binding.tuple_len,
                                                   binding.tuple_types,
                                                   binding.tuple_variant_indices,
                                                   binding.tuple_struct_indices,
                                                   struct_visiting,
                                                   variant_visiting);
}

static bool same_variant_instance_shape(const HirVariant& variant,
                                        u32 template_variant_index,
                                        const GenericBinding* bindings,
                                        u32 binding_count) {
    if (variant.template_variant_index != template_variant_index) return false;
    if (variant.instance_type_arg_count != binding_count) return false;
    for (u32 i = 0; i < binding_count; i++) {
        if (variant.instance_type_args[i] != bindings[i].type) return false;
        if (variant.instance_generic_indices[i] != bindings[i].generic_index) return false;
        if (variant.instance_variant_indices[i] != bindings[i].variant_index) return false;
        if (variant.instance_struct_indices[i] != bindings[i].struct_index) return false;
        if (variant.instance_shape_indices[i] != 0xffffffffu &&
            bindings[i].shape_index != 0xffffffffu &&
            variant.instance_shape_indices[i] != bindings[i].shape_index)
            return false;
        if (variant.instance_tuple_lens[i] != bindings[i].tuple_len) return false;
        for (u32 ti = 0; ti < bindings[i].tuple_len; ti++) {
            if (variant.instance_tuple_types[i][ti] != bindings[i].tuple_types[ti]) return false;
            if (variant.instance_tuple_variant_indices[i][ti] !=
                bindings[i].tuple_variant_indices[ti])
                return false;
            if (variant.instance_tuple_struct_indices[i][ti] !=
                bindings[i].tuple_struct_indices[ti])
                return false;
        }
    }
    return true;
}

static FrontendResult<u32> instantiate_variant(HirModule* mod,
                                               u32 template_variant_index,
                                               const GenericBinding* bindings,
                                               u32 binding_count,
                                               Span span) {
    for (u32 i = 0; i < mod->variants.len; i++) {
        if (same_variant_instance_shape(
                mod->variants[i], template_variant_index, bindings, binding_count))
            return i;
    }
    if (template_variant_index >= mod->variants.len)
        return frontend_error(FrontendError::UnsupportedSyntax, span);
    const auto& templ = mod->variants[template_variant_index];
    HirVariant concrete{};
    concrete.span = templ.span;
    concrete.name = templ.name;
    concrete.template_variant_index = template_variant_index;
    concrete.instance_type_arg_count = binding_count;
    for (u32 i = 0; i < binding_count; i++) {
        concrete.instance_type_args[i] = bindings[i].type;
        concrete.instance_generic_indices[i] = bindings[i].generic_index;
        concrete.instance_variant_indices[i] = bindings[i].variant_index;
        concrete.instance_struct_indices[i] = bindings[i].struct_index;
        concrete.instance_shape_indices[i] = bindings[i].shape_index;
        concrete.instance_tuple_lens[i] = bindings[i].tuple_len;
        for (u32 ti = 0; ti < bindings[i].tuple_len; ti++) {
            concrete.instance_tuple_types[i][ti] = bindings[i].tuple_types[ti];
            concrete.instance_tuple_variant_indices[i][ti] = bindings[i].tuple_variant_indices[ti];
            concrete.instance_tuple_struct_indices[i][ti] = bindings[i].tuple_struct_indices[ti];
        }
    }
    for (u32 ci = 0; ci < templ.cases.len; ci++) {
        HirVariant::CaseDecl case_decl = templ.cases[ci];
        if (case_decl.payload_template_variant_index != 0xffffffffu ||
            case_decl.payload_template_struct_index != 0xffffffffu) {
            u32 concrete_variant_index = 0xffffffffu;
            u32 concrete_struct_index = 0xffffffffu;
            auto concrete_kind =
                instantiate_deferred_named_type(*mod,
                                                case_decl.payload_type,
                                                case_decl.payload_template_variant_index,
                                                case_decl.payload_template_struct_index,
                                                case_decl.payload_type_args,
                                                case_decl.payload_type_arg_count,
                                                bindings,
                                                binding_count,
                                                concrete_variant_index,
                                                concrete_struct_index,
                                                span);
            if (!concrete_kind) return core::make_unexpected(concrete_kind.error());
            case_decl.payload_type = concrete_kind.value();
            case_decl.payload_variant_index = concrete_variant_index;
            case_decl.payload_struct_index = concrete_struct_index;
            case_decl.payload_template_variant_index = 0xffffffffu;
            case_decl.payload_template_struct_index = 0xffffffffu;
            case_decl.payload_type_arg_count = 0;
        }
        if (case_decl.payload_type == HirTypeKind::Generic) {
            if (case_decl.payload_generic_index >= binding_count)
                return frontend_error(FrontendError::UnsupportedSyntax, span);
            const u32 source_generic_index = case_decl.payload_generic_index;
            case_decl.payload_type = bindings[source_generic_index].type;
            case_decl.payload_generic_index =
                bindings[source_generic_index].type == HirTypeKind::Generic
                    ? bindings[source_generic_index].generic_index
                    : 0xffffffffu;
            case_decl.payload_generic_has_error_constraint =
                bindings[source_generic_index].generic_has_error_constraint;
            case_decl.payload_generic_has_eq_constraint =
                bindings[source_generic_index].generic_has_eq_constraint;
            case_decl.payload_generic_has_ord_constraint =
                bindings[source_generic_index].generic_has_ord_constraint;
            case_decl.payload_generic_protocol_index =
                bindings[source_generic_index].generic_protocol_index;
            case_decl.payload_generic_protocol_count =
                bindings[source_generic_index].generic_protocol_count;
            for (u32 cpi = 0; cpi < case_decl.payload_generic_protocol_count; cpi++) {
                case_decl.payload_generic_protocol_indices[cpi] =
                    bindings[source_generic_index].generic_protocol_indices[cpi];
            }
            case_decl.payload_variant_index = bindings[source_generic_index].variant_index;
            case_decl.payload_struct_index = bindings[source_generic_index].struct_index;
            case_decl.payload_tuple_len = bindings[source_generic_index].tuple_len;
            for (u32 ti = 0; ti < case_decl.payload_tuple_len; ti++) {
                case_decl.payload_tuple_types[ti] = bindings[source_generic_index].tuple_types[ti];
                case_decl.payload_tuple_variant_indices[ti] =
                    bindings[source_generic_index].tuple_variant_indices[ti];
                case_decl.payload_tuple_struct_indices[ti] =
                    bindings[source_generic_index].tuple_struct_indices[ti];
            }
        }
        if (case_decl.has_payload) {
            auto payload_shape = intern_hir_type_shape(mod,
                                                       case_decl.payload_type,
                                                       case_decl.payload_generic_index,
                                                       case_decl.payload_variant_index,
                                                       case_decl.payload_struct_index,
                                                       case_decl.payload_tuple_len,
                                                       case_decl.payload_tuple_types,
                                                       case_decl.payload_tuple_variant_indices,
                                                       case_decl.payload_tuple_struct_indices,
                                                       span);
            if (!payload_shape) return core::make_unexpected(payload_shape.error());
            case_decl.payload_shape_index = payload_shape.value();
        }
        if (!concrete.cases.push(case_decl))
            return frontend_error(FrontendError::TooManyItems, span);
    }
    if (!mod->variants.push(concrete)) return frontend_error(FrontendError::TooManyItems, span);
    return mod->variants.len - 1;
}

static FrontendResult<HirTypeKind> instantiate_deferred_named_type(
    const HirModule& mod,
    HirTypeKind declared_kind,
    u32 template_variant_index,
    u32 template_struct_index,
    const HirVariant::TypeArgRef* type_args,
    u32 type_arg_count,
    const GenericBinding* bindings,
    u32 binding_count,
    u32& out_variant_index,
    u32& out_struct_index,
    Span span) {
    out_variant_index = 0xffffffffu;
    out_struct_index = 0xffffffffu;
    GenericBinding concrete_args[HirFunction::kMaxTypeParams]{};
    for (u32 i = 0; i < type_arg_count; i++) {
        if (type_args[i].generic_index != 0xffffffffu) {
            if (type_args[i].generic_index >= binding_count)
                return frontend_error(FrontendError::UnsupportedSyntax, span);
            concrete_args[i] = bindings[type_args[i].generic_index];
            continue;
        }
        auto filled = fill_bound_binding_from_type_metadata(&concrete_args[i],
                                                            const_cast<HirModule*>(&mod),
                                                            type_args[i].type,
                                                            0xffffffffu,
                                                            type_args[i].variant_index,
                                                            type_args[i].struct_index,
                                                            type_args[i].tuple_len,
                                                            type_args[i].tuple_types,
                                                            type_args[i].tuple_variant_indices,
                                                            type_args[i].tuple_struct_indices,
                                                            type_args[i].shape_index,
                                                            span);
        if (!filled) return core::make_unexpected(filled.error());
    }
    if (declared_kind == HirTypeKind::Variant) {
        auto concrete_index = instantiate_variant(const_cast<HirModule*>(&mod),
                                                  template_variant_index,
                                                  concrete_args,
                                                  type_arg_count,
                                                  span);
        if (!concrete_index) return core::make_unexpected(concrete_index.error());
        out_variant_index = concrete_index.value();
        return HirTypeKind::Variant;
    }
    if (declared_kind == HirTypeKind::Struct) {
        auto concrete_index = instantiate_struct(const_cast<HirModule*>(&mod),
                                                 template_struct_index,
                                                 concrete_args,
                                                 type_arg_count,
                                                 span);
        if (!concrete_index) return core::make_unexpected(concrete_index.error());
        out_struct_index = concrete_index.value();
        return HirTypeKind::Struct;
    }
    return frontend_error(FrontendError::UnsupportedSyntax, span);
}

static bool same_struct_instance_shape(const HirStruct& st,
                                       u32 template_struct_index,
                                       const GenericBinding* bindings,
                                       u32 binding_count) {
    if (st.template_struct_index != template_struct_index) return false;
    if (st.instance_type_arg_count != binding_count) return false;
    for (u32 i = 0; i < binding_count; i++) {
        if (st.instance_type_args[i] != bindings[i].type) return false;
        if (st.instance_generic_indices[i] != bindings[i].generic_index) return false;
        if (st.instance_variant_indices[i] != bindings[i].variant_index) return false;
        if (st.instance_struct_indices[i] != bindings[i].struct_index) return false;
        if (st.instance_shape_indices[i] != 0xffffffffu && bindings[i].shape_index != 0xffffffffu &&
            st.instance_shape_indices[i] != bindings[i].shape_index)
            return false;
        if (st.instance_tuple_lens[i] != bindings[i].tuple_len) return false;
        for (u32 ti = 0; ti < bindings[i].tuple_len; ti++) {
            if (st.instance_tuple_types[i][ti] != bindings[i].tuple_types[ti]) return false;
            if (st.instance_tuple_variant_indices[i][ti] != bindings[i].tuple_variant_indices[ti])
                return false;
            if (st.instance_tuple_struct_indices[i][ti] != bindings[i].tuple_struct_indices[ti])
                return false;
        }
    }
    return true;
}

static FrontendResult<u32> instantiate_struct(HirModule* mod,
                                              u32 template_struct_index,
                                              const GenericBinding* bindings,
                                              u32 binding_count,
                                              Span span) {
    for (u32 i = 0; i < mod->structs.len; i++) {
        if (same_struct_instance_shape(
                mod->structs[i], template_struct_index, bindings, binding_count))
            return i;
    }
    if (template_struct_index >= mod->structs.len)
        return frontend_error(FrontendError::UnsupportedSyntax, span);
    const auto& templ = mod->structs[template_struct_index];
    HirStruct concrete{};
    concrete.span = templ.span;
    concrete.name = templ.name;
    concrete.conforms_error = templ.conforms_error;
    concrete.template_struct_index = template_struct_index;
    concrete.instance_type_arg_count = binding_count;
    for (u32 i = 0; i < binding_count; i++) {
        concrete.instance_type_args[i] = bindings[i].type;
        concrete.instance_generic_indices[i] = bindings[i].generic_index;
        concrete.instance_variant_indices[i] = bindings[i].variant_index;
        concrete.instance_struct_indices[i] = bindings[i].struct_index;
        concrete.instance_shape_indices[i] = bindings[i].shape_index;
        concrete.instance_tuple_lens[i] = bindings[i].tuple_len;
        for (u32 ti = 0; ti < bindings[i].tuple_len; ti++) {
            concrete.instance_tuple_types[i][ti] = bindings[i].tuple_types[ti];
            concrete.instance_tuple_variant_indices[i][ti] = bindings[i].tuple_variant_indices[ti];
            concrete.instance_tuple_struct_indices[i][ti] = bindings[i].tuple_struct_indices[ti];
        }
    }
    for (u32 fi = 0; fi < templ.fields.len; fi++) {
        HirStruct::FieldDecl field = templ.fields[fi];
        if (field.template_variant_index != 0xffffffffu ||
            field.template_struct_index != 0xffffffffu) {
            u32 concrete_variant_index = 0xffffffffu;
            u32 concrete_struct_index = 0xffffffffu;
            auto concrete_kind = instantiate_deferred_named_type(*mod,
                                                                 field.type,
                                                                 field.template_variant_index,
                                                                 field.template_struct_index,
                                                                 field.type_args,
                                                                 field.type_arg_count,
                                                                 bindings,
                                                                 binding_count,
                                                                 concrete_variant_index,
                                                                 concrete_struct_index,
                                                                 span);
            if (!concrete_kind) return core::make_unexpected(concrete_kind.error());
            field.type = concrete_kind.value();
            field.variant_index = concrete_variant_index;
            field.struct_index = concrete_struct_index;
            field.template_variant_index = 0xffffffffu;
            field.template_struct_index = 0xffffffffu;
            field.type_arg_count = 0;
        }
        if (field.type == HirTypeKind::Generic) {
            if (field.generic_index >= binding_count)
                return frontend_error(FrontendError::UnsupportedSyntax, span);
            const u32 source_generic_index = field.generic_index;
            field.type = bindings[source_generic_index].type;
            field.generic_index = bindings[source_generic_index].type == HirTypeKind::Generic
                                      ? bindings[source_generic_index].generic_index
                                      : 0xffffffffu;
            field.generic_has_error_constraint =
                bindings[source_generic_index].generic_has_error_constraint;
            field.generic_has_eq_constraint =
                bindings[source_generic_index].generic_has_eq_constraint;
            field.generic_has_ord_constraint =
                bindings[source_generic_index].generic_has_ord_constraint;
            field.generic_protocol_index = bindings[source_generic_index].generic_protocol_index;
            field.generic_protocol_count = bindings[source_generic_index].generic_protocol_count;
            for (u32 cpi = 0; cpi < field.generic_protocol_count; cpi++)
                field.generic_protocol_indices[cpi] =
                    bindings[source_generic_index].generic_protocol_indices[cpi];
            field.variant_index = bindings[source_generic_index].variant_index;
            field.struct_index = bindings[source_generic_index].struct_index;
            field.tuple_len = bindings[source_generic_index].tuple_len;
            for (u32 ti = 0; ti < field.tuple_len; ti++) {
                field.tuple_types[ti] = bindings[source_generic_index].tuple_types[ti];
                field.tuple_variant_indices[ti] =
                    bindings[source_generic_index].tuple_variant_indices[ti];
                field.tuple_struct_indices[ti] =
                    bindings[source_generic_index].tuple_struct_indices[ti];
            }
        }
        auto field_shape = intern_hir_type_shape(mod,
                                                 field.type,
                                                 field.generic_index,
                                                 field.variant_index,
                                                 field.struct_index,
                                                 field.tuple_len,
                                                 field.tuple_types,
                                                 field.tuple_variant_indices,
                                                 field.tuple_struct_indices,
                                                 span);
        if (!field_shape) return core::make_unexpected(field_shape.error());
        field.shape_index = field_shape.value();
        if (!concrete.fields.push(field)) return frontend_error(FrontendError::TooManyItems, span);
    }
    if (!mod->structs.push(concrete)) return frontend_error(FrontendError::TooManyItems, span);
    return mod->structs.len - 1;
}

static FrontendResult<void> refresh_concrete_struct_instances(HirModule* mod) {
    for (u32 si = 0; si < mod->structs.len; si++) {
        if (mod->structs[si].template_struct_index == 0xffffffffu) continue;
        const u32 templ_index = mod->structs[si].template_struct_index;
        if (templ_index >= mod->structs.len)
            return frontend_error(FrontendError::UnsupportedSyntax, mod->structs[si].span);
        const auto& templ = mod->structs[templ_index];
        HirStruct refreshed = mod->structs[si];
        refreshed.fields.len = 0;
        GenericBinding bindings[HirFunction::kMaxTypeParams]{};
        const u32 binding_count = refreshed.instance_type_arg_count;
        for (u32 bi = 0; bi < binding_count; bi++) {
            auto filled =
                fill_bound_binding_from_type_metadata(&bindings[bi],
                                                      mod,
                                                      refreshed.instance_type_args[bi],
                                                      refreshed.instance_generic_indices[bi],
                                                      refreshed.instance_variant_indices[bi],
                                                      refreshed.instance_struct_indices[bi],
                                                      refreshed.instance_tuple_lens[bi],
                                                      refreshed.instance_tuple_types[bi],
                                                      refreshed.instance_tuple_variant_indices[bi],
                                                      refreshed.instance_tuple_struct_indices[bi],
                                                      refreshed.instance_shape_indices[bi],
                                                      refreshed.span);
            if (!filled) return core::make_unexpected(filled.error());
            refreshed.instance_shape_indices[bi] = bindings[bi].shape_index;
        }
        for (u32 fi = 0; fi < templ.fields.len; fi++) {
            HirStruct::FieldDecl field = templ.fields[fi];
            if (field.template_variant_index != 0xffffffffu ||
                field.template_struct_index != 0xffffffffu) {
                u32 concrete_variant_index = 0xffffffffu;
                u32 concrete_struct_index = 0xffffffffu;
                auto concrete_kind = instantiate_deferred_named_type(*mod,
                                                                     field.type,
                                                                     field.template_variant_index,
                                                                     field.template_struct_index,
                                                                     field.type_args,
                                                                     field.type_arg_count,
                                                                     bindings,
                                                                     binding_count,
                                                                     concrete_variant_index,
                                                                     concrete_struct_index,
                                                                     refreshed.span);
                if (!concrete_kind) return core::make_unexpected(concrete_kind.error());
                field.type = concrete_kind.value();
                field.variant_index = concrete_variant_index;
                field.struct_index = concrete_struct_index;
                field.template_variant_index = 0xffffffffu;
                field.template_struct_index = 0xffffffffu;
                field.type_arg_count = 0;
            }
            if (field.type == HirTypeKind::Generic) {
                if (field.generic_index >= binding_count)
                    return frontend_error(FrontendError::UnsupportedSyntax, refreshed.span);
                field.type = bindings[field.generic_index].type;
                field.generic_has_error_constraint =
                    bindings[field.generic_index].generic_has_error_constraint;
                field.generic_has_eq_constraint =
                    bindings[field.generic_index].generic_has_eq_constraint;
                field.generic_has_ord_constraint =
                    bindings[field.generic_index].generic_has_ord_constraint;
                field.generic_protocol_index = bindings[field.generic_index].generic_protocol_index;
                field.generic_protocol_count = bindings[field.generic_index].generic_protocol_count;
                for (u32 cpi = 0; cpi < field.generic_protocol_count; cpi++)
                    field.generic_protocol_indices[cpi] =
                        bindings[field.generic_index].generic_protocol_indices[cpi];
                field.variant_index = bindings[field.generic_index].variant_index;
                field.struct_index = bindings[field.generic_index].struct_index;
                field.tuple_len = bindings[field.generic_index].tuple_len;
                for (u32 ti = 0; ti < field.tuple_len; ti++) {
                    field.tuple_types[ti] = bindings[field.generic_index].tuple_types[ti];
                    field.tuple_variant_indices[ti] =
                        bindings[field.generic_index].tuple_variant_indices[ti];
                    field.tuple_struct_indices[ti] =
                        bindings[field.generic_index].tuple_struct_indices[ti];
                }
                field.generic_index = 0xffffffffu;
            }
            auto field_shape = intern_hir_type_shape(mod,
                                                     field.type,
                                                     field.generic_index,
                                                     field.variant_index,
                                                     field.struct_index,
                                                     field.tuple_len,
                                                     field.tuple_types,
                                                     field.tuple_variant_indices,
                                                     field.tuple_struct_indices,
                                                     refreshed.span);
            if (!field_shape) return core::make_unexpected(field_shape.error());
            field.shape_index = field_shape.value();
            if (!refreshed.fields.push(field))
                return frontend_error(FrontendError::TooManyItems, refreshed.span);
        }
        refreshed.conforms_error = false;
        for (u32 fi = 0; fi < refreshed.fields.len; fi++) {
            if (refreshed.fields[fi].name.eq({"err", 3}) && refreshed.fields[fi].is_error_type) {
                refreshed.conforms_error = true;
                break;
            }
        }
        mod->structs[si] = refreshed;
    }
    return {};
}

static void copy_binding_constraints_from_type_params(
    GenericBinding* binding,
    const FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>& type_params) {
    if (binding->type != HirTypeKind::Generic) return;
    if (binding->generic_index >= type_params.len) return;
    const auto& type_param = type_params[binding->generic_index];
    binding->generic_has_error_constraint = type_param.has_error_constraint;
    binding->generic_has_eq_constraint = type_param.has_eq_constraint;
    binding->generic_has_ord_constraint = type_param.has_ord_constraint;
    binding->generic_protocol_index =
        type_param.custom_protocol_count != 0 ? type_param.custom_protocol_indices[0] : 0xffffffffu;
    binding->generic_protocol_count = type_param.custom_protocol_count;
    for (u32 cpi = 0; cpi < binding->generic_protocol_count; cpi++)
        binding->generic_protocol_indices[cpi] = type_param.custom_protocol_indices[cpi];
}

static FrontendResult<void> refresh_concrete_variant_instances(HirModule* mod) {
    for (u32 vi = 0; vi < mod->variants.len; vi++) {
        if (mod->variants[vi].template_variant_index == 0xffffffffu) continue;
        const u32 templ_index = mod->variants[vi].template_variant_index;
        if (templ_index >= mod->variants.len)
            return frontend_error(FrontendError::UnsupportedSyntax, mod->variants[vi].span);
        const auto& templ = mod->variants[templ_index];
        HirVariant refreshed = mod->variants[vi];
        refreshed.cases.len = 0;
        GenericBinding bindings[HirFunction::kMaxTypeParams]{};
        const u32 binding_count = refreshed.instance_type_arg_count;
        for (u32 bi = 0; bi < binding_count; bi++) {
            auto filled =
                fill_bound_binding_from_type_metadata(&bindings[bi],
                                                      mod,
                                                      refreshed.instance_type_args[bi],
                                                      refreshed.instance_generic_indices[bi],
                                                      refreshed.instance_variant_indices[bi],
                                                      refreshed.instance_struct_indices[bi],
                                                      refreshed.instance_tuple_lens[bi],
                                                      refreshed.instance_tuple_types[bi],
                                                      refreshed.instance_tuple_variant_indices[bi],
                                                      refreshed.instance_tuple_struct_indices[bi],
                                                      refreshed.instance_shape_indices[bi],
                                                      refreshed.span);
            if (!filled) return core::make_unexpected(filled.error());
            refreshed.instance_shape_indices[bi] = bindings[bi].shape_index;
        }
        for (u32 ci = 0; ci < templ.cases.len; ci++) {
            HirVariant::CaseDecl case_decl = templ.cases[ci];
            if (case_decl.payload_template_variant_index != 0xffffffffu ||
                case_decl.payload_template_struct_index != 0xffffffffu) {
                u32 concrete_variant_index = 0xffffffffu;
                u32 concrete_struct_index = 0xffffffffu;
                auto concrete_kind =
                    instantiate_deferred_named_type(*mod,
                                                    case_decl.payload_type,
                                                    case_decl.payload_template_variant_index,
                                                    case_decl.payload_template_struct_index,
                                                    case_decl.payload_type_args,
                                                    case_decl.payload_type_arg_count,
                                                    bindings,
                                                    binding_count,
                                                    concrete_variant_index,
                                                    concrete_struct_index,
                                                    refreshed.span);
                if (!concrete_kind) return core::make_unexpected(concrete_kind.error());
                case_decl.payload_type = concrete_kind.value();
                case_decl.payload_variant_index = concrete_variant_index;
                case_decl.payload_struct_index = concrete_struct_index;
                case_decl.payload_template_variant_index = 0xffffffffu;
                case_decl.payload_template_struct_index = 0xffffffffu;
                case_decl.payload_type_arg_count = 0;
            }
            if (case_decl.payload_type == HirTypeKind::Generic) {
                if (case_decl.payload_generic_index >= binding_count)
                    return frontend_error(FrontendError::UnsupportedSyntax, refreshed.span);
                const u32 source_generic_index = case_decl.payload_generic_index;
                case_decl.payload_type = bindings[source_generic_index].type;
                case_decl.payload_generic_index =
                    bindings[source_generic_index].type == HirTypeKind::Generic
                        ? bindings[source_generic_index].generic_index
                        : 0xffffffffu;
                case_decl.payload_generic_has_error_constraint =
                    bindings[source_generic_index].generic_has_error_constraint;
                case_decl.payload_generic_has_eq_constraint =
                    bindings[source_generic_index].generic_has_eq_constraint;
                case_decl.payload_generic_has_ord_constraint =
                    bindings[source_generic_index].generic_has_ord_constraint;
                case_decl.payload_generic_protocol_index =
                    bindings[source_generic_index].generic_protocol_index;
                case_decl.payload_generic_protocol_count =
                    bindings[source_generic_index].generic_protocol_count;
                for (u32 cpi = 0; cpi < case_decl.payload_generic_protocol_count; cpi++) {
                    case_decl.payload_generic_protocol_indices[cpi] =
                        bindings[source_generic_index].generic_protocol_indices[cpi];
                }
                case_decl.payload_variant_index = bindings[source_generic_index].variant_index;
                case_decl.payload_struct_index = bindings[source_generic_index].struct_index;
                case_decl.payload_tuple_len = bindings[source_generic_index].tuple_len;
                for (u32 ti = 0; ti < case_decl.payload_tuple_len; ti++) {
                    case_decl.payload_tuple_types[ti] =
                        bindings[source_generic_index].tuple_types[ti];
                    case_decl.payload_tuple_variant_indices[ti] =
                        bindings[source_generic_index].tuple_variant_indices[ti];
                    case_decl.payload_tuple_struct_indices[ti] =
                        bindings[source_generic_index].tuple_struct_indices[ti];
                }
            }
            if (case_decl.has_payload) {
                auto payload_shape = intern_hir_type_shape(mod,
                                                           case_decl.payload_type,
                                                           case_decl.payload_generic_index,
                                                           case_decl.payload_variant_index,
                                                           case_decl.payload_struct_index,
                                                           case_decl.payload_tuple_len,
                                                           case_decl.payload_tuple_types,
                                                           case_decl.payload_tuple_variant_indices,
                                                           case_decl.payload_tuple_struct_indices,
                                                           refreshed.span);
                if (!payload_shape) return core::make_unexpected(payload_shape.error());
                case_decl.payload_shape_index = payload_shape.value();
            }
            if (!refreshed.cases.push(case_decl))
                return frontend_error(FrontendError::TooManyItems, refreshed.span);
        }
        mod->variants[vi] = refreshed;
    }
    return {};
}

static u32 find_variant_index(const HirModule& mod, Str name) {
    for (u32 i = 0; i < mod.variants.len; i++) {
        if (mod.variants[i].name.eq(name)) return i;
    }
    return mod.variants.len;
}

static u32 find_struct_index(const HirModule& mod, Str name) {
    for (u32 i = 0; i < mod.structs.len; i++) {
        if (mod.structs[i].name.eq(name)) return i;
    }
    return mod.structs.len;
}

static u32 find_function_index(const HirModule& mod, Str name) {
    for (u32 i = 0; i < mod.functions.len; i++) {
        if (mod.functions[i].name.eq(name)) return i;
    }
    return mod.functions.len;
}

// Validates a function's signature for use as a route decorator (middleware).
// Rules:
//   - At least one parameter (the implicit Request slot).
//   - First parameter declared with `_ name: Type` (Swift omitted-label).
//     Strict from day one — relaxing later is non-breaking, tightening is.
//   - Return type must be i32. The runtime convention: 0 = pass through to
//     the next pre-middleware/handler, non-zero = reject and short-circuit
//     return that value as the HTTP status code.
//
// Deliberately does NOT check that the first param's *type* is `Request` —
// HirTypeKind has no Request yet (only Bool/I32/Str/Variant/Struct/Tuple/
// Generic/Unknown). Extend when the runtime Request type lands.
static FrontendResult<u32> validate_decorator_signature(const HirFunction& fn,
                                                        Span decorator_span) {
    if (fn.params.len == 0)
        return frontend_error(FrontendError::UnsupportedSyntax, decorator_span, fn.name);
    if (!fn.params[0].has_underscore_label)
        return frontend_error(FrontendError::UnsupportedSyntax, decorator_span, fn.params[0].name);
    if (fn.return_type != HirTypeKind::I32)
        return frontend_error(FrontendError::UnsupportedSyntax, decorator_span, fn.name);
    return 0u;
}

static const HirAlias* find_alias(const HirModule& mod, Str name) {
    for (u32 i = 0; i < mod.aliases.len; i++) {
        if (mod.aliases[i].name.eq(name)) return &mod.aliases[i];
    }
    return nullptr;
}

static Str resolve_alias_target_leaf(const HirModule& mod, Str name) {
    const auto* alias = find_alias(mod, name);
    if (alias == nullptr || alias->target_parts.len == 0) return name;
    return alias->target_parts[alias->target_parts.len - 1];
}

static Str import_namespace_name(Str path, bool has_alias, Str alias) {
    if (has_alias) return alias;
    const char* ptr = path.ptr;
    u32 len = path.len;
    u32 start = 0;
    for (u32 i = 0; i < len; i++) {
        if (ptr[i] == '/' || ptr[i] == '\\') start = i + 1;
    }
    u32 end = len;
    if (end >= 4 && ptr[end - 4] == '.' && ptr[end - 3] == 'r' && ptr[end - 2] == 'u' &&
        ptr[end - 1] == 't')
        end -= 4;
    if (end < start) return {};
    return {ptr + start, end - start};
}

static bool import_namespace_matches(Str path, bool has_alias, Str alias, Str ns) {
    const auto name = import_namespace_name(path, has_alias, alias);
    return name.eq(ns);
}

static bool has_import_namespace(const HirModule& mod, Str ns) {
    for (u32 i = 0; i < mod.imports.len; i++) {
        if (!mod.imports[i].selective &&
            import_namespace_matches(mod.imports[i].path,
                                     mod.imports[i].has_namespace_alias,
                                     mod.imports[i].namespace_alias,
                                     ns))
            return true;
    }
    return false;
}

static FrontendResult<void> validate_import_namespaces(
    const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    for (u32 i = 0; i < imports.len; i++) {
        if (imports[i].selective) continue;
        const auto lhs = import_namespace_name(
            imports[i].path, imports[i].has_namespace_alias, imports[i].namespace_alias);
        for (u32 j = i + 1; j < imports.len; j++) {
            if (imports[j].selective) continue;
            const auto rhs = import_namespace_name(
                imports[j].path, imports[j].has_namespace_alias, imports[j].namespace_alias);
            if (!imports[i].path.eq(imports[j].path) && lhs.eq(rhs))
                return frontend_error(FrontendError::UnsupportedSyntax, imports[j].span, rhs);
        }
    }
    return {};
}

static FrontendResult<void> validate_import_namespace_bindings(
    const AstFile& file, const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    for (u32 ii = 0; ii < imports.len; ii++) {
        if (imports[ii].selective) continue;
        const auto ns = import_namespace_name(
            imports[ii].path, imports[ii].has_namespace_alias, imports[ii].namespace_alias);
        for (u32 i = 0; i < file.items.len; i++) {
            const auto& item = file.items[i];
            switch (item.kind) {
                case AstItemKind::Func:
                    if (item.func.name.eq(ns))
                        return frontend_error(FrontendError::UnsupportedSyntax, item.func.span, ns);
                    break;
                case AstItemKind::Struct:
                    if (item.struct_decl.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, item.struct_decl.span, ns);
                    break;
                case AstItemKind::Variant:
                    if (item.variant.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, item.variant.span, ns);
                    break;
                case AstItemKind::Protocol:
                    if (item.protocol.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, item.protocol.span, ns);
                    break;
                case AstItemKind::Using:
                    if (item.using_decl.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, item.using_decl.span, ns);
                    break;
                default:
                    break;
            }
        }
        for (u32 jj = 0; jj < imports.len; jj++) {
            if (ii == jj) continue;
            const auto& other = imports[jj];
            if (!other.selective) {
                if (other.has_namespace_alias) continue;
                if (import_namespace_name(
                        other.path, other.has_namespace_alias, other.namespace_alias)
                        .eq(ns))
                    return frontend_error(FrontendError::UnsupportedSyntax, imports[ii].span, ns);
                for (u32 fi = 0; fi < other.module->functions.len; fi++) {
                    const auto& fn = other.module->functions[fi];
                    const bool hidden_runtime_fn =
                        (fn.name.len >= 7 && __builtin_memcmp(fn.name.ptr, "__impl_", 7) == 0) ||
                        (fn.name.len >= 8 && __builtin_memcmp(fn.name.ptr, "__proto_", 8) == 0);
                    if (!hidden_runtime_fn && fn.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, imports[ii].span, ns);
                }
                for (u32 pi = 0; pi < other.module->protocols.len; pi++) {
                    const auto& proto = other.module->protocols[pi];
                    if (proto.kind == HirProtocolKind::Custom && proto.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, imports[ii].span, ns);
                }
                for (u32 si = 0; si < other.module->structs.len; si++) {
                    const auto& st = other.module->structs[si];
                    if (st.template_struct_index == 0xffffffffu && st.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, imports[ii].span, ns);
                }
                for (u32 vi = 0; vi < other.module->variants.len; vi++) {
                    const auto& variant = other.module->variants[vi];
                    if (variant.template_variant_index == 0xffffffffu && variant.name.eq(ns))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, imports[ii].span, ns);
                }
                continue;
            }
            for (u32 si = 0; si < other.selected_names.len; si++) {
                const auto visible = other.selected_names[si].has_alias
                                         ? other.selected_names[si].alias
                                         : other.selected_names[si].name;
                if (visible.eq(ns))
                    return frontend_error(FrontendError::UnsupportedSyntax, imports[ii].span, ns);
            }
        }
    }
    return {};
}

static bool resolve_import_namespace_member(const HirModule& mod,
                                            const AstExpr& expr,
                                            Str& out_member) {
    if (expr.kind != AstExprKind::Field || expr.lhs == nullptr) return false;
    if (expr.lhs->kind != AstExprKind::Ident) return false;
    for (u32 i = 0; i < mod.imports.len; i++) {
        const auto& imported = mod.imports[i];
        if (imported.selective) continue;
        if (!import_namespace_matches(imported.path,
                                      imported.has_namespace_alias,
                                      imported.namespace_alias,
                                      expr.lhs->name))
            continue;
        if (imported.has_namespace_alias)
            out_member = intern_generated_name(str_to_std_string(imported.namespace_alias) + "__" +
                                               str_to_std_string(expr.name));
        else
            out_member = expr.name;
        return true;
    }
    return false;
}

static bool resolve_import_namespace_type_name(const HirModule& mod,
                                               Str ns,
                                               Str member,
                                               Str& out_name) {
    for (u32 i = 0; i < mod.imports.len; i++) {
        const auto& imported = mod.imports[i];
        if (imported.selective) continue;
        if (!import_namespace_matches(
                imported.path, imported.has_namespace_alias, imported.namespace_alias, ns))
            continue;
        if (imported.has_namespace_alias)
            out_name = intern_generated_name(str_to_std_string(imported.namespace_alias) + "__" +
                                             str_to_std_string(member));
        else
            out_name = member;
        return true;
    }
    return false;
}

static u32 find_protocol_index(const HirModule& mod, Str name) {
    for (u32 i = 0; i < mod.protocols.len; i++) {
        if (mod.protocols[i].name.eq(name)) return i;
    }
    return mod.protocols.len;
}

static const HirProtocol::MethodDecl* find_protocol_method(const HirProtocol& proto, Str name) {
    for (u32 i = 0; i < proto.methods.len; i++) {
        if (proto.methods[i].name.eq(name)) return &proto.methods[i];
    }
    return nullptr;
}

static HirProtocol::MethodDecl* find_protocol_method_mut(HirProtocol& proto, Str name) {
    for (u32 i = 0; i < proto.methods.len; i++) {
        if (proto.methods[i].name.eq(name)) return &proto.methods[i];
    }
    return nullptr;
}

static bool impl_matches_type(const HirModule& mod,
                              const HirImpl& impl,
                              HirTypeKind type,
                              u32 struct_index) {
    if (impl.type != type) return false;
    if (type == HirTypeKind::Struct) {
        if (!impl.is_generic_template) return impl.struct_index == struct_index;
        if (struct_index >= mod.structs.len) return false;
        return mod.structs[struct_index].template_struct_index == impl.struct_index;
    }
    return impl.struct_index == 0xffffffffu;
}

static FrontendResult<bool> fill_impl_generic_bindings(const HirModule& mod,
                                                       const HirImpl& impl,
                                                       u32 concrete_struct_index,
                                                       GenericBinding* out,
                                                       u32& out_count) {
    out_count = 0;
    if (!impl.is_generic_template) return true;
    if (concrete_struct_index >= mod.structs.len) return false;
    const auto& st = mod.structs[concrete_struct_index];
    if (st.template_struct_index != impl.struct_index) return false;
    out_count = st.instance_type_arg_count;
    for (u32 i = 0; i < st.instance_type_arg_count; i++) {
        auto filled = fill_bound_binding_from_type_metadata(&out[i],
                                                            const_cast<HirModule*>(&mod),
                                                            st.instance_type_args[i],
                                                            st.instance_generic_indices[i],
                                                            st.instance_variant_indices[i],
                                                            st.instance_struct_indices[i],
                                                            st.instance_tuple_lens[i],
                                                            st.instance_tuple_types[i],
                                                            st.instance_tuple_variant_indices[i],
                                                            st.instance_tuple_struct_indices[i],
                                                            st.instance_shape_indices[i],
                                                            impl.span);
        if (!filled) return core::make_unexpected(filled.error());
    }
    return true;
}

static const HirImpl* find_impl_for_type(const HirModule& mod,
                                         HirTypeKind type,
                                         u32 struct_index,
                                         Str method_name) {
    const HirImpl* match = nullptr;
    for (u32 i = 0; i < mod.impls.len; i++) {
        const auto& impl = mod.impls[i];
        if (!impl_matches_type(mod, impl, type, struct_index)) continue;
        bool has_method = false;
        for (u32 mi = 0; mi < impl.methods.len; mi++) {
            if (impl.methods[mi].name.eq(method_name)) {
                has_method = true;
                break;
            }
        }
        if (!has_method) continue;
        if (match != nullptr) return nullptr;
        match = &impl;
    }
    return match;
}

static const HirImplMethod* find_impl_method(const HirImpl& impl, Str name) {
    for (u32 i = 0; i < impl.methods.len; i++) {
        if (impl.methods[i].name.eq(name)) return &impl.methods[i];
    }
    return nullptr;
}

static FrontendResult<Str> make_protocol_default_function_name(Str protocol_name, Str method_name) {
    const char prefix[] = "__proto_";
    const u32 len = static_cast<u32>(sizeof(prefix) - 1) + protocol_name.len + 1 + method_name.len;
    auto* buf = new (std::nothrow) char[len];
    if (buf == nullptr) return frontend_error(FrontendError::OutOfMemory, {});
    u32 off = 0;
    for (u32 i = 0; i < sizeof(prefix) - 1; i++) buf[off++] = prefix[i];
    for (u32 i = 0; i < protocol_name.len; i++) buf[off++] = protocol_name.ptr[i];
    buf[off++] = '_';
    for (u32 i = 0; i < method_name.len; i++) buf[off++] = method_name.ptr[i];
    return Str{buf, len};
}

static bool impl_targets_overlap(const HirModule& mod,
                                 const HirImpl& impl,
                                 HirTypeKind type,
                                 u32 struct_index,
                                 bool is_generic_template) {
    if (impl.type != type) return false;
    if (type != HirTypeKind::Struct) return impl.struct_index == 0xffffffffu;
    if (impl.is_generic_template && is_generic_template) return impl.struct_index == struct_index;
    if (!impl.is_generic_template && !is_generic_template) return impl.struct_index == struct_index;
    if (impl.is_generic_template) {
        if (struct_index >= mod.structs.len) return false;
        return mod.structs[struct_index].template_struct_index == impl.struct_index;
    }
    if (impl.struct_index >= mod.structs.len) return false;
    return mod.structs[impl.struct_index].template_struct_index == struct_index;
}

static bool impl_matches_exact_target(const HirImpl& impl,
                                      HirTypeKind type,
                                      u32 struct_index,
                                      bool is_generic_template) {
    return impl.type == type && impl.struct_index == struct_index &&
           impl.is_generic_template == is_generic_template;
}

static FrontendResult<Str> make_impl_function_name(Str protocol_name,
                                                   Str type_name,
                                                   Str method_name) {
    const char prefix[] = "__impl_";
    const u32 len = static_cast<u32>(sizeof(prefix) - 1) + protocol_name.len + 1 + type_name.len +
                    1 + method_name.len;
    auto* buf = new (std::nothrow) char[len];
    if (buf == nullptr) return frontend_error(FrontendError::OutOfMemory, {});
    u32 off = 0;
    for (u32 i = 0; i < sizeof(prefix) - 1; i++) buf[off++] = prefix[i];
    for (u32 i = 0; i < protocol_name.len; i++) buf[off++] = protocol_name.ptr[i];
    buf[off++] = '_';
    for (u32 i = 0; i < type_name.len; i++) buf[off++] = type_name.ptr[i];
    buf[off++] = '_';
    for (u32 i = 0; i < method_name.len; i++) buf[off++] = method_name.ptr[i];
    return Str{buf, len};
}

static FrontendResult<Str> make_impl_target_name(const AstTypeRef& ref) {
    std::function<std::string(const AstTypeRef&)> build =
        [&](const AstTypeRef& cur) -> std::string {
        if (cur.is_tuple) {
            std::string out = "tuple";
            for (u32 i = 0; i < cur.tuple_elem_types.len; i++) {
                out += "_";
                if (cur.tuple_elem_types[i] != nullptr)
                    out += build(*cur.tuple_elem_types[i]);
                else
                    out += str_to_std_string(cur.tuple_elem_names[i]);
            }
            return out;
        }
        std::string out;
        if (cur.namespace_name.len != 0) {
            out += str_to_std_string(cur.namespace_name);
            out += "__";
        }
        out += str_to_std_string(cur.name);
        for (u32 i = 0; i < cur.type_arg_names.len; i++) {
            AstTypeRef arg{};
            if (i < cur.type_args.len && cur.type_args[i] != nullptr)
                arg = *cur.type_args[i];
            else {
                arg.name = cur.type_arg_names[i];
                if (i < cur.type_arg_namespaces.len)
                    arg.namespace_name = cur.type_arg_namespaces[i];
            }
            out += "_";
            out += build(arg);
        }
        return out;
    };
    return intern_generated_name(build(ref));
}

static HirProtocolKind resolve_protocol_kind(const HirModule& mod, Str name) {
    if (name.eq({"Error", 5})) return HirProtocolKind::Error;
    if (name.eq({"Eq", 2})) return HirProtocolKind::Eq;
    if (name.eq({"Ord", 3})) return HirProtocolKind::Ord;
    if (find_protocol_index(mod, name) < mod.protocols.len) return HirProtocolKind::Custom;
    return static_cast<HirProtocolKind>(0xff);
}

static u32 find_generic_param_index(
    const FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>& type_params,
    Str name) {
    for (u32 i = 0; i < type_params.len; i++) {
        if (type_params[i].name.eq(name)) return i;
    }
    return type_params.len;
}

static HirTypeKind resolve_named_type(const HirModule& mod,
                                      Str name,
                                      u32& variant_index,
                                      u32& struct_index) {
    variant_index = 0xffffffffu;
    struct_index = 0xffffffffu;
    if (name.eq({"bool", 4})) return HirTypeKind::Bool;
    if (name.eq({"i32", 3})) return HirTypeKind::I32;
    if (name.eq({"str", 3})) return HirTypeKind::Str;
    const u32 idx = find_variant_index(mod, name);
    if (idx < mod.variants.len) {
        variant_index = idx;
        return HirTypeKind::Variant;
    }
    const u32 struct_idx = find_struct_index(mod, name);
    if (struct_idx < mod.structs.len) {
        struct_index = struct_idx;
        return HirTypeKind::Struct;
    }
    return HirTypeKind::Unknown;
}

static AstTypeRef get_ast_type_arg_ref(const AstTypeRef& ref, u32 index) {
    if (index < ref.type_args.len && ref.type_args[index] != nullptr) return *ref.type_args[index];
    AstTypeRef out{};
    if (index < ref.type_arg_names.len) out.name = ref.type_arg_names[index];
    if (index < ref.type_arg_namespaces.len) out.namespace_name = ref.type_arg_namespaces[index];
    return out;
}

static AstTypeRef get_ast_tuple_elem_ref(const AstTypeRef& ref, u32 index) {
    if (index < ref.tuple_elem_types.len && ref.tuple_elem_types[index] != nullptr)
        return *ref.tuple_elem_types[index];
    AstTypeRef out{};
    if (index < ref.tuple_elem_names.len) out.name = ref.tuple_elem_names[index];
    return out;
}

static FrontendResult<HirTypeKind> resolve_func_type_ref(
    const HirModule& mod,
    const AstTypeRef& ref,
    const FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>* type_params,
    u32* generic_index,
    u32& variant_index,
    u32& struct_index,
    u32& tuple_len,
    HirTypeKind* tuple_types,
    u32* tuple_variant_indices,
    u32* tuple_struct_indices,
    Span span) {
    if (generic_index) *generic_index = 0xffffffffu;
    variant_index = 0xffffffffu;
    struct_index = 0xffffffffu;
    tuple_len = 0;
    if (!ref.is_tuple) {
        Str resolved_name = ref.name;
        if (ref.namespace_name.len != 0) {
            if (!resolve_import_namespace_type_name(
                    mod, ref.namespace_name, ref.name, resolved_name))
                return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
        }
        if (type_params != nullptr && ref.namespace_name.len == 0) {
            const u32 type_param_index = find_generic_param_index(*type_params, resolved_name);
            if (type_param_index < type_params->len) {
                if (ref.type_arg_names.len != 0)
                    return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
                if (generic_index) *generic_index = type_param_index;
                return HirTypeKind::Generic;
            }
        }
        const auto kind = resolve_named_type(mod, resolved_name, variant_index, struct_index);
        if (kind == HirTypeKind::Unknown)
            return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
        if (kind == HirTypeKind::Variant && variant_index < mod.variants.len &&
            mod.variants[variant_index].type_params.len != 0) {
            if (ref.type_arg_names.len != mod.variants[variant_index].type_params.len)
                return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
            GenericBinding bindings[HirVariant::kMaxTypeParams]{};
            for (u32 i = 0; i < ref.type_arg_names.len; i++) {
                u32 type_variant_index = 0xffffffffu;
                u32 type_struct_index = 0xffffffffu;
                u32 elem_tuple_len = 0;
                HirTypeKind elem_tuple_types[kMaxTupleSlots]{};
                u32 elem_tuple_variant_indices[kMaxTupleSlots]{};
                u32 elem_tuple_struct_indices[kMaxTupleSlots]{};
                AstTypeRef type_arg_ref = get_ast_type_arg_ref(ref, i);
                auto type_arg = resolve_func_type_ref(mod,
                                                      type_arg_ref,
                                                      type_params,
                                                      nullptr,
                                                      type_variant_index,
                                                      type_struct_index,
                                                      elem_tuple_len,
                                                      elem_tuple_types,
                                                      elem_tuple_variant_indices,
                                                      elem_tuple_struct_indices,
                                                      span);
                if (!type_arg) return core::make_unexpected(type_arg.error());
                if (type_arg.value() == HirTypeKind::Generic)
                    return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
                auto filled = fill_bound_binding_from_type_metadata(&bindings[i],
                                                                    const_cast<HirModule*>(&mod),
                                                                    type_arg.value(),
                                                                    0xffffffffu,
                                                                    type_variant_index,
                                                                    type_struct_index,
                                                                    elem_tuple_len,
                                                                    elem_tuple_types,
                                                                    elem_tuple_variant_indices,
                                                                    elem_tuple_struct_indices,
                                                                    0xffffffffu,
                                                                    span);
                if (!filled) return core::make_unexpected(filled.error());
            }
            auto concrete_index = instantiate_variant(const_cast<HirModule*>(&mod),
                                                      variant_index,
                                                      bindings,
                                                      ref.type_arg_names.len,
                                                      span);
            if (!concrete_index) return core::make_unexpected(concrete_index.error());
            variant_index = concrete_index.value();
            return HirTypeKind::Variant;
        }
        if (kind == HirTypeKind::Struct && struct_index < mod.structs.len &&
            mod.structs[struct_index].type_params.len != 0) {
            if (ref.type_arg_names.len != mod.structs[struct_index].type_params.len)
                return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
            GenericBinding bindings[HirStruct::kMaxTypeParams]{};
            for (u32 i = 0; i < ref.type_arg_names.len; i++) {
                u32 type_variant_index = 0xffffffffu;
                u32 type_struct_index = 0xffffffffu;
                u32 elem_tuple_len = 0;
                HirTypeKind elem_tuple_types[kMaxTupleSlots]{};
                u32 elem_tuple_variant_indices[kMaxTupleSlots]{};
                u32 elem_tuple_struct_indices[kMaxTupleSlots]{};
                AstTypeRef type_arg_ref = get_ast_type_arg_ref(ref, i);
                auto type_arg = resolve_func_type_ref(mod,
                                                      type_arg_ref,
                                                      type_params,
                                                      nullptr,
                                                      type_variant_index,
                                                      type_struct_index,
                                                      elem_tuple_len,
                                                      elem_tuple_types,
                                                      elem_tuple_variant_indices,
                                                      elem_tuple_struct_indices,
                                                      span);
                if (!type_arg) return core::make_unexpected(type_arg.error());
                if (type_arg.value() == HirTypeKind::Generic)
                    return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
                auto filled = fill_bound_binding_from_type_metadata(&bindings[i],
                                                                    const_cast<HirModule*>(&mod),
                                                                    type_arg.value(),
                                                                    0xffffffffu,
                                                                    type_variant_index,
                                                                    type_struct_index,
                                                                    elem_tuple_len,
                                                                    elem_tuple_types,
                                                                    elem_tuple_variant_indices,
                                                                    elem_tuple_struct_indices,
                                                                    0xffffffffu,
                                                                    span);
                if (!filled) return core::make_unexpected(filled.error());
            }
            auto concrete_index = instantiate_struct(
                const_cast<HirModule*>(&mod), struct_index, bindings, ref.type_arg_names.len, span);
            if (!concrete_index) return core::make_unexpected(concrete_index.error());
            struct_index = concrete_index.value();
            return HirTypeKind::Struct;
        }
        if (ref.type_arg_names.len != 0)
            return frontend_error(FrontendError::UnsupportedSyntax, span, ref.name);
        return kind;
    }
    if (ref.tuple_elem_names.len < 2 && ref.tuple_elem_types.len < 2)
        return frontend_error(FrontendError::UnsupportedSyntax, span);
    const u32 elem_count =
        ref.tuple_elem_types.len != 0 ? ref.tuple_elem_types.len : ref.tuple_elem_names.len;
    tuple_len = elem_count;
    for (u32 i = 0; i < elem_count; i++) {
        u32 elem_variant_index = 0xffffffffu;
        u32 elem_struct_index = 0xffffffffu;
        u32 elem_tuple_len = 0;
        HirTypeKind elem_tuple_types[kMaxTupleSlots]{};
        u32 elem_tuple_variant_indices[kMaxTupleSlots]{};
        u32 elem_tuple_struct_indices[kMaxTupleSlots]{};
        AstTypeRef elem_ref = get_ast_tuple_elem_ref(ref, i);
        auto kind = resolve_func_type_ref(mod,
                                          elem_ref,
                                          type_params,
                                          nullptr,
                                          elem_variant_index,
                                          elem_struct_index,
                                          elem_tuple_len,
                                          elem_tuple_types,
                                          elem_tuple_variant_indices,
                                          elem_tuple_struct_indices,
                                          span);
        if (!kind) return core::make_unexpected(kind.error());
        if (kind.value() == HirTypeKind::Generic || kind.value() == HirTypeKind::Tuple)
            return frontend_error(FrontendError::UnsupportedSyntax, span, elem_ref.name);
        tuple_types[i] = kind.value();
        tuple_variant_indices[i] =
            kind.value() == HirTypeKind::Variant ? elem_variant_index : 0xffffffffu;
        tuple_struct_indices[i] =
            kind.value() == HirTypeKind::Struct ? elem_struct_index : 0xffffffffu;
    }
    return HirTypeKind::Tuple;
}

static bool same_hir_type_shape(const HirExpr& lhs, const HirExpr& rhs) {
    if (lhs.type != rhs.type) return false;
    if (lhs.type == HirTypeKind::Generic) return lhs.generic_index == rhs.generic_index;
    if (lhs.type == HirTypeKind::Variant) return lhs.variant_index == rhs.variant_index;
    if (lhs.type == HirTypeKind::Struct) return lhs.struct_index == rhs.struct_index;
    if (lhs.type == HirTypeKind::Tuple) {
        if (lhs.tuple_len != rhs.tuple_len) return false;
        for (u32 i = 0; i < lhs.tuple_len; i++) {
            if (lhs.tuple_types[i] != rhs.tuple_types[i]) return false;
            if (lhs.tuple_types[i] == HirTypeKind::Variant &&
                lhs.tuple_variant_indices[i] != rhs.tuple_variant_indices[i])
                return false;
            if (lhs.tuple_types[i] == HirTypeKind::Struct &&
                lhs.tuple_struct_indices[i] != rhs.tuple_struct_indices[i])
                return false;
        }
    }
    return true;
}

static bool same_hir_shape_index(const HirModule& mod, u32 lhs_shape_index, u32 rhs_shape_index) {
    if (lhs_shape_index == rhs_shape_index) return true;
    if (lhs_shape_index == 0xffffffffu || rhs_shape_index == 0xffffffffu) return false;
    if (lhs_shape_index >= mod.type_shapes.len || rhs_shape_index >= mod.type_shapes.len)
        return false;
    const auto& lhs = mod.type_shapes[lhs_shape_index];
    const auto& rhs = mod.type_shapes[rhs_shape_index];
    if (lhs.type != rhs.type) return false;
    if (lhs.generic_index != rhs.generic_index) return false;
    if (lhs.variant_index != rhs.variant_index) return false;
    if (lhs.struct_index != rhs.struct_index) return false;
    if (lhs.tuple_len != rhs.tuple_len) return false;
    for (u32 i = 0; i < lhs.tuple_len; i++) {
        if (!same_hir_shape_index(
                mod, lhs.tuple_elem_shape_indices[i], rhs.tuple_elem_shape_indices[i])) {
            return false;
        }
    }
    return true;
}

static bool same_hir_type_shape(const HirModule& mod, const HirExpr& lhs, const HirExpr& rhs) {
    if (lhs.shape_index != 0xffffffffu && rhs.shape_index != 0xffffffffu)
        if (same_hir_shape_index(mod, lhs.shape_index, rhs.shape_index)) return true;
    return same_hir_type_shape(lhs, rhs);
}

static HirExpr make_expected_param_expr(const HirFunction::ParamDecl& param) {
    HirExpr expected{};
    expected.type = param.type;
    expected.generic_index = param.generic_index;
    expected.generic_has_error_constraint = param.generic_has_error_constraint;
    expected.generic_has_eq_constraint = param.generic_has_eq_constraint;
    expected.generic_has_ord_constraint = param.generic_has_ord_constraint;
    expected.generic_protocol_index = param.generic_protocol_index;
    expected.generic_protocol_count = param.generic_protocol_count;
    for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
        expected.generic_protocol_indices[cpi] = param.generic_protocol_indices[cpi];
    expected.variant_index = param.variant_index;
    expected.struct_index = param.struct_index;
    expected.shape_index = param.shape_index;
    expected.tuple_len = param.tuple_len;
    for (u32 i = 0; i < param.tuple_len; i++) {
        expected.tuple_types[i] = param.tuple_types[i];
        expected.tuple_variant_indices[i] = param.tuple_variant_indices[i];
        expected.tuple_struct_indices[i] = param.tuple_struct_indices[i];
    }
    return expected;
}

static FrontendResult<void> fill_bound_binding_from_type_metadata(GenericBinding* binding,
                                                                  HirModule* mod,
                                                                  HirTypeKind type,
                                                                  u32 generic_index,
                                                                  u32 variant_index,
                                                                  u32 struct_index,
                                                                  u32 tuple_len,
                                                                  const HirTypeKind* tuple_types,
                                                                  const u32* tuple_variant_indices,
                                                                  const u32* tuple_struct_indices,
                                                                  u32 shape_index,
                                                                  Span span) {
    binding->bound = true;
    binding->type = type;
    binding->generic_index = generic_index;
    binding->variant_index = variant_index;
    binding->struct_index = struct_index;
    binding->tuple_len = tuple_len;
    for (u32 ti = 0; ti < tuple_len; ti++) {
        binding->tuple_types[ti] = tuple_types[ti];
        binding->tuple_variant_indices[ti] = tuple_variant_indices[ti];
        binding->tuple_struct_indices[ti] = tuple_struct_indices[ti];
    }
    binding->shape_index = shape_index;
    if (binding->shape_index == 0xffffffffu) {
        auto computed_shape = intern_hir_type_shape(mod,
                                                    binding->type,
                                                    binding->generic_index,
                                                    binding->variant_index,
                                                    binding->struct_index,
                                                    binding->tuple_len,
                                                    binding->tuple_types,
                                                    binding->tuple_variant_indices,
                                                    binding->tuple_struct_indices,
                                                    span);
        if (!computed_shape) return core::make_unexpected(computed_shape.error());
        binding->shape_index = computed_shape.value();
    }
    return {};
}

static FrontendResult<void> concretize_named_instance_shape(HirExpr* expr,
                                                            const HirModule& mod,
                                                            const GenericBinding* generic_bindings,
                                                            u32 generic_binding_count) {
    if (expr->type == HirTypeKind::Variant && expr->variant_index < mod.variants.len) {
        const auto& variant = mod.variants[expr->variant_index];
        if (variant.template_variant_index != 0xffffffffu) {
            GenericBinding bindings[HirVariant::kMaxTypeParams]{};
            for (u32 i = 0; i < variant.instance_type_arg_count; i++) {
                if (variant.instance_type_args[i] == HirTypeKind::Generic) {
                    const u32 gi = variant.instance_generic_indices[i];
                    if (gi >= generic_binding_count || !generic_bindings[gi].bound)
                        return frontend_error(FrontendError::UnsupportedSyntax, expr->span);
                    bindings[i] = generic_bindings[gi];
                } else {
                    auto filled = fill_bound_binding_from_type_metadata(
                        &bindings[i],
                        const_cast<HirModule*>(&mod),
                        variant.instance_type_args[i],
                        variant.instance_generic_indices[i],
                        variant.instance_variant_indices[i],
                        variant.instance_struct_indices[i],
                        variant.instance_tuple_lens[i],
                        variant.instance_tuple_types[i],
                        variant.instance_tuple_variant_indices[i],
                        variant.instance_tuple_struct_indices[i],
                        variant.instance_shape_indices[i],
                        expr->span);
                    if (!filled) return core::make_unexpected(filled.error());
                }
            }
            auto concrete = instantiate_variant(const_cast<HirModule*>(&mod),
                                                variant.template_variant_index,
                                                bindings,
                                                variant.instance_type_arg_count,
                                                expr->span);
            if (!concrete) return core::make_unexpected(concrete.error());
            expr->variant_index = concrete.value();
        }
    }
    if (expr->type == HirTypeKind::Struct && expr->struct_index < mod.structs.len) {
        const auto& st = mod.structs[expr->struct_index];
        if (st.template_struct_index != 0xffffffffu) {
            GenericBinding bindings[HirStruct::kMaxTypeParams]{};
            for (u32 i = 0; i < st.instance_type_arg_count; i++) {
                if (st.instance_type_args[i] == HirTypeKind::Generic) {
                    const u32 gi = st.instance_generic_indices[i];
                    if (gi >= generic_binding_count || !generic_bindings[gi].bound)
                        return frontend_error(FrontendError::UnsupportedSyntax, expr->span);
                    bindings[i] = generic_bindings[gi];
                } else {
                    auto filled =
                        fill_bound_binding_from_type_metadata(&bindings[i],
                                                              const_cast<HirModule*>(&mod),
                                                              st.instance_type_args[i],
                                                              st.instance_generic_indices[i],
                                                              st.instance_variant_indices[i],
                                                              st.instance_struct_indices[i],
                                                              st.instance_tuple_lens[i],
                                                              st.instance_tuple_types[i],
                                                              st.instance_tuple_variant_indices[i],
                                                              st.instance_tuple_struct_indices[i],
                                                              st.instance_shape_indices[i],
                                                              expr->span);
                    if (!filled) return core::make_unexpected(filled.error());
                }
            }
            auto concrete = instantiate_struct(const_cast<HirModule*>(&mod),
                                               st.template_struct_index,
                                               bindings,
                                               st.instance_type_arg_count,
                                               expr->span);
            if (!concrete) return core::make_unexpected(concrete.error());
            expr->struct_index = concrete.value();
        }
    }
    if (expr->type != HirTypeKind::Unknown) {
        auto shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                           expr->type,
                                           expr->generic_index,
                                           expr->variant_index,
                                           expr->struct_index,
                                           expr->tuple_len,
                                           expr->tuple_types,
                                           expr->tuple_variant_indices,
                                           expr->tuple_struct_indices,
                                           expr->span);
        if (!shape) return core::make_unexpected(shape.error());
        expr->shape_index = shape.value();
    }
    return {};
}

static HirExpr make_expected_type_expr(HirTypeKind type,
                                       u32 variant_index,
                                       u32 struct_index,
                                       u32 tuple_len,
                                       const HirTypeKind* tuple_types,
                                       const u32* tuple_variant_indices,
                                       const u32* tuple_struct_indices,
                                       u32 shape_index) {
    HirExpr expected{};
    expected.type = type;
    expected.variant_index = variant_index;
    expected.struct_index = struct_index;
    expected.shape_index = shape_index;
    expected.tuple_len = tuple_len;
    for (u32 i = 0; i < tuple_len; i++) {
        expected.tuple_types[i] = tuple_types[i];
        expected.tuple_variant_indices[i] = tuple_variant_indices[i];
        expected.tuple_struct_indices[i] = tuple_struct_indices[i];
    }
    return expected;
}

static HirExpr make_instance_arg_expr(HirTypeKind type,
                                      u32 generic_index,
                                      u32 variant_index,
                                      u32 struct_index,
                                      u32 tuple_len,
                                      const HirTypeKind* tuple_types,
                                      const u32* tuple_variant_indices,
                                      const u32* tuple_struct_indices,
                                      u32 shape_index) {
    HirExpr out{};
    out.type = type;
    out.generic_index = generic_index;
    out.variant_index = variant_index;
    out.struct_index = struct_index;
    out.shape_index = shape_index;
    out.tuple_len = tuple_len;
    for (u32 i = 0; i < tuple_len; i++) {
        out.tuple_types[i] = tuple_types[i];
        out.tuple_variant_indices[i] = tuple_variant_indices[i];
        out.tuple_struct_indices[i] = tuple_struct_indices[i];
    }
    return out;
}

static HirExpr apply_generic_binding_to_expr(const HirExpr& expr,
                                             const GenericBinding* generic_bindings,
                                             u32 generic_binding_count) {
    HirExpr out = expr;
    if (expr.type == HirTypeKind::Generic && expr.generic_index < generic_binding_count &&
        generic_bindings[expr.generic_index].bound) {
        const auto& binding = generic_bindings[expr.generic_index];
        out.type = binding.type;
        out.generic_index = 0xffffffffu;
        out.generic_has_error_constraint = false;
        out.generic_has_eq_constraint = false;
        out.generic_has_ord_constraint = false;
        out.generic_protocol_index = 0xffffffffu;
        out.generic_protocol_count = 0;
        out.variant_index = binding.variant_index;
        out.struct_index = binding.struct_index;
        out.shape_index = binding.shape_index;
        out.tuple_len = binding.tuple_len;
        for (u32 i = 0; i < binding.tuple_len; i++) {
            out.tuple_types[i] = binding.tuple_types[i];
            out.tuple_variant_indices[i] = binding.tuple_variant_indices[i];
            out.tuple_struct_indices[i] = binding.tuple_struct_indices[i];
        }
    }
    return out;
}

static bool bind_generic_shape(GenericBinding* generic_bindings,
                               u32 generic_binding_count,
                               u32 generic_index,
                               const HirExpr& actual) {
    if (generic_index >= generic_binding_count) return false;
    auto& binding = generic_bindings[generic_index];
    if (!binding.bound) {
        binding.bound = true;
        binding.type = actual.type;
        binding.generic_index =
            actual.type == HirTypeKind::Generic ? actual.generic_index : 0xffffffffu;
        binding.generic_has_error_constraint = actual.generic_has_error_constraint;
        binding.generic_has_eq_constraint = actual.generic_has_eq_constraint;
        binding.generic_has_ord_constraint = actual.generic_has_ord_constraint;
        binding.generic_protocol_index = actual.generic_protocol_index;
        binding.generic_protocol_count = actual.generic_protocol_count;
        for (u32 cpi = 0; cpi < binding.generic_protocol_count; cpi++)
            binding.generic_protocol_indices[cpi] = actual.generic_protocol_indices[cpi];
        binding.variant_index =
            actual.type == HirTypeKind::Variant ? actual.variant_index : 0xffffffffu;
        binding.struct_index =
            actual.type == HirTypeKind::Struct ? actual.struct_index : 0xffffffffu;
        binding.shape_index = actual.shape_index;
        binding.tuple_len = actual.type == HirTypeKind::Tuple ? actual.tuple_len : 0;
        for (u32 i = 0; i < binding.tuple_len; i++) {
            binding.tuple_types[i] = actual.tuple_types[i];
            binding.tuple_variant_indices[i] = actual.tuple_variant_indices[i];
            binding.tuple_struct_indices[i] = actual.tuple_struct_indices[i];
        }
        return true;
    }
    HirExpr expected{};
    expected.type = binding.type;
    expected.generic_index = binding.generic_index;
    expected.generic_has_error_constraint = binding.generic_has_error_constraint;
    expected.generic_has_eq_constraint = binding.generic_has_eq_constraint;
    expected.generic_has_ord_constraint = binding.generic_has_ord_constraint;
    expected.generic_protocol_index = binding.generic_protocol_index;
    expected.generic_protocol_count = binding.generic_protocol_count;
    for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
        expected.generic_protocol_indices[cpi] = binding.generic_protocol_indices[cpi];
    expected.variant_index = binding.variant_index;
    expected.struct_index = binding.struct_index;
    expected.shape_index = binding.shape_index;
    expected.tuple_len = binding.tuple_len;
    for (u32 i = 0; i < binding.tuple_len; i++) {
        expected.tuple_types[i] = binding.tuple_types[i];
        expected.tuple_variant_indices[i] = binding.tuple_variant_indices[i];
        expected.tuple_struct_indices[i] = binding.tuple_struct_indices[i];
    }
    return same_hir_type_shape(actual, expected);
}

static bool bind_named_generic_shape(const HirModule& mod,
                                     GenericBinding* generic_bindings,
                                     u32 generic_binding_count,
                                     const HirExpr& expected,
                                     const HirExpr& actual) {
    if (expected.type == HirTypeKind::Generic)
        return bind_generic_shape(
            generic_bindings, generic_binding_count, expected.generic_index, actual);
    if (expected.type == HirTypeKind::Struct) {
        if (actual.type != HirTypeKind::Struct || expected.struct_index >= mod.structs.len ||
            actual.struct_index >= mod.structs.len)
            return false;
        const auto& est = mod.structs[expected.struct_index];
        const auto& act = mod.structs[actual.struct_index];
        if (est.template_struct_index == 0xffffffffu)
            return same_hir_type_shape(mod, actual, expected);
        if (act.template_struct_index != est.template_struct_index ||
            act.instance_type_arg_count != est.instance_type_arg_count)
            return false;
        for (u32 i = 0; i < est.instance_type_arg_count; i++) {
            const auto actual_arg = make_instance_arg_expr(act.instance_type_args[i],
                                                           act.instance_generic_indices[i],
                                                           act.instance_variant_indices[i],
                                                           act.instance_struct_indices[i],
                                                           act.instance_tuple_lens[i],
                                                           act.instance_tuple_types[i],
                                                           act.instance_tuple_variant_indices[i],
                                                           act.instance_tuple_struct_indices[i],
                                                           act.instance_shape_indices[i]);
            const auto expected_arg = make_instance_arg_expr(est.instance_type_args[i],
                                                             est.instance_generic_indices[i],
                                                             est.instance_variant_indices[i],
                                                             est.instance_struct_indices[i],
                                                             est.instance_tuple_lens[i],
                                                             est.instance_tuple_types[i],
                                                             est.instance_tuple_variant_indices[i],
                                                             est.instance_tuple_struct_indices[i],
                                                             est.instance_shape_indices[i]);
            if (!bind_named_generic_shape(
                    mod, generic_bindings, generic_binding_count, expected_arg, actual_arg))
                return false;
        }
        return true;
    }
    if (expected.type == HirTypeKind::Variant) {
        if (actual.type != HirTypeKind::Variant || expected.variant_index >= mod.variants.len ||
            actual.variant_index >= mod.variants.len)
            return false;
        const auto& est = mod.variants[expected.variant_index];
        const auto& act = mod.variants[actual.variant_index];
        if (est.template_variant_index == 0xffffffffu)
            return same_hir_type_shape(mod, actual, expected);
        if (act.template_variant_index != est.template_variant_index ||
            act.instance_type_arg_count != est.instance_type_arg_count)
            return false;
        for (u32 i = 0; i < est.instance_type_arg_count; i++) {
            const auto actual_arg = make_instance_arg_expr(act.instance_type_args[i],
                                                           act.instance_generic_indices[i],
                                                           act.instance_variant_indices[i],
                                                           act.instance_struct_indices[i],
                                                           act.instance_tuple_lens[i],
                                                           act.instance_tuple_types[i],
                                                           act.instance_tuple_variant_indices[i],
                                                           act.instance_tuple_struct_indices[i],
                                                           act.instance_shape_indices[i]);
            const auto expected_arg = make_instance_arg_expr(est.instance_type_args[i],
                                                             est.instance_generic_indices[i],
                                                             est.instance_variant_indices[i],
                                                             est.instance_struct_indices[i],
                                                             est.instance_tuple_lens[i],
                                                             est.instance_tuple_types[i],
                                                             est.instance_tuple_variant_indices[i],
                                                             est.instance_tuple_struct_indices[i],
                                                             est.instance_shape_indices[i]);
            if (!bind_named_generic_shape(
                    mod, generic_bindings, generic_binding_count, expected_arg, actual_arg))
                return false;
        }
        return true;
    }
    return same_hir_type_shape(mod, actual, expected);
}

static FrontendResult<void> apply_declared_type_to_expr(HirExpr* expr,
                                                        const HirModule& mod,
                                                        const AstStatement& stmt) {
    if (!stmt.has_type) return {};
    u32 variant_index = 0xffffffffu;
    u32 struct_index = 0xffffffffu;
    u32 tuple_len = 0;
    HirTypeKind tuple_types[kMaxTupleSlots]{};
    u32 tuple_variant_indices[kMaxTupleSlots]{};
    u32 tuple_struct_indices[kMaxTupleSlots]{};
    auto declared = resolve_func_type_ref(mod,
                                          stmt.type,
                                          nullptr,
                                          nullptr,
                                          variant_index,
                                          struct_index,
                                          tuple_len,
                                          tuple_types,
                                          tuple_variant_indices,
                                          tuple_struct_indices,
                                          stmt.span);
    if (!declared) return core::make_unexpected(declared.error());
    auto declared_shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                                declared.value(),
                                                0xffffffffu,
                                                variant_index,
                                                struct_index,
                                                tuple_len,
                                                tuple_types,
                                                tuple_variant_indices,
                                                tuple_struct_indices,
                                                stmt.span);
    if (!declared_shape) return core::make_unexpected(declared_shape.error());
    if (expr->type == HirTypeKind::Unknown && (expr->may_nil || expr->may_error)) {
        expr->type = declared.value();
        expr->variant_index = variant_index;
        expr->struct_index = struct_index;
        expr->shape_index = declared_shape.value();
        expr->tuple_len = tuple_len;
        for (u32 i = 0; i < tuple_len; i++) {
            expr->tuple_types[i] = tuple_types[i];
            expr->tuple_variant_indices[i] = tuple_variant_indices[i];
            expr->tuple_struct_indices[i] = tuple_struct_indices[i];
        }
    }
    const auto expected = make_expected_type_expr(declared.value(),
                                                  variant_index,
                                                  struct_index,
                                                  tuple_len,
                                                  tuple_types,
                                                  tuple_variant_indices,
                                                  tuple_struct_indices,
                                                  declared_shape.value());
    if (!same_hir_type_shape(mod, *expr, expected))
        return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
    return {};
}

static KnownValueState known_value_state(const HirExpr& expr,
                                         const HirLocal* locals,
                                         u32 local_count,
                                         u32 depth);
static bool const_eval_expr(
    const HirExpr& expr, const HirLocal* locals, u32 local_count, ConstValue* out, u32 depth);
static KnownErrorCase known_error_case(const HirExpr& expr,
                                       const HirLocal* locals,
                                       u32 local_count,
                                       u32 depth);
static Str known_error_name(const HirExpr& expr,
                            const HirLocal* locals,
                            u32 local_count,
                            u32 depth);
static const HirExpr* known_error_expr(const HirExpr& expr,
                                       const HirLocal* locals,
                                       u32 local_count,
                                       u32 depth);
static FrontendResult<HirExpr> analyze_expr(const AstExpr& expr,
                                            HirRoute* route,
                                            const HirModule& mod,
                                            const HirLocal* locals,
                                            u32 local_count,
                                            const MatchPayloadBinding* binding);
static FrontendResult<HirExpr> instantiate_function_expr(const HirExpr& expr,
                                                         HirRoute* route,
                                                         const HirModule& mod,
                                                         const HirExpr* args,
                                                         u32 arg_count,
                                                         const GenericBinding* generic_bindings,
                                                         u32 generic_binding_count);
static FrontendResult<HirExpr> normalize_function_expr(
    const HirExpr& expr, HirFunction* fn, const HirLocal* locals, u32 local_count, u32 param_count);
static FrontendResult<HirExpr> analyze_function_body_stmt(const AstStatement& stmt,
                                                          HirRoute* scratch,
                                                          const HirModule& mod,
                                                          const HirLocal* locals,
                                                          u32 local_count,
                                                          const MatchPayloadBinding* binding);
static FrontendResult<void> analyze_control_stmt(const AstStatement& stmt,
                                                 HirRoute* route,
                                                 const HirModule& mod,
                                                 const MatchPayloadBinding* binding);
static FrontendResult<void> collect_named_error_cases(
    const HirExpr& expr, FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases);
static FrontendResult<void> collect_named_error_cases_ast(
    const AstStatement& stmt, FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases);
static void patch_named_error_variant(
    HirExpr* expr,
    u32 variant_index,
    const FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases);
static FrontendResult<HirExpr> analyze_known_error_field(const HirExpr& base,
                                                         Str field_name,
                                                         Span span,
                                                         const HirModule& mod,
                                                         const HirLocal* locals,
                                                         u32 local_count,
                                                         u32 depth);
static FrontendResult<void> analyze_guard_fail_body(const AstStatement& stmt,
                                                    HirGuardBody* body,
                                                    HirRoute* route,
                                                    const HirModule& mod,
                                                    const HirLocal* locals,
                                                    u32 local_count,
                                                    const MatchPayloadBinding* binding);
static FrontendResult<HirExpr> analyze_guard_cond(const AstExpr& expr,
                                                  HirRoute* route,
                                                  const HirModule& mod,
                                                  const HirLocal* locals,
                                                  u32 local_count);
static bool is_standard_error_field(Str field_name);
static FrontendResult<HirExpr> analyze_call_expr(const AstExpr& expr,
                                                 HirRoute* route,
                                                 const HirModule& mod,
                                                 const HirLocal* locals,
                                                 u32 local_count,
                                                 const MatchPayloadBinding* binding,
                                                 const HirExpr* pipe_lhs);
static FrontendResult<HirExpr> analyze_method_call_expr(const AstExpr& expr,
                                                        HirRoute* route,
                                                        const HirModule& mod,
                                                        const HirLocal* locals,
                                                        u32 local_count,
                                                        const MatchPayloadBinding* binding) {
    if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
    if (expr.lhs->kind == AstExprKind::Ident && has_import_namespace(mod, expr.lhs->name)) {
        Str qualified_member{};
        AstExpr ns_field{};
        ns_field.kind = AstExprKind::Field;
        ns_field.lhs = expr.lhs;
        ns_field.name = expr.name;
        ns_field.span = expr.span;
        if (!resolve_import_namespace_member(mod, ns_field, qualified_member))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        AstExpr call_expr{};
        call_expr.kind = AstExprKind::Call;
        call_expr.span = expr.span;
        call_expr.name = qualified_member;
        call_expr.args = expr.args;
        return analyze_call_expr(call_expr, route, mod, locals, local_count, binding, nullptr);
    }
    Str qualified_type_name{};
    if (resolve_import_namespace_member(mod, *expr.lhs, qualified_type_name)) {
        AstExpr variant_expr{};
        variant_expr.kind = AstExprKind::VariantCase;
        variant_expr.span = expr.span;
        variant_expr.name = qualified_type_name;
        variant_expr.type_args = expr.lhs->type_args;
        variant_expr.str_value = expr.name;
        if (expr.args.len == 1)
            variant_expr.lhs = expr.args[0];
        else if (expr.args.len > 1)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto variant = analyze_expr(variant_expr, route, mod, locals, local_count, binding);
        if (variant) return variant;
    }
    if (expr.lhs->kind == AstExprKind::Ident) {
        AstExpr variant_expr{};
        variant_expr.kind = AstExprKind::VariantCase;
        variant_expr.span = expr.span;
        variant_expr.name = expr.lhs->name;
        variant_expr.type_args = expr.lhs->type_args;
        variant_expr.str_value = expr.name;
        if (expr.args.len == 1)
            variant_expr.lhs = expr.args[0];
        else if (expr.args.len > 1)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto variant = analyze_expr(variant_expr, route, mod, locals, local_count, binding);
        if (variant) return variant;
    }

    auto build_cmp = [&](HirExprKind kind,
                         const HirExpr& lhs_expr,
                         const HirExpr& rhs_expr) -> FrontendResult<HirExpr> {
        HirExpr out{};
        out.kind = kind;
        out.type = HirTypeKind::Bool;
        out.span = expr.span;
        if (!route->exprs.push(lhs_expr))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &route->exprs[route->exprs.len - 1];
        if (!route->exprs.push(rhs_expr))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.rhs = &route->exprs[route->exprs.len - 1];
        return out;
    };

    AstExpr field_expr{};
    field_expr.kind = AstExprKind::Field;
    field_expr.span = expr.span;
    field_expr.lhs = expr.lhs;
    field_expr.name = expr.name;

    auto recv = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
    if (!recv) return core::make_unexpected(recv.error());
    if (expr.args.len == 0 && is_standard_error_field(expr.name) &&
        (recv->may_error ||
         (recv->type == HirTypeKind::Generic && recv->generic_has_error_constraint))) {
        return analyze_expr(field_expr, route, mod, locals, local_count, binding);
    }
    if (recv->may_nil || recv->may_error)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span);

    const bool is_eq_family = expr.name.eq({"eq", 2}) || expr.name.eq({"ne", 2});
    const bool is_ord_family = expr.name.eq({"lt", 2}) || expr.name.eq({"gt", 2}) ||
                               expr.name.eq({"le", 2}) || expr.name.eq({"ge", 2});
    if (is_eq_family || is_ord_family) {
        if (expr.args.len != 1) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto rhs = analyze_expr(*expr.args[0], route, mod, locals, local_count, binding);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (rhs->may_nil || rhs->may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (!same_hir_type_shape(mod, *recv, *rhs))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (is_eq_family) {
            if (recv->type == HirTypeKind::Generic) {
                if (!recv->generic_has_eq_constraint)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            } else {
                bool struct_visiting[HirModule::kMaxStructs]{};
                bool variant_visiting[HirModule::kMaxVariants]{};
                if (!hir_type_shape_satisfies_eq_constraint(mod,
                                                            recv->type,
                                                            recv->variant_index,
                                                            recv->struct_index,
                                                            recv->tuple_len,
                                                            recv->tuple_types,
                                                            recv->tuple_variant_indices,
                                                            recv->tuple_struct_indices,
                                                            struct_visiting,
                                                            variant_visiting)) {
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
                }
            }
            auto eq_expr = build_cmp(HirExprKind::Eq, recv.value(), rhs.value());
            if (!eq_expr) return core::make_unexpected(eq_expr.error());
            if (expr.name.eq({"eq", 2})) return eq_expr;
            HirExpr false_expr{};
            false_expr.kind = HirExprKind::BoolLit;
            false_expr.type = HirTypeKind::Bool;
            false_expr.bool_value = false;
            false_expr.span = expr.span;
            return build_cmp(HirExprKind::Eq, eq_expr.value(), false_expr);
        } else {
            if (recv->type == HirTypeKind::Generic) {
                if (!recv->generic_has_ord_constraint)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            } else {
                bool struct_visiting[HirModule::kMaxStructs]{};
                bool variant_visiting[HirModule::kMaxVariants]{};
                if (!hir_type_shape_satisfies_ord_constraint(mod,
                                                             recv->type,
                                                             recv->variant_index,
                                                             recv->struct_index,
                                                             recv->tuple_len,
                                                             recv->tuple_types,
                                                             recv->tuple_variant_indices,
                                                             recv->tuple_struct_indices,
                                                             struct_visiting,
                                                             variant_visiting)) {
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
                }
            }
            if (expr.name.eq({"lt", 2}))
                return build_cmp(HirExprKind::Lt, recv.value(), rhs.value());
            if (expr.name.eq({"gt", 2}))
                return build_cmp(HirExprKind::Gt, recv.value(), rhs.value());
            auto strict = expr.name.eq({"le", 2})
                              ? build_cmp(HirExprKind::Gt, recv.value(), rhs.value())
                              : build_cmp(HirExprKind::Lt, recv.value(), rhs.value());
            if (!strict) return core::make_unexpected(strict.error());
            HirExpr false_expr{};
            false_expr.kind = HirExprKind::BoolLit;
            false_expr.type = HirTypeKind::Bool;
            false_expr.bool_value = false;
            false_expr.span = expr.span;
            return build_cmp(HirExprKind::Eq, strict.value(), false_expr);
        }
    }
    if (recv->type == HirTypeKind::Generic && recv->generic_index != 0xffffffffu) {
        u32 matched_protocol_index = 0xffffffffu;
        const HirProtocol::MethodDecl* matched_req = nullptr;
        for (u32 gi = 0; gi < recv->generic_protocol_count; gi++) {
            const u32 proto_index = recv->generic_protocol_indices[gi];
            if (proto_index >= mod.protocols.len) continue;
            const auto* req = find_protocol_method(mod.protocols[proto_index], expr.name);
            if (req == nullptr) continue;
            if (matched_protocol_index != 0xffffffffu)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            matched_protocol_index = proto_index;
            matched_req = req;
        }
        if (matched_protocol_index != 0xffffffffu && matched_req != nullptr) {
            HirExpr out{};
            out.kind = HirExprKind::ProtocolCall;
            out.span = expr.span;
            out.type = HirTypeKind::Unknown;
            out.protocol_index = matched_protocol_index;
            out.str_value = expr.name;
            if (!route->exprs.push(recv.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            out.lhs = &route->exprs[route->exprs.len - 1];
            for (u32 i = 0; i < expr.args.len; i++) {
                auto arg = analyze_expr(*expr.args[i], route, mod, locals, local_count, binding);
                if (!arg) return core::make_unexpected(arg.error());
                if (arg->may_nil || arg->may_error)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                if (!route->exprs.push(arg.value()))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
                if (!out.args.push(&route->exprs[route->exprs.len - 1]))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
            }
            if (matched_req->has_return_type) {
                out.type = matched_req->return_type;
                out.generic_index = matched_req->return_generic_index;
                out.variant_index = matched_req->return_variant_index;
                out.struct_index = matched_req->return_struct_index;
                out.shape_index = matched_req->return_shape_index;
                out.tuple_len = matched_req->return_tuple_len;
                for (u32 i = 0; i < matched_req->return_tuple_len; i++) {
                    out.tuple_types[i] = matched_req->return_tuple_types[i];
                    out.tuple_variant_indices[i] = matched_req->return_tuple_variant_indices[i];
                    out.tuple_struct_indices[i] = matched_req->return_tuple_struct_indices[i];
                }
                out.may_nil = matched_req->return_may_nil;
                out.may_error = matched_req->return_may_error;
                out.error_struct_index = matched_req->return_error_struct_index;
                out.error_variant_index = matched_req->return_error_variant_index;
            }
            return out;
        }
        for (u32 i = 0; i < mod.protocols.len; i++) {
            if (find_protocol_method(mod.protocols[i], expr.name) != nullptr)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        }
    }
    if (recv->type == HirTypeKind::Bool || recv->type == HirTypeKind::I32 ||
        recv->type == HirTypeKind::Str || recv->type == HirTypeKind::Struct ||
        recv->type == HirTypeKind::Variant || recv->type == HirTypeKind::Tuple) {
        const auto* impl = find_impl_for_type(mod, recv->type, recv->struct_index, expr.name);
        if (impl != nullptr) {
            const auto* method = find_impl_method(*impl, expr.name);
            if (method == nullptr || method->function_index >= mod.functions.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            AstExpr call_expr{};
            call_expr.kind = AstExprKind::Call;
            call_expr.span = expr.span;
            call_expr.name = mod.functions[method->function_index].name;
            if (!call_expr.args.push(expr.lhs))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            for (u32 i = 0; i < expr.args.len; i++) {
                if (!call_expr.args.push(expr.args[i]))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
            }
            return analyze_call_expr(call_expr, route, mod, locals, local_count, binding, nullptr);
        }
        u32 matched_protocol_index = 0xffffffffu;
        const HirProtocol::MethodDecl* matched_req = nullptr;
        for (u32 ci = 0; ci < mod.conformances.len; ci++) {
            const auto& conf = mod.conformances[ci];
            if (conf.type != recv->type) continue;
            if (recv->type == HirTypeKind::Struct && conf.struct_index != recv->struct_index)
                continue;
            if (recv->type == HirTypeKind::Variant && conf.variant_index != recv->variant_index)
                continue;
            if (recv->type == HirTypeKind::Tuple) {
                if (conf.tuple_len != recv->tuple_len) continue;
                bool tuple_match = true;
                for (u32 ti = 0; ti < recv->tuple_len; ti++) {
                    if (conf.tuple_types[ti] != recv->tuple_types[ti]) {
                        tuple_match = false;
                        break;
                    }
                    if (conf.tuple_types[ti] == HirTypeKind::Variant &&
                        conf.tuple_variant_indices[ti] != recv->tuple_variant_indices[ti]) {
                        tuple_match = false;
                        break;
                    }
                    if (conf.tuple_types[ti] == HirTypeKind::Struct &&
                        conf.tuple_struct_indices[ti] != recv->tuple_struct_indices[ti]) {
                        tuple_match = false;
                        break;
                    }
                }
                if (!tuple_match) continue;
            }
            if (conf.protocol_index >= mod.protocols.len) continue;
            const auto* req = find_protocol_method(mod.protocols[conf.protocol_index], expr.name);
            if (req == nullptr || req->function_index == 0xffffffffu) continue;
            if (matched_protocol_index != 0xffffffffu)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            matched_protocol_index = conf.protocol_index;
            matched_req = req;
        }
        if (matched_protocol_index != 0xffffffffu && matched_req != nullptr) {
            AstExpr call_expr{};
            call_expr.kind = AstExprKind::Call;
            call_expr.span = expr.span;
            call_expr.name = mod.functions[matched_req->function_index].name;
            if (!call_expr.args.push(expr.lhs))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            for (u32 i = 0; i < expr.args.len; i++) {
                if (!call_expr.args.push(expr.args[i]))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
            }
            return analyze_call_expr(call_expr, route, mod, locals, local_count, binding, nullptr);
        }
    }
    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
}

static FrontendResult<HirExpr> analyze_call_expr(const AstExpr& expr,
                                                 HirRoute* route,
                                                 const HirModule& mod,
                                                 const HirLocal* locals,
                                                 u32 local_count,
                                                 const MatchPayloadBinding* binding,
                                                 const HirExpr* pipe_lhs);

static FrontendResult<HirExpr> make_guard_bound_init(HirRoute* route,
                                                     const HirExpr& bound,
                                                     Span span) {
    if (!bound.may_error) return bound;
    if (!route->exprs.push(bound)) return frontend_error(FrontendError::TooManyItems, span);
    HirExpr init{};
    init.kind = HirExprKind::ValueOf;
    init.type = bound.type;
    init.generic_index = bound.generic_index;
    init.generic_has_error_constraint = bound.generic_has_error_constraint;
    init.generic_has_eq_constraint = bound.generic_has_eq_constraint;
    init.generic_has_ord_constraint = bound.generic_has_ord_constraint;
    init.generic_protocol_index = bound.generic_protocol_index;
    init.generic_protocol_count = bound.generic_protocol_count;
    for (u32 i = 0; i < init.generic_protocol_count; i++)
        init.generic_protocol_indices[i] = bound.generic_protocol_indices[i];
    init.shape_index = bound.shape_index;
    init.may_nil = bound.may_nil;
    init.may_error = false;
    init.variant_index = bound.variant_index;
    init.struct_index = bound.struct_index;
    init.tuple_len = bound.tuple_len;
    for (u32 i = 0; i < bound.tuple_len; i++) {
        init.tuple_types[i] = bound.tuple_types[i];
        init.tuple_variant_indices[i] = bound.tuple_variant_indices[i];
        init.tuple_struct_indices[i] = bound.tuple_struct_indices[i];
    }
    init.error_struct_index = bound.error_struct_index;
    init.error_variant_index = 0xffffffffu;
    init.span = span;
    init.lhs = &route->exprs[route->exprs.len - 1];
    return init;
}

static u32 next_local_ref_index(const HirRoute* route, const HirLocal* locals, u32 local_count) {
    u32 next = 0;
    for (u32 i = 0; i < local_count; i++) {
        if (locals[i].ref_index >= next) next = locals[i].ref_index + 1;
    }
    for (u32 i = 0; i < route->locals.len; i++) {
        if (route->locals[i].ref_index >= next) next = route->locals[i].ref_index + 1;
    }
    return next;
}

static FrontendResult<void> insert_scoped_local(
    HirLocal* locals, u32& local_count, u32 capacity, const HirLocal& local, Span span) {
    if (local.ref_index >= capacity) return frontend_error(FrontendError::TooManyItems, span);
    if (local_count <= local.ref_index) local_count = local.ref_index + 1;
    locals[local.ref_index] = local;
    return {};
}

static const HirStruct* error_struct_decl(const HirModule& mod, u32 struct_index) {
    if (struct_index >= mod.structs.len) return nullptr;
    return &mod.structs[struct_index];
}

static const HirStruct::FieldDecl* find_struct_field(const HirStruct& st, Str field_name) {
    for (u32 i = 0; i < st.fields.len; i++) {
        if (st.fields[i].name.eq(field_name)) return &st.fields[i];
    }
    return nullptr;
}

static const HirExpr* find_error_field_init(const HirExpr& expr, Str field_name) {
    for (u32 i = 0; i < expr.field_inits.len; i++) {
        if (expr.field_inits[i].name.eq(field_name)) return expr.field_inits[i].value;
    }
    return nullptr;
}

static FrontendResult<HirExpr> analyze_known_error_field(const HirExpr& base,
                                                         Str field_name,
                                                         Span span,
                                                         const HirModule& mod,
                                                         const HirLocal* locals,
                                                         u32 local_count,
                                                         u32 depth) {
    if (depth > 8) return frontend_error(FrontendError::UnsupportedSyntax, span);
    if (base.kind == HirExprKind::LocalRef) {
        if (base.local_index >= local_count)
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        return analyze_known_error_field(
            locals[base.local_index].init, field_name, span, mod, locals, local_count, depth + 1);
    }
    if (base.kind != HirExprKind::Error)
        return frontend_error(FrontendError::UnsupportedSyntax, span);

    if (field_name.eq({"code", 4})) {
        HirExpr out{};
        out.kind = HirExprKind::IntLit;
        out.type = HirTypeKind::I32;
        out.int_value = base.int_value;
        out.span = span;
        return out;
    }
    if (field_name.eq({"msg", 3})) {
        HirExpr out{};
        out.kind = HirExprKind::StrLit;
        out.type = HirTypeKind::Str;
        out.str_value = base.msg;
        out.span = span;
        return out;
    }
    if (field_name.eq({"line", 4})) {
        HirExpr out{};
        out.kind = HirExprKind::IntLit;
        out.type = HirTypeKind::I32;
        out.int_value = static_cast<i32>(base.span.line);
        out.span = span;
        return out;
    }
    if (field_name.eq({"file", 4}) || field_name.eq({"func", 4}))
        return frontend_error(FrontendError::UnsupportedSyntax, span);

    const HirStruct* st = error_struct_decl(mod, base.error_struct_index);
    if (!st) return frontend_error(FrontendError::UnsupportedSyntax, span);
    const HirExpr* field_init = find_error_field_init(base, field_name);
    if (!field_init) return frontend_error(FrontendError::UnsupportedSyntax, span, field_name);
    HirExpr out = *field_init;
    out.span = span;
    return out;
}

static bool is_standard_error_field(Str field_name) {
    return field_name.eq({"code", 4}) || field_name.eq({"msg", 3}) || field_name.eq({"file", 4}) ||
           field_name.eq({"func", 4}) || field_name.eq({"line", 4});
}

static FrontendResult<HirExpr> analyze_match_pattern(const AstExpr& pattern_expr,
                                                     const HirExpr& subject,
                                                     HirRoute* route,
                                                     const HirModule& mod,
                                                     const HirLocal* locals,
                                                     u32 local_count) {
    if (pattern_expr.kind == AstExprKind::VariantCase) {
        const bool subject_is_error_kind = subject.type != HirTypeKind::Variant &&
                                           subject.may_error &&
                                           subject.error_variant_index != 0xffffffffu;
        u32 variant_index =
            subject_is_error_kind ? subject.error_variant_index : subject.variant_index;
        if (pattern_expr.name.len != 0) {
            variant_index = mod.variants.len;
            for (u32 i = 0; i < mod.variants.len; i++) {
                if (mod.variants[i].name.eq(pattern_expr.name)) {
                    variant_index = i;
                    break;
                }
            }
            if (variant_index == mod.variants.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, pattern_expr.span, pattern_expr.name);
            if ((!subject_is_error_kind && (subject.type != HirTypeKind::Variant ||
                                            subject.variant_index != variant_index)) ||
                (subject_is_error_kind && subject.error_variant_index != variant_index))
                return frontend_error(FrontendError::UnsupportedSyntax, pattern_expr.span);
        } else if (subject.type != HirTypeKind::Variant && !subject_is_error_kind) {
            return frontend_error(FrontendError::UnsupportedSyntax, pattern_expr.span);
        }

        const auto& variant = mod.variants[variant_index];
        u32 case_index = variant.cases.len;
        for (u32 i = 0; i < variant.cases.len; i++) {
            if (variant.cases[i].name.eq(pattern_expr.str_value)) {
                case_index = i;
                break;
            }
        }
        if (case_index == variant.cases.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, pattern_expr.span, pattern_expr.str_value);

        const auto& case_decl = variant.cases[case_index];
        if (case_decl.has_payload) {
            if (pattern_expr.lhs != nullptr && pattern_expr.lhs->kind != AstExprKind::Ident)
                return frontend_error(FrontendError::UnsupportedSyntax, pattern_expr.span);
        } else if (pattern_expr.lhs != nullptr) {
            return frontend_error(FrontendError::UnsupportedSyntax, pattern_expr.span);
        }

        HirExpr out{};
        out.kind = HirExprKind::VariantCase;
        out.type = HirTypeKind::Variant;
        out.span = pattern_expr.span;
        out.variant_index = variant_index;
        out.case_index = case_index;
        out.int_value = static_cast<i32>(case_index);
        out.str_value = pattern_expr.str_value;
        return out;
    }

    return analyze_expr(pattern_expr, route, mod, locals, local_count, nullptr);
}

static u8 method_char(u8 token_type) {
    switch (static_cast<TokenType>(token_type)) {
        case TokenType::KwGet:
            return 'G';
        case TokenType::KwPost:
        case TokenType::KwPut:
        case TokenType::KwPatch:
            return 'P';
        case TokenType::KwDelete:
            return 'D';
        case TokenType::KwHead:
            return 'H';
        case TokenType::KwOptions:
            return 'O';
        default:
            return 0;
    }
}

static FrontendResult<HirExpr> instantiate_function_expr(const HirExpr& expr,
                                                         HirRoute* route,
                                                         const HirModule& mod,
                                                         const HirExpr* args,
                                                         u32 arg_count,
                                                         const GenericBinding* generic_bindings,
                                                         u32 generic_binding_count) {
    HirExpr out = apply_generic_binding_to_expr(expr, generic_bindings, generic_binding_count);
    auto concretized =
        concretize_named_instance_shape(&out, mod, generic_bindings, generic_binding_count);
    if (!concretized) return core::make_unexpected(concretized.error());
    out.lhs = nullptr;
    out.rhs = nullptr;
    out.field_inits.len = 0;
    out.args.len = 0;

    if (expr.kind == HirExprKind::LocalRef) {
        if (expr.local_index >= arg_count)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        HirExpr arg = args[expr.local_index];
        arg.span = expr.span;
        auto concretized =
            concretize_named_instance_shape(&arg, mod, generic_bindings, generic_binding_count);
        if (!concretized) return core::make_unexpected(concretized.error());
        return arg;
    }

    if (expr.kind == HirExprKind::ProtocolCall) {
        if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto recv = instantiate_function_expr(
            *expr.lhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!recv) return core::make_unexpected(recv.error());
        if (recv->type == HirTypeKind::Generic)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
        const HirImpl* impl =
            find_impl_for_type(mod, recv->type, recv->struct_index, expr.str_value);
        u32 function_index = 0xffffffffu;
        if (impl != nullptr) {
            const auto* method = find_impl_method(*impl, expr.str_value);
            if (method == nullptr || method->function_index >= mod.functions.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
            function_index = method->function_index;
        } else {
            if (expr.protocol_index >= mod.protocols.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
            const auto* req =
                find_protocol_method(mod.protocols[expr.protocol_index], expr.str_value);
            if (req == nullptr || req->function_index == 0xffffffffu)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
            function_index = req->function_index;
        }
        const auto& fn = mod.functions[function_index];
        HirExpr call_args[AstExpr::kMaxArgs]{};
        u32 call_arg_count = 0;
        call_args[call_arg_count++] = recv.value();
        for (u32 i = 0; i < expr.args.len; i++) {
            auto arg = instantiate_function_expr(*expr.args[i],
                                                 route,
                                                 mod,
                                                 args,
                                                 arg_count,
                                                 generic_bindings,
                                                 generic_binding_count);
            if (!arg) return core::make_unexpected(arg.error());
            call_args[call_arg_count++] = arg.value();
        }
        GenericBinding impl_bindings[HirFunction::kMaxTypeParams]{};
        u32 impl_binding_count = 0;
        if (impl != nullptr) {
            auto impl_filled = fill_impl_generic_bindings(
                mod, *impl, recv->struct_index, impl_bindings, impl_binding_count);
            if (!impl_filled) return core::make_unexpected(impl_filled.error());
            if (!impl_filled.value())
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
        } else {
            if (fn.type_params.len == 0 || !fn.type_params[0].name.eq(Str{"Self", 4}))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
            impl_binding_count = 1;
            auto filled = fill_bound_binding_from_type_metadata(&impl_bindings[0],
                                                                const_cast<HirModule*>(&mod),
                                                                recv->type,
                                                                recv->generic_index,
                                                                recv->variant_index,
                                                                recv->struct_index,
                                                                recv->tuple_len,
                                                                recv->tuple_types,
                                                                recv->tuple_variant_indices,
                                                                recv->tuple_struct_indices,
                                                                recv->shape_index,
                                                                expr.span);
            if (!filled) return core::make_unexpected(filled.error());
        }
        auto inlined = instantiate_function_expr(
            fn.body, route, mod, call_args, call_arg_count, impl_bindings, impl_binding_count);
        if (!inlined) return core::make_unexpected(inlined.error());
        if (fn.return_type != HirTypeKind::Unknown &&
            (inlined->kind == HirExprKind::Nil || inlined->kind == HirExprKind::Error)) {
            HirExpr expected{};
            expected.type = fn.return_type;
            expected.generic_index = fn.return_generic_index;
            expected.shape_index = fn.return_shape_index;
            expected.variant_index = fn.return_variant_index;
            expected.struct_index = fn.return_struct_index;
            expected.tuple_len = fn.return_tuple_len;
            for (u32 i = 0; i < fn.return_tuple_len; i++) {
                expected.tuple_types[i] = fn.return_tuple_types[i];
                expected.tuple_variant_indices[i] = fn.return_tuple_variant_indices[i];
                expected.tuple_struct_indices[i] = fn.return_tuple_struct_indices[i];
            }
            if (fn.return_generic_index < fn.type_params.len) {
                expected.generic_has_error_constraint =
                    fn.type_params[fn.return_generic_index].has_error_constraint;
                expected.generic_has_eq_constraint =
                    fn.type_params[fn.return_generic_index].has_eq_constraint;
                expected.generic_has_ord_constraint =
                    fn.type_params[fn.return_generic_index].has_ord_constraint;
                expected.generic_protocol_index =
                    fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                        ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                        : 0xffffffffu;
                expected.generic_protocol_count =
                    fn.type_params[fn.return_generic_index].custom_protocol_count;
                for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
                    expected.generic_protocol_indices[cpi] =
                        fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
            }
            auto concrete =
                apply_generic_binding_to_expr(expected, impl_bindings, impl_binding_count);
            auto concretized =
                concretize_named_instance_shape(&concrete, mod, impl_bindings, impl_binding_count);
            if (!concretized) return core::make_unexpected(concretized.error());
            inlined->type = concrete.type;
            inlined->generic_index = concrete.generic_index;
            inlined->generic_has_error_constraint = concrete.generic_has_error_constraint;
            inlined->generic_has_eq_constraint = concrete.generic_has_eq_constraint;
            inlined->generic_has_ord_constraint = concrete.generic_has_ord_constraint;
            inlined->generic_protocol_index = concrete.generic_protocol_index;
            inlined->generic_protocol_count = concrete.generic_protocol_count;
            for (u32 cpi = 0; cpi < concrete.generic_protocol_count; cpi++)
                inlined->generic_protocol_indices[cpi] = concrete.generic_protocol_indices[cpi];
            inlined->variant_index = concrete.variant_index;
            inlined->struct_index = concrete.struct_index;
            inlined->shape_index = concrete.shape_index;
            inlined->tuple_len = concrete.tuple_len;
            for (u32 i = 0; i < concrete.tuple_len; i++) {
                inlined->tuple_types[i] = concrete.tuple_types[i];
                inlined->tuple_variant_indices[i] = concrete.tuple_variant_indices[i];
                inlined->tuple_struct_indices[i] = concrete.tuple_struct_indices[i];
            }
            inlined->may_nil = inlined->kind == HirExprKind::Nil;
            inlined->may_error = inlined->kind == HirExprKind::Error;
        }
        inlined->span = expr.span;
        return inlined.value();
    }

    if (expr.kind == HirExprKind::TupleSlot) {
        if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto lhs = instantiate_function_expr(
            *expr.lhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (lhs->type != HirTypeKind::Tuple)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        const u32 slot = static_cast<u32>(expr.int_value);
        if (slot >= lhs->tuple_len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (lhs->args.len == lhs->tuple_len && lhs->args[slot] != nullptr) {
            HirExpr out_elem = *lhs->args[slot];
            out_elem.span = expr.span;
            return out_elem;
        }
        HirExpr out_slot = expr;
        if (!route->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out_slot.lhs = &route->exprs[route->exprs.len - 1];
        out_slot.span = expr.span;
        return out_slot;
    }

    if (expr.kind == HirExprKind::Or) {
        if (expr.lhs == nullptr || expr.rhs == nullptr)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto lhs = instantiate_function_expr(
            *expr.lhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!lhs->may_nil && !lhs->may_error) {
            HirExpr out_lhs = lhs.value();
            out_lhs.span = expr.span;
            return out_lhs;
        }
        auto rhs = instantiate_function_expr(
            *expr.rhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!rhs) return core::make_unexpected(rhs.error());
        HirExpr out_or = expr;
        if (!route->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out_or.lhs = &route->exprs[route->exprs.len - 1];
        if (!route->exprs.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out_or.rhs = &route->exprs[route->exprs.len - 1];
        out_or.span = expr.span;
        return out_or;
    }

    if (expr.lhs != nullptr) {
        auto lhs = instantiate_function_expr(
            *expr.lhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!route->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &route->exprs[route->exprs.len - 1];
    }
    if (expr.rhs != nullptr) {
        auto rhs = instantiate_function_expr(
            *expr.rhs, route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (!route->exprs.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.rhs = &route->exprs[route->exprs.len - 1];
    }
    for (u32 i = 0; i < expr.field_inits.len; i++) {
        auto val = instantiate_function_expr(*expr.field_inits[i].value,
                                             route,
                                             mod,
                                             args,
                                             arg_count,
                                             generic_bindings,
                                             generic_binding_count);
        if (!val) return core::make_unexpected(val.error());
        if (!route->exprs.push(val.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr::FieldInit init{};
        init.name = expr.field_inits[i].name;
        init.value = &route->exprs[route->exprs.len - 1];
        if (!out.field_inits.push(init))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    for (u32 i = 0; i < expr.args.len; i++) {
        auto arg = instantiate_function_expr(
            *expr.args[i], route, mod, args, arg_count, generic_bindings, generic_binding_count);
        if (!arg) return core::make_unexpected(arg.error());
        if (!route->exprs.push(arg.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        if (!out.args.push(&route->exprs[route->exprs.len - 1]))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    out.span = expr.span;
    return out;
}

static FrontendResult<HirExpr> normalize_function_expr(const HirExpr& expr,
                                                       HirFunction* fn,
                                                       const HirLocal* locals,
                                                       u32 local_count,
                                                       u32 param_count) {
    HirExpr out = expr;
    out.lhs = nullptr;
    out.rhs = nullptr;
    out.field_inits.len = 0;
    out.args.len = 0;

    if (expr.kind == HirExprKind::LocalRef) {
        if (expr.local_index >= local_count)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (expr.local_index < param_count) return expr;
        return normalize_function_expr(
            locals[expr.local_index].init, fn, locals, local_count, param_count);
    }

    if (expr.kind == HirExprKind::TupleSlot) {
        if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto lhs = normalize_function_expr(*expr.lhs, fn, locals, local_count, param_count);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (lhs->type != HirTypeKind::Tuple)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        const u32 slot = static_cast<u32>(expr.int_value);
        if (slot >= lhs->tuple_len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (lhs->args.len == lhs->tuple_len && lhs->args[slot] != nullptr) {
            HirExpr out_elem = *lhs->args[slot];
            out_elem.span = expr.span;
            return out_elem;
        }
        if (!fn->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &fn->exprs[fn->exprs.len - 1];
        out.span = expr.span;
        return out;
    }

    if (expr.lhs != nullptr) {
        auto lhs = normalize_function_expr(*expr.lhs, fn, locals, local_count, param_count);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (!fn->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &fn->exprs[fn->exprs.len - 1];
    }
    if (expr.rhs != nullptr) {
        auto rhs = normalize_function_expr(*expr.rhs, fn, locals, local_count, param_count);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (!fn->exprs.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.rhs = &fn->exprs[fn->exprs.len - 1];
    }
    for (u32 i = 0; i < expr.field_inits.len; i++) {
        auto val = normalize_function_expr(
            *expr.field_inits[i].value, fn, locals, local_count, param_count);
        if (!val) return core::make_unexpected(val.error());
        if (!fn->exprs.push(val.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr::FieldInit init{};
        init.name = expr.field_inits[i].name;
        init.value = &fn->exprs[fn->exprs.len - 1];
        if (!out.field_inits.push(init))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    for (u32 i = 0; i < expr.args.len; i++) {
        auto arg = normalize_function_expr(*expr.args[i], fn, locals, local_count, param_count);
        if (!arg) return core::make_unexpected(arg.error());
        if (!fn->exprs.push(arg.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        if (!out.args.push(&fn->exprs[fn->exprs.len - 1]))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    out.span = expr.span;
    return out;
}

static FrontendResult<HirExpr> analyze_function_body_stmt(const AstStatement& stmt,
                                                          HirRoute* scratch,
                                                          const HirModule& mod,
                                                          const HirLocal* locals,
                                                          u32 local_count,
                                                          const MatchPayloadBinding* binding) {
    auto merge_expr_shape = [&](const HirExpr& lhs, const HirExpr& rhs, HirExpr* out) -> bool {
        HirTypeKind merged_type = lhs.type;
        u32 merged_generic_index = lhs.generic_index;
        u32 merged_shape_index = lhs.shape_index;
        bool merged_generic_has_error_constraint = lhs.generic_has_error_constraint;
        bool merged_generic_has_eq_constraint = lhs.generic_has_eq_constraint;
        bool merged_generic_has_ord_constraint = lhs.generic_has_ord_constraint;
        u32 merged_variant_index = lhs.variant_index;
        u32 merged_tuple_len = lhs.tuple_len;
        HirTypeKind merged_tuple_types[kMaxTupleSlots]{};
        u32 merged_tuple_variant_indices[kMaxTupleSlots]{};
        u32 merged_tuple_struct_indices[kMaxTupleSlots]{};
        for (u32 i = 0; i < lhs.tuple_len; i++) {
            merged_tuple_types[i] = lhs.tuple_types[i];
            merged_tuple_variant_indices[i] = lhs.tuple_variant_indices[i];
            merged_tuple_struct_indices[i] = lhs.tuple_struct_indices[i];
        }
        if (lhs.type == HirTypeKind::Unknown) {
            merged_type = rhs.type;
            merged_generic_index = rhs.generic_index;
            merged_shape_index = rhs.shape_index;
            merged_generic_has_error_constraint = rhs.generic_has_error_constraint;
            merged_generic_has_eq_constraint = rhs.generic_has_eq_constraint;
            merged_generic_has_ord_constraint = rhs.generic_has_ord_constraint;
            merged_variant_index = rhs.variant_index;
            merged_tuple_len = rhs.tuple_len;
            for (u32 i = 0; i < rhs.tuple_len; i++) {
                merged_tuple_types[i] = rhs.tuple_types[i];
                merged_tuple_variant_indices[i] = rhs.tuple_variant_indices[i];
                merged_tuple_struct_indices[i] = rhs.tuple_struct_indices[i];
            }
        } else if (rhs.type != HirTypeKind::Unknown) {
            if (!same_hir_type_shape(mod, lhs, rhs)) return false;
        }
        u32 merged_error_struct = lhs.error_struct_index;
        u32 merged_error_variant = lhs.error_variant_index;
        u32 merged_struct_index = lhs.struct_index;
        if (lhs.type == HirTypeKind::Unknown) merged_struct_index = rhs.struct_index;
        if (lhs.error_struct_index == 0xffffffffu) {
            merged_error_struct = rhs.error_struct_index;
            merged_error_variant = rhs.error_variant_index;
        } else if (rhs.error_struct_index != 0xffffffffu) {
            if (lhs.error_struct_index != rhs.error_struct_index ||
                lhs.error_variant_index != rhs.error_variant_index)
                return false;
        }

        out->type = merged_type;
        out->generic_index = merged_generic_index;
        out->shape_index = merged_shape_index;
        out->generic_has_error_constraint = merged_generic_has_error_constraint;
        out->generic_has_eq_constraint = merged_generic_has_eq_constraint;
        out->generic_has_ord_constraint = merged_generic_has_ord_constraint;
        out->generic_protocol_index = lhs.generic_protocol_index != 0xffffffffu
                                          ? lhs.generic_protocol_index
                                          : rhs.generic_protocol_index;
        out->generic_protocol_count = lhs.generic_protocol_count != 0 ? lhs.generic_protocol_count
                                                                      : rhs.generic_protocol_count;
        for (u32 i = 0; i < out->generic_protocol_count; i++) {
            out->generic_protocol_indices[i] = lhs.generic_protocol_count != 0
                                                   ? lhs.generic_protocol_indices[i]
                                                   : rhs.generic_protocol_indices[i];
        }
        out->variant_index = merged_variant_index;
        out->struct_index = merged_struct_index;
        out->tuple_len = merged_tuple_len;
        for (u32 i = 0; i < merged_tuple_len; i++) {
            out->tuple_types[i] = merged_tuple_types[i];
            out->tuple_variant_indices[i] = merged_tuple_variant_indices[i];
            out->tuple_struct_indices[i] = merged_tuple_struct_indices[i];
        }
        out->may_nil = lhs.may_nil || rhs.may_nil;
        out->may_error = lhs.may_error || rhs.may_error;
        out->error_struct_index = merged_error_struct;
        out->error_variant_index = merged_error_variant;
        return true;
    };

    auto analyze_function_guard_match_expr =
        [&](const FixedVec<AstStatement::MatchArm, AstStatement::kMaxMatchArms>& ast_arms,
            const HirExpr& subject,
            const HirLocal* cur_locals,
            u32 cur_local_count) -> FrontendResult<HirExpr> {
        if (!subject.may_error) return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);

        bool seen_wildcard = false;
        bool seen_variant_cases[HirVariant::kMaxCases]{};
        u32 seen_variant_case_count = 0;
        HirExpr result{};
        bool have_result = false;

        for (i32 ai = static_cast<i32>(ast_arms.len) - 1; ai >= 0; ai--) {
            const auto& arm = ast_arms[static_cast<u32>(ai)];
            if (arm.is_wildcard) {
                if (seen_wildcard)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                seen_wildcard = true;
                auto body = analyze_function_body_stmt(
                    *arm.stmt, scratch, mod, cur_locals, cur_local_count, binding);
                if (!body) return core::make_unexpected(body.error());
                result = body.value();
                have_result = true;
                continue;
            }

            auto pattern = analyze_match_pattern(
                arm.pattern, subject, scratch, mod, cur_locals, cur_local_count);
            if (!pattern) return core::make_unexpected(pattern.error());
            if (pattern->kind != HirExprKind::VariantCase)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            if (!(subject.may_error && subject.error_variant_index != 0xffffffffu) ||
                pattern->variant_index != subject.error_variant_index)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            const u32 case_index = static_cast<u32>(pattern->int_value);
            if (case_index >= HirVariant::kMaxCases)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            if (seen_variant_cases[case_index])
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            seen_variant_cases[case_index] = true;
            seen_variant_case_count++;

            auto body = analyze_function_body_stmt(
                *arm.stmt, scratch, mod, cur_locals, cur_local_count, binding);
            if (!body) return core::make_unexpected(body.error());
            if (!have_result) {
                result = body.value();
                have_result = true;
                continue;
            }

            HirExpr merged_shape{};
            if (!merge_expr_shape(body.value(), result, &merged_shape))
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);

            HirExpr code_field{};
            code_field.kind = HirExprKind::Field;
            code_field.type = HirTypeKind::I32;
            code_field.span = arm.span;
            code_field.str_value = Str{"code", 4};
            if (!scratch->exprs.push(subject))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            code_field.lhs = &scratch->exprs[scratch->exprs.len - 1];

            HirExpr pattern_code{};
            pattern_code.kind = HirExprKind::IntLit;
            pattern_code.type = HirTypeKind::I32;
            pattern_code.int_value = static_cast<i32>(case_index);
            pattern_code.span = arm.span;

            HirExpr cond{};
            cond.kind = HirExprKind::Eq;
            cond.type = HirTypeKind::Bool;
            cond.span = arm.span;
            if (!scratch->exprs.push(code_field))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            cond.lhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(pattern_code))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            cond.rhs = &scratch->exprs[scratch->exprs.len - 1];

            HirExpr out{};
            out.kind = HirExprKind::IfElse;
            out.type = merged_shape.type;
            out.generic_index = merged_shape.generic_index;
            out.generic_has_error_constraint = merged_shape.generic_has_error_constraint;
            out.generic_has_eq_constraint = merged_shape.generic_has_eq_constraint;
            out.generic_has_ord_constraint = merged_shape.generic_has_ord_constraint;
            out.generic_protocol_index = merged_shape.generic_protocol_index;
            out.generic_protocol_count = merged_shape.generic_protocol_count;
            for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                out.generic_protocol_indices[cpi] = merged_shape.generic_protocol_indices[cpi];
            out.shape_index = merged_shape.shape_index;
            out.may_nil = merged_shape.may_nil;
            out.may_error = merged_shape.may_error;
            out.variant_index = merged_shape.variant_index;
            out.struct_index = merged_shape.struct_index;
            out.tuple_len = merged_shape.tuple_len;
            for (u32 i = 0; i < merged_shape.tuple_len; i++) {
                out.tuple_types[i] = merged_shape.tuple_types[i];
                out.tuple_variant_indices[i] = merged_shape.tuple_variant_indices[i];
                out.tuple_struct_indices[i] = merged_shape.tuple_struct_indices[i];
            }
            out.error_struct_index = merged_shape.error_struct_index;
            out.error_variant_index = merged_shape.error_variant_index;
            out.span = stmt.span;
            if (!scratch->exprs.push(cond))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            out.lhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(body.value()))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            out.rhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(result))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            if (!out.args.push(&scratch->exprs[scratch->exprs.len - 1]))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            result = out;
        }

        if (!have_result) return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        if (!seen_wildcard) return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        if (subject.error_variant_index != 0xffffffffu) {
            const auto& variant = mod.variants[subject.error_variant_index];
            if (!seen_wildcard && seen_variant_case_count != variant.cases.len)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        }
        return result;
    };

    if (stmt.kind == AstStmtKind::Expr)
        return analyze_expr(stmt.expr, scratch, mod, locals, local_count, binding);

    if (stmt.kind == AstStmtKind::If) {
        auto cond = analyze_expr(stmt.expr, scratch, mod, locals, local_count, binding);
        if (!cond) return core::make_unexpected(cond.error());
        if (cond->type != HirTypeKind::Bool || cond->may_nil || cond->may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        auto then_expr =
            analyze_function_body_stmt(*stmt.then_stmt, scratch, mod, locals, local_count, binding);
        if (!then_expr) return core::make_unexpected(then_expr.error());
        auto else_expr =
            analyze_function_body_stmt(*stmt.else_stmt, scratch, mod, locals, local_count, binding);
        if (!else_expr) return core::make_unexpected(else_expr.error());
        HirExpr merged_shape{};
        if (!merge_expr_shape(then_expr.value(), else_expr.value(), &merged_shape))
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);

        HirExpr out{};
        out.kind = HirExprKind::IfElse;
        out.type = merged_shape.type;
        out.generic_index = merged_shape.generic_index;
        out.generic_has_error_constraint = merged_shape.generic_has_error_constraint;
        out.generic_has_eq_constraint = merged_shape.generic_has_eq_constraint;
        out.generic_has_ord_constraint = merged_shape.generic_has_ord_constraint;
        out.generic_protocol_index = merged_shape.generic_protocol_index;
        out.generic_protocol_count = merged_shape.generic_protocol_count;
        for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
            out.generic_protocol_indices[cpi] = merged_shape.generic_protocol_indices[cpi];
        out.shape_index = merged_shape.shape_index;
        out.may_nil = merged_shape.may_nil;
        out.may_error = merged_shape.may_error;
        out.variant_index = merged_shape.variant_index;
        out.struct_index = merged_shape.struct_index;
        out.tuple_len = merged_shape.tuple_len;
        for (u32 i = 0; i < merged_shape.tuple_len; i++) {
            out.tuple_types[i] = merged_shape.tuple_types[i];
            out.tuple_variant_indices[i] = merged_shape.tuple_variant_indices[i];
            out.tuple_struct_indices[i] = merged_shape.tuple_struct_indices[i];
        }
        out.error_struct_index = merged_shape.error_struct_index;
        out.error_variant_index = merged_shape.error_variant_index;
        out.span = stmt.span;
        if (!scratch->exprs.push(cond.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        out.lhs = &scratch->exprs[scratch->exprs.len - 1];
        if (!scratch->exprs.push(then_expr.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        out.rhs = &scratch->exprs[scratch->exprs.len - 1];
        if (!scratch->exprs.push(else_expr.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        if (!out.args.push(&scratch->exprs[scratch->exprs.len - 1]))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        return out;
    }

    if (stmt.kind == AstStmtKind::Match && stmt.is_const) {
        auto subject = analyze_expr(stmt.expr, scratch, mod, locals, local_count, nullptr);
        if (!subject) return core::make_unexpected(subject.error());
        ConstValue subject_value{};
        if (!const_eval_expr(subject.value(), locals, local_count, &subject_value, 0))
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);

        if (!scratch->exprs.push(subject.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        const HirExpr* subject_ptr = &scratch->exprs[scratch->exprs.len - 1];

        const AstStatement::MatchArm* wildcard_arm = nullptr;
        const AstStatement::MatchArm* selected_arm = nullptr;
        MatchPayloadBinding selected_binding{};
        const MatchPayloadBinding* selected_binding_ptr = binding;

        for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
            const auto& arm = stmt.match_arms[ai];
            if (arm.is_wildcard) {
                wildcard_arm = &arm;
                continue;
            }
            auto pattern = analyze_match_pattern(
                arm.pattern, subject.value(), scratch, mod, locals, local_count);
            if (!pattern) return core::make_unexpected(pattern.error());

            bool matched = false;
            if (pattern->kind == HirExprKind::BoolLit && subject_value.type == HirTypeKind::Bool) {
                matched = pattern->bool_value == subject_value.bool_value;
            } else if (pattern->kind == HirExprKind::IntLit &&
                       subject_value.type == HirTypeKind::I32) {
                matched = pattern->int_value == subject_value.int_value;
            } else if (pattern->kind == HirExprKind::VariantCase &&
                       subject_value.type == HirTypeKind::Variant) {
                matched = pattern->variant_index == subject_value.variant_index &&
                          pattern->case_index == subject_value.case_index;
            }
            if (!matched) continue;
            selected_arm = &arm;
            if (pattern->kind == HirExprKind::VariantCase &&
                mod.variants[pattern->variant_index].cases[pattern->case_index].has_payload &&
                arm.pattern.lhs != nullptr) {
                selected_binding.name = arm.pattern.lhs->name;
                selected_binding.type =
                    mod.variants[pattern->variant_index].cases[pattern->case_index].payload_type;
                selected_binding.generic_index = mod.variants[pattern->variant_index]
                                                     .cases[pattern->case_index]
                                                     .payload_generic_index;
                selected_binding.generic_has_error_constraint =
                    mod.variants[pattern->variant_index]
                        .cases[pattern->case_index]
                        .payload_generic_has_error_constraint;
                selected_binding.generic_has_eq_constraint = mod.variants[pattern->variant_index]
                                                                 .cases[pattern->case_index]
                                                                 .payload_generic_has_eq_constraint;
                selected_binding.generic_has_ord_constraint =
                    mod.variants[pattern->variant_index]
                        .cases[pattern->case_index]
                        .payload_generic_has_ord_constraint;
                selected_binding.generic_protocol_index = mod.variants[pattern->variant_index]
                                                              .cases[pattern->case_index]
                                                              .payload_generic_protocol_index;
                selected_binding.generic_protocol_count = mod.variants[pattern->variant_index]
                                                              .cases[pattern->case_index]
                                                              .payload_generic_protocol_count;
                for (u32 cpi = 0; cpi < selected_binding.generic_protocol_count; cpi++) {
                    selected_binding.generic_protocol_indices[cpi] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_generic_protocol_indices[cpi];
                }
                selected_binding.variant_index = mod.variants[pattern->variant_index]
                                                     .cases[pattern->case_index]
                                                     .payload_variant_index;
                selected_binding.struct_index = mod.variants[pattern->variant_index]
                                                    .cases[pattern->case_index]
                                                    .payload_struct_index;
                selected_binding.shape_index = mod.variants[pattern->variant_index]
                                                   .cases[pattern->case_index]
                                                   .payload_shape_index;
                selected_binding.tuple_len = mod.variants[pattern->variant_index]
                                                 .cases[pattern->case_index]
                                                 .payload_tuple_len;
                for (u32 ti = 0; ti < selected_binding.tuple_len; ti++) {
                    selected_binding.tuple_types[ti] = mod.variants[pattern->variant_index]
                                                           .cases[pattern->case_index]
                                                           .payload_tuple_types[ti];
                    selected_binding.tuple_variant_indices[ti] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_tuple_variant_indices[ti];
                    selected_binding.tuple_struct_indices[ti] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_tuple_struct_indices[ti];
                }
                selected_binding.case_index = pattern->case_index;
                selected_binding.subject = subject_ptr;
                selected_binding_ptr = &selected_binding;
            }
            break;
        }

        if (selected_arm == nullptr) selected_arm = wildcard_arm;
        if (selected_arm == nullptr)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        return analyze_function_body_stmt(
            *selected_arm->stmt, scratch, mod, locals, local_count, selected_binding_ptr);
    }

    if (stmt.kind == AstStmtKind::Match) {
        auto subject = analyze_expr(stmt.expr, scratch, mod, locals, local_count, nullptr);
        if (!subject) return core::make_unexpected(subject.error());
        if (subject->may_nil || subject->may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);

        if (!scratch->exprs.push(subject.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        const HirExpr* subject_ptr = &scratch->exprs[scratch->exprs.len - 1];

        bool seen_wildcard = false;
        bool seen_variant_cases[HirVariant::kMaxCases]{};
        u32 seen_variant_case_count = 0;
        HirExpr result{};
        bool have_result = false;

        for (i32 ai = static_cast<i32>(stmt.match_arms.len) - 1; ai >= 0; ai--) {
            const auto& arm = stmt.match_arms[static_cast<u32>(ai)];
            if (arm.is_wildcard) {
                if (seen_wildcard)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                seen_wildcard = true;
                auto body = analyze_function_body_stmt(
                    *arm.stmt, scratch, mod, locals, local_count, binding);
                if (!body) return core::make_unexpected(body.error());
                result = body.value();
                have_result = true;
                continue;
            }

            auto pattern = analyze_match_pattern(
                arm.pattern, subject.value(), scratch, mod, locals, local_count);
            if (!pattern) return core::make_unexpected(pattern.error());
            if (pattern->kind != HirExprKind::BoolLit && pattern->kind != HirExprKind::IntLit &&
                pattern->kind != HirExprKind::VariantCase)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            if (pattern->type != subject->type)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            if (pattern->type == HirTypeKind::Variant) {
                if (pattern->variant_index != subject->variant_index)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                const u32 case_index = static_cast<u32>(pattern->int_value);
                if (case_index >= HirVariant::kMaxCases)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                if (seen_variant_cases[case_index])
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                seen_variant_cases[case_index] = true;
                seen_variant_case_count++;
            }

            MatchPayloadBinding arm_binding{};
            const MatchPayloadBinding* arm_binding_ptr = binding;
            if (pattern->type == HirTypeKind::Variant) {
                const u32 case_index = static_cast<u32>(pattern->int_value);
                const auto& case_decl = mod.variants[pattern->variant_index].cases[case_index];
                if (case_decl.has_payload && arm.pattern.lhs != nullptr) {
                    arm_binding.name = arm.pattern.lhs->name;
                    arm_binding.type = case_decl.payload_type;
                    arm_binding.generic_index = case_decl.payload_generic_index;
                    arm_binding.generic_has_error_constraint =
                        case_decl.payload_generic_has_error_constraint;
                    arm_binding.generic_has_eq_constraint =
                        case_decl.payload_generic_has_eq_constraint;
                    arm_binding.generic_has_ord_constraint =
                        case_decl.payload_generic_has_ord_constraint;
                    arm_binding.generic_protocol_index = case_decl.payload_generic_protocol_index;
                    arm_binding.generic_protocol_count = case_decl.payload_generic_protocol_count;
                    for (u32 cpi = 0; cpi < arm_binding.generic_protocol_count; cpi++)
                        arm_binding.generic_protocol_indices[cpi] =
                            case_decl.payload_generic_protocol_indices[cpi];
                    arm_binding.variant_index = case_decl.payload_variant_index;
                    arm_binding.struct_index = case_decl.payload_struct_index;
                    arm_binding.shape_index = case_decl.payload_shape_index;
                    arm_binding.tuple_len = case_decl.payload_tuple_len;
                    for (u32 ti = 0; ti < arm_binding.tuple_len; ti++) {
                        arm_binding.tuple_types[ti] = case_decl.payload_tuple_types[ti];
                        arm_binding.tuple_variant_indices[ti] =
                            case_decl.payload_tuple_variant_indices[ti];
                        arm_binding.tuple_struct_indices[ti] =
                            case_decl.payload_tuple_struct_indices[ti];
                    }
                    arm_binding.case_index = case_index;
                    arm_binding.subject = subject_ptr;
                    arm_binding_ptr = &arm_binding;
                }
            }

            auto body = analyze_function_body_stmt(
                *arm.stmt, scratch, mod, locals, local_count, arm_binding_ptr);
            if (!body) return core::make_unexpected(body.error());
            if (!have_result) {
                result = body.value();
                have_result = true;
                continue;
            }
            HirExpr merged_shape{};
            if (!merge_expr_shape(body.value(), result, &merged_shape))
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);

            HirExpr cond{};
            cond.kind = HirExprKind::Eq;
            cond.type = HirTypeKind::Bool;
            cond.span = arm.span;
            if (!scratch->exprs.push(*subject_ptr))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            cond.lhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(pattern.value()))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            cond.rhs = &scratch->exprs[scratch->exprs.len - 1];

            HirExpr out{};
            out.kind = HirExprKind::IfElse;
            out.type = merged_shape.type;
            out.generic_index = merged_shape.generic_index;
            out.generic_has_error_constraint = merged_shape.generic_has_error_constraint;
            out.generic_has_eq_constraint = merged_shape.generic_has_eq_constraint;
            out.generic_has_ord_constraint = merged_shape.generic_has_ord_constraint;
            out.generic_protocol_index = merged_shape.generic_protocol_index;
            out.generic_protocol_count = merged_shape.generic_protocol_count;
            for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                out.generic_protocol_indices[cpi] = merged_shape.generic_protocol_indices[cpi];
            out.may_nil = merged_shape.may_nil;
            out.may_error = merged_shape.may_error;
            out.variant_index = merged_shape.variant_index;
            out.struct_index = merged_shape.struct_index;
            out.tuple_len = merged_shape.tuple_len;
            for (u32 i = 0; i < merged_shape.tuple_len; i++) {
                out.tuple_types[i] = merged_shape.tuple_types[i];
                out.tuple_variant_indices[i] = merged_shape.tuple_variant_indices[i];
                out.tuple_struct_indices[i] = merged_shape.tuple_struct_indices[i];
            }
            out.error_struct_index = merged_shape.error_struct_index;
            out.error_variant_index = merged_shape.error_variant_index;
            out.shape_index = merged_shape.shape_index;
            out.span = stmt.span;
            if (!scratch->exprs.push(cond))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            out.lhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(body.value()))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            out.rhs = &scratch->exprs[scratch->exprs.len - 1];
            if (!scratch->exprs.push(result))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            if (!out.args.push(&scratch->exprs[scratch->exprs.len - 1]))
                return frontend_error(FrontendError::TooManyItems, arm.span);
            result = out;
        }

        if (!have_result) return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        if (result.type == HirTypeKind::Unknown && !result.may_nil && !result.may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        if (!seen_wildcard) {
            if (subject->type != HirTypeKind::Variant)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
            const auto& variant = mod.variants[subject->variant_index];
            if (seen_variant_case_count != variant.cases.len)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        }
        return result;
    }

    if (stmt.kind == AstStmtKind::Block) {
        HirLocal scoped[HirRoute::kMaxLocals]{};
        for (u32 i = 0; i < local_count; i++) scoped[i] = locals[i];
        u32 scoped_count = local_count;
        auto analyze_block_tail = [&](auto&& self,
                                      u32 si,
                                      const HirLocal* cur_locals,
                                      u32 cur_local_count) -> FrontendResult<HirExpr> {
            if (si >= stmt.block_stmts.len)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
            const auto& inner = *stmt.block_stmts[si];
            const bool is_last = si + 1 == stmt.block_stmts.len;
            if (inner.kind == AstStmtKind::Let && !is_last) {
                auto init =
                    analyze_expr(inner.expr, scratch, mod, cur_locals, cur_local_count, nullptr);
                if (!init) return core::make_unexpected(init.error());
                auto typed = apply_declared_type_to_expr(&init.value(), mod, inner);
                if (!typed) return core::make_unexpected(typed.error());
                if (cur_local_count >= HirRoute::kMaxLocals)
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                HirLocal next_locals[HirRoute::kMaxLocals]{};
                for (u32 i = 0; i < cur_local_count; i++) next_locals[i] = cur_locals[i];
                u32 next_count = cur_local_count;
                HirLocal local{};
                local.span = inner.span;
                local.name = inner.name;
                local.ref_index = next_local_ref_index(scratch, cur_locals, cur_local_count);
                local.type = init->type;
                local.generic_index = init->generic_index;
                local.generic_has_error_constraint = init->generic_has_error_constraint;
                local.generic_has_eq_constraint = init->generic_has_eq_constraint;
                local.generic_has_ord_constraint = init->generic_has_ord_constraint;
                local.generic_protocol_index = init->generic_protocol_index;
                local.generic_protocol_count = init->generic_protocol_count;
                for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                    local.generic_protocol_indices[cpi] = init->generic_protocol_indices[cpi];
                local.may_nil = init->may_nil;
                local.may_error = init->may_error;
                local.variant_index = init->variant_index;
                local.struct_index = init->struct_index;
                local.tuple_len = init->tuple_len;
                for (u32 ti = 0; ti < init->tuple_len; ti++) {
                    local.tuple_types[ti] = init->tuple_types[ti];
                    local.tuple_variant_indices[ti] = init->tuple_variant_indices[ti];
                    local.tuple_struct_indices[ti] = init->tuple_struct_indices[ti];
                }
                local.error_struct_index = init->error_struct_index;
                local.error_variant_index = init->error_variant_index;
                local.shape_index = init->shape_index;
                local.init = init.value();
                auto inserted = insert_scoped_local(
                    next_locals, next_count, HirRoute::kMaxLocals, local, inner.span);
                if (!inserted) return core::make_unexpected(inserted.error());
                if (!scratch->locals.push(local))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                return self(self, si + 1, next_locals, next_count);
            }
            if (inner.kind == AstStmtKind::Guard && !is_last) {
                auto bound =
                    analyze_expr(inner.expr, scratch, mod, cur_locals, cur_local_count, nullptr);
                if (!bound) return core::make_unexpected(bound.error());
                auto cond =
                    analyze_guard_cond(inner.expr, scratch, mod, cur_locals, cur_local_count);
                if (!cond) return core::make_unexpected(cond.error());

                HirLocal succ_locals[HirRoute::kMaxLocals]{};
                for (u32 i = 0; i < cur_local_count; i++) succ_locals[i] = cur_locals[i];
                u32 succ_count = cur_local_count;
                if (inner.bind_value) {
                    if (known_value_state(bound.value(), cur_locals, cur_local_count, 0) ==
                        KnownValueState::Error)
                        return frontend_error(FrontendError::UnsupportedSyntax, inner.expr.span);
                    HirLocal local{};
                    local.span = inner.span;
                    local.name = inner.name;
                    local.ref_index = next_local_ref_index(scratch, cur_locals, cur_local_count);
                    local.type = bound->type;
                    local.generic_index = bound->generic_index;
                    local.generic_has_error_constraint = bound->generic_has_error_constraint;
                    local.generic_has_eq_constraint = bound->generic_has_eq_constraint;
                    local.generic_has_ord_constraint = bound->generic_has_ord_constraint;
                    local.generic_protocol_index = bound->generic_protocol_index;
                    local.generic_protocol_count = bound->generic_protocol_count;
                    for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                        local.generic_protocol_indices[cpi] = bound->generic_protocol_indices[cpi];
                    local.may_nil = bound->may_nil;
                    local.may_error = false;
                    local.variant_index = bound->variant_index;
                    local.struct_index = bound->struct_index;
                    local.tuple_len = bound->tuple_len;
                    for (u32 ti = 0; ti < bound->tuple_len; ti++) {
                        local.tuple_types[ti] = bound->tuple_types[ti];
                        local.tuple_variant_indices[ti] = bound->tuple_variant_indices[ti];
                        local.tuple_struct_indices[ti] = bound->tuple_struct_indices[ti];
                    }
                    local.error_struct_index = bound->error_struct_index;
                    local.error_variant_index = 0xffffffffu;
                    local.shape_index = bound->shape_index;
                    auto init = make_guard_bound_init(scratch, bound.value(), inner.span);
                    if (!init) return core::make_unexpected(init.error());
                    local.init = init.value();
                    auto inserted = insert_scoped_local(
                        succ_locals, succ_count, HirRoute::kMaxLocals, local, inner.span);
                    if (!inserted) return core::make_unexpected(inserted.error());
                    if (!scratch->locals.push(local))
                        return frontend_error(FrontendError::TooManyItems, inner.span);
                }

                auto then_expr = self(self, si + 1, succ_locals, succ_count);
                if (!then_expr) return core::make_unexpected(then_expr.error());
                FrontendResult<HirExpr> else_expr =
                    frontend_error(FrontendError::UnsupportedSyntax, inner.span);
                if (inner.match_arms.len != 0) {
                    else_expr = analyze_function_guard_match_expr(
                        inner.match_arms, bound.value(), cur_locals, cur_local_count);
                } else {
                    else_expr = analyze_function_body_stmt(
                        *inner.else_stmt, scratch, mod, cur_locals, cur_local_count, binding);
                }
                if (!else_expr) return core::make_unexpected(else_expr.error());
                HirExpr merged_shape{};
                if (!merge_expr_shape(then_expr.value(), else_expr.value(), &merged_shape))
                    return frontend_error(FrontendError::UnsupportedSyntax, inner.span);

                HirExpr out{};
                out.kind = HirExprKind::IfElse;
                out.type = merged_shape.type;
                out.generic_index = merged_shape.generic_index;
                out.generic_has_error_constraint = merged_shape.generic_has_error_constraint;
                out.generic_has_eq_constraint = merged_shape.generic_has_eq_constraint;
                out.generic_has_ord_constraint = merged_shape.generic_has_ord_constraint;
                out.generic_protocol_index = merged_shape.generic_protocol_index;
                out.generic_protocol_count = merged_shape.generic_protocol_count;
                for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                    out.generic_protocol_indices[cpi] = merged_shape.generic_protocol_indices[cpi];
                out.may_nil = merged_shape.may_nil;
                out.may_error = merged_shape.may_error;
                out.variant_index = merged_shape.variant_index;
                out.struct_index = merged_shape.struct_index;
                out.shape_index = merged_shape.shape_index;
                out.tuple_len = merged_shape.tuple_len;
                for (u32 i = 0; i < merged_shape.tuple_len; i++) {
                    out.tuple_types[i] = merged_shape.tuple_types[i];
                    out.tuple_variant_indices[i] = merged_shape.tuple_variant_indices[i];
                    out.tuple_struct_indices[i] = merged_shape.tuple_struct_indices[i];
                }
                out.error_struct_index = merged_shape.error_struct_index;
                out.error_variant_index = merged_shape.error_variant_index;
                out.span = inner.span;
                if (!scratch->exprs.push(cond.value()))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                out.lhs = &scratch->exprs[scratch->exprs.len - 1];
                if (!scratch->exprs.push(then_expr.value()))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                out.rhs = &scratch->exprs[scratch->exprs.len - 1];
                if (!scratch->exprs.push(else_expr.value()))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (!out.args.push(&scratch->exprs[scratch->exprs.len - 1]))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                return out;
            }
            if (!is_last && inner.kind != AstStmtKind::Let && inner.kind != AstStmtKind::Guard)
                return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
            return analyze_function_body_stmt(
                inner, scratch, mod, cur_locals, cur_local_count, binding);
        };
        return analyze_block_tail(analyze_block_tail, 0, scoped, scoped_count);
        return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
    }

    return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
}

static FrontendResult<HirExpr> analyze_expr(const AstExpr& expr,
                                            HirRoute* route,
                                            const HirModule& mod,
                                            const HirLocal* locals,
                                            u32 local_count,
                                            const MatchPayloadBinding* binding) {
    HirExpr out{};
    out.span = expr.span;
    if (expr.kind == AstExprKind::Placeholder)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
    if (expr.kind == AstExprKind::BoolLit) {
        out.kind = HirExprKind::BoolLit;
        out.type = HirTypeKind::Bool;
        out.bool_value = expr.bool_value;
        return out;
    }
    if (expr.kind == AstExprKind::IntLit) {
        out.kind = HirExprKind::IntLit;
        out.type = HirTypeKind::I32;
        out.int_value = expr.int_value;
        return out;
    }
    if (expr.kind == AstExprKind::StrLit) {
        out.kind = HirExprKind::StrLit;
        out.type = HirTypeKind::Str;
        out.str_value = expr.str_value;
        return out;
    }
    if (expr.kind == AstExprKind::Call)
        return analyze_call_expr(expr, route, mod, locals, local_count, binding, nullptr);
    if (expr.kind == AstExprKind::StructInit) {
        Str resolved_struct_name = expr.name;
        if (expr.lhs != nullptr) {
            if (expr.lhs->kind == AstExprKind::Ident) {
                if (!has_import_namespace(mod, expr.lhs->name) ||
                    !resolve_import_namespace_type_name(
                        mod, expr.lhs->name, expr.name, resolved_struct_name))
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            } else {
                if (!resolve_import_namespace_member(mod, *expr.lhs, resolved_struct_name))
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            }
        }
        const u32 struct_index = find_struct_index(mod, resolved_struct_name);
        if (struct_index == mod.structs.len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        u32 concrete_struct_index = struct_index;
        if (mod.structs[struct_index].type_params.len != 0) {
            GenericBinding bindings[HirStruct::kMaxTypeParams]{};
            if (expr.type_args.len != 0 &&
                expr.type_args.len != mod.structs[struct_index].type_params.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            if (expr.type_args.len != 0) {
                for (u32 i = 0; i < expr.type_args.len; i++) {
                    u32 type_variant_index = 0xffffffffu;
                    u32 type_struct_index = 0xffffffffu;
                    u32 tuple_len = 0;
                    HirTypeKind tuple_types[kMaxTupleSlots]{};
                    u32 tuple_variant_indices[kMaxTupleSlots]{};
                    u32 tuple_struct_indices[kMaxTupleSlots]{};
                    auto type_arg = resolve_func_type_ref(mod,
                                                          expr.type_args[i],
                                                          nullptr,
                                                          nullptr,
                                                          type_variant_index,
                                                          type_struct_index,
                                                          tuple_len,
                                                          tuple_types,
                                                          tuple_variant_indices,
                                                          tuple_struct_indices,
                                                          expr.span);
                    if (!type_arg) return core::make_unexpected(type_arg.error());
                    auto filled =
                        fill_bound_binding_from_type_metadata(&bindings[i],
                                                              const_cast<HirModule*>(&mod),
                                                              type_arg.value(),
                                                              0xffffffffu,
                                                              type_variant_index,
                                                              type_struct_index,
                                                              tuple_len,
                                                              tuple_types,
                                                              tuple_variant_indices,
                                                              tuple_struct_indices,
                                                              0xffffffffu,
                                                              expr.span);
                    if (!filled) return core::make_unexpected(filled.error());
                }
            } else {
                if (expr.field_inits.len != mod.structs[struct_index].fields.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
                for (u32 fi = 0; fi < expr.field_inits.len; fi++) {
                    u32 field_index = mod.structs[struct_index].fields.len;
                    for (u32 decl_i = 0; decl_i < mod.structs[struct_index].fields.len; decl_i++) {
                        if (mod.structs[struct_index].fields[decl_i].name.eq(
                                expr.field_inits[fi].name)) {
                            field_index = decl_i;
                            break;
                        }
                    }
                    if (field_index == mod.structs[struct_index].fields.len)
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
                    const auto& field_decl = mod.structs[struct_index].fields[field_index];
                    auto field_value = analyze_expr(
                        *expr.field_inits[fi].value, route, mod, locals, local_count, binding);
                    if (!field_value) return core::make_unexpected(field_value.error());
                    if (field_value->may_nil || field_value->may_error)
                        return frontend_error(FrontendError::UnsupportedSyntax,
                                              expr.field_inits[fi].value->span);
                    bool bound = false;
                    if (field_decl.generic_index != 0xffffffffu) {
                        bound = bind_generic_shape(bindings,
                                                   mod.structs[struct_index].type_params.len,
                                                   field_decl.generic_index,
                                                   field_value.value());
                    } else if (field_decl.template_variant_index != 0xffffffffu ||
                               field_decl.template_struct_index != 0xffffffffu) {
                        const auto expected =
                            make_expected_type_expr(field_decl.type,
                                                    field_decl.variant_index,
                                                    field_decl.struct_index,
                                                    field_decl.tuple_len,
                                                    field_decl.tuple_types,
                                                    field_decl.tuple_variant_indices,
                                                    field_decl.tuple_struct_indices,
                                                    field_decl.shape_index);
                        bound = bind_named_generic_shape(mod,
                                                         bindings,
                                                         mod.structs[struct_index].type_params.len,
                                                         expected,
                                                         field_value.value());
                    } else {
                        bound = true;
                    }
                    if (!bound)
                        return frontend_error(FrontendError::UnsupportedSyntax,
                                              expr.field_inits[fi].value->span);
                }
            }
            for (u32 i = 0; i < mod.structs[struct_index].type_params.len; i++) {
                if (!bindings[i].bound)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            }
            auto concrete_index = instantiate_struct(const_cast<HirModule*>(&mod),
                                                     struct_index,
                                                     bindings,
                                                     mod.structs[struct_index].type_params.len,
                                                     expr.span);
            if (!concrete_index) return core::make_unexpected(concrete_index.error());
            concrete_struct_index = concrete_index.value();
        } else if (expr.type_args.len != 0) {
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        }
        if (expr.field_inits.len != mod.structs[concrete_struct_index].fields.len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        out.kind = HirExprKind::StructInit;
        out.type = HirTypeKind::Struct;
        out.struct_index = concrete_struct_index;
        out.str_value = resolved_struct_name;
        auto struct_shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                                  out.type,
                                                  out.generic_index,
                                                  out.variant_index,
                                                  out.struct_index,
                                                  out.tuple_len,
                                                  out.tuple_types,
                                                  out.tuple_variant_indices,
                                                  out.tuple_struct_indices,
                                                  expr.span);
        if (!struct_shape) return core::make_unexpected(struct_shape.error());
        out.shape_index = struct_shape.value();
        for (u32 fi = 0; fi < expr.field_inits.len; fi++) {
            for (u32 seen = 0; seen < fi; seen++) {
                if (expr.field_inits[seen].name.eq(expr.field_inits[fi].name))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
            }
            u32 field_index = mod.structs[concrete_struct_index].fields.len;
            for (u32 decl_i = 0; decl_i < mod.structs[concrete_struct_index].fields.len; decl_i++) {
                if (mod.structs[concrete_struct_index].fields[decl_i].name.eq(
                        expr.field_inits[fi].name)) {
                    field_index = decl_i;
                    break;
                }
            }
            if (field_index == mod.structs[concrete_struct_index].fields.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
            const auto& field_decl = mod.structs[concrete_struct_index].fields[field_index];
            auto field_value =
                analyze_expr(*expr.field_inits[fi].value, route, mod, locals, local_count, binding);
            if (!field_value) return core::make_unexpected(field_value.error());
            if (field_value->may_nil || field_value->may_error)
                return frontend_error(FrontendError::UnsupportedSyntax,
                                      expr.field_inits[fi].value->span);
            const auto expected = make_expected_type_expr(field_decl.type,
                                                          field_decl.variant_index,
                                                          field_decl.struct_index,
                                                          field_decl.tuple_len,
                                                          field_decl.tuple_types,
                                                          field_decl.tuple_variant_indices,
                                                          field_decl.tuple_struct_indices,
                                                          field_decl.shape_index);
            if (!same_hir_type_shape(mod, field_value.value(), expected))
                return frontend_error(FrontendError::UnsupportedSyntax,
                                      expr.field_inits[fi].value->span);
            if (!route->exprs.push(field_value.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            HirExpr::FieldInit field_init{};
            field_init.name = expr.field_inits[fi].name;
            field_init.value = &route->exprs[route->exprs.len - 1];
            if (!out.field_inits.push(field_init))
                return frontend_error(FrontendError::TooManyItems, expr.span);
        }
        return out;
    }
    if (expr.kind == AstExprKind::MethodCall) {
        return analyze_method_call_expr(expr, route, mod, locals, local_count, binding);
    }
    if (expr.kind == AstExprKind::Field) {
        if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (expr.lhs->kind == AstExprKind::Ident) {
            const u32 variant_index = [&]() {
                for (u32 i = 0; i < mod.variants.len; i++) {
                    if (mod.variants[i].name.eq(expr.lhs->name)) return i;
                }
                return mod.variants.len;
            }();
            if (variant_index < mod.variants.len) {
                AstExpr variant_expr{};
                variant_expr.kind = AstExprKind::VariantCase;
                variant_expr.span = expr.span;
                variant_expr.name = expr.lhs->name;
                variant_expr.str_value = expr.name;
                return analyze_expr(variant_expr, route, mod, locals, local_count, binding);
            }
        }
        Str qualified_type_name{};
        if (resolve_import_namespace_member(mod, *expr.lhs, qualified_type_name)) {
            AstExpr variant_expr{};
            variant_expr.kind = AstExprKind::VariantCase;
            variant_expr.span = expr.span;
            variant_expr.name = qualified_type_name;
            variant_expr.type_args = expr.type_args;
            variant_expr.str_value = expr.name;
            return analyze_expr(variant_expr, route, mod, locals, local_count, binding);
        }
        auto base = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
        if (!base) return core::make_unexpected(base.error());
        const auto known_state = known_value_state(base.value(), locals, local_count, 0);
        if (known_state == KnownValueState::Error && !expr.name.eq({"file", 4}) &&
            !expr.name.eq({"func", 4}))
            return analyze_known_error_field(
                base.value(), expr.name, expr.span, mod, locals, local_count, 0);
        if (base->type == HirTypeKind::Struct && !base->may_error) {
            if (base->struct_index >= mod.structs.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            const auto* field = find_struct_field(mod.structs[base->struct_index], expr.name);
            if (!field)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            out.kind = HirExprKind::Field;
            out.span = expr.span;
            out.type = field->type;
            out.generic_index = field->generic_index;
            out.generic_has_error_constraint = field->generic_has_error_constraint;
            out.generic_has_eq_constraint = field->generic_has_eq_constraint;
            out.generic_has_ord_constraint = field->generic_has_ord_constraint;
            out.generic_protocol_index = field->generic_protocol_index;
            out.generic_protocol_count = field->generic_protocol_count;
            for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                out.generic_protocol_indices[cpi] = field->generic_protocol_indices[cpi];
            out.variant_index = field->variant_index;
            out.struct_index = field->struct_index;
            out.shape_index = field->shape_index;
            out.tuple_len = field->tuple_len;
            for (u32 i = 0; i < field->tuple_len; i++) {
                out.tuple_types[i] = field->tuple_types[i];
                out.tuple_variant_indices[i] = field->tuple_variant_indices[i];
                out.tuple_struct_indices[i] = field->tuple_struct_indices[i];
            }
            if (!route->exprs.push(base.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            out.lhs = &route->exprs[route->exprs.len - 1];
            out.str_value = expr.name;
            return out;
        }
        if (base->type == HirTypeKind::Generic && !base->may_error) {
            if (!base->generic_has_error_constraint)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            if (!is_standard_error_field(expr.name))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            out.kind = HirExprKind::Field;
            out.span = expr.span;
            out.type = expr.name.eq({"code", 4}) || expr.name.eq({"line", 4}) ? HirTypeKind::I32
                                                                              : HirTypeKind::Str;
            if (!route->exprs.push(base.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            out.lhs = &route->exprs[route->exprs.len - 1];
            out.str_value = expr.name;
            return out;
        }
        if (!base->may_error) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (is_standard_error_field(expr.name)) {
            out.kind = HirExprKind::Field;
            out.span = expr.span;
            out.type = expr.name.eq({"code", 4}) || expr.name.eq({"line", 4}) ? HirTypeKind::I32
                                                                              : HirTypeKind::Str;
            if (!route->exprs.push(base.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            out.lhs = &route->exprs[route->exprs.len - 1];
            out.str_value = expr.name;
            return out;
        }
        const HirStruct* st = error_struct_decl(mod, base->error_struct_index);
        if (!st) return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        const auto* field = find_struct_field(*st, expr.name);
        if (!field) return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        if (field->is_error_type)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        out.kind = HirExprKind::Field;
        out.span = expr.span;
        out.type = field->type;
        out.generic_index = field->generic_index;
        out.generic_has_error_constraint = field->generic_has_error_constraint;
        out.generic_has_eq_constraint = field->generic_has_eq_constraint;
        out.generic_has_ord_constraint = field->generic_has_ord_constraint;
        out.generic_protocol_index = field->generic_protocol_index;
        out.generic_protocol_count = field->generic_protocol_count;
        for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
            out.generic_protocol_indices[cpi] = field->generic_protocol_indices[cpi];
        out.variant_index = field->variant_index;
        out.struct_index = field->struct_index;
        out.shape_index = field->shape_index;
        out.tuple_len = field->tuple_len;
        for (u32 i = 0; i < field->tuple_len; i++) {
            out.tuple_types[i] = field->tuple_types[i];
            out.tuple_variant_indices[i] = field->tuple_variant_indices[i];
            out.tuple_struct_indices[i] = field->tuple_struct_indices[i];
        }
        if (!route->exprs.push(base.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &route->exprs[route->exprs.len - 1];
        out.str_value = expr.name;
        out.error_struct_index = base->error_struct_index;
        return out;
    }
    if (expr.kind == AstExprKind::VariantCase) {
        if (expr.name.len == 0) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        u32 variant_index = mod.variants.len;
        for (u32 i = 0; i < mod.variants.len; i++) {
            if (mod.variants[i].name.eq(expr.name)) {
                variant_index = i;
                break;
            }
        }
        if (variant_index == mod.variants.len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        if (mod.variants[variant_index].type_params.len != 0) {
            GenericBinding bindings[HirVariant::kMaxTypeParams]{};
            if (expr.type_args.len != 0 &&
                expr.type_args.len != mod.variants[variant_index].type_params.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            u32 template_case_index = mod.variants[variant_index].cases.len;
            for (u32 i = 0; i < mod.variants[variant_index].cases.len; i++) {
                if (mod.variants[variant_index].cases[i].name.eq(expr.str_value)) {
                    template_case_index = i;
                    break;
                }
            }
            if (template_case_index == mod.variants[variant_index].cases.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
            const auto& template_case_decl = mod.variants[variant_index].cases[template_case_index];
            if (expr.type_args.len != 0) {
                for (u32 i = 0; i < expr.type_args.len; i++) {
                    u32 type_variant_index = 0xffffffffu;
                    u32 type_struct_index = 0xffffffffu;
                    u32 tuple_len = 0;
                    HirTypeKind tuple_types[kMaxTupleSlots]{};
                    u32 tuple_variant_indices[kMaxTupleSlots]{};
                    u32 tuple_struct_indices[kMaxTupleSlots]{};
                    auto type_arg = resolve_func_type_ref(mod,
                                                          expr.type_args[i],
                                                          nullptr,
                                                          nullptr,
                                                          type_variant_index,
                                                          type_struct_index,
                                                          tuple_len,
                                                          tuple_types,
                                                          tuple_variant_indices,
                                                          tuple_struct_indices,
                                                          expr.span);
                    if (!type_arg) return core::make_unexpected(type_arg.error());
                    auto filled =
                        fill_bound_binding_from_type_metadata(&bindings[i],
                                                              const_cast<HirModule*>(&mod),
                                                              type_arg.value(),
                                                              0xffffffffu,
                                                              type_variant_index,
                                                              type_struct_index,
                                                              tuple_len,
                                                              tuple_types,
                                                              tuple_variant_indices,
                                                              tuple_struct_indices,
                                                              0xffffffffu,
                                                              expr.span);
                    if (!filled) return core::make_unexpected(filled.error());
                }
            } else {
                if (!template_case_decl.has_payload || expr.lhs == nullptr)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
                auto payload = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
                if (!payload) return core::make_unexpected(payload.error());
                if (payload->may_nil || payload->may_error)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
                bool bound = false;
                if (template_case_decl.payload_generic_index != 0xffffffffu) {
                    bound = bind_generic_shape(bindings,
                                               mod.variants[variant_index].type_params.len,
                                               template_case_decl.payload_generic_index,
                                               payload.value());
                } else if (template_case_decl.payload_template_variant_index != 0xffffffffu ||
                           template_case_decl.payload_template_struct_index != 0xffffffffu) {
                    const auto expected =
                        make_expected_type_expr(template_case_decl.payload_type,
                                                template_case_decl.payload_variant_index,
                                                template_case_decl.payload_struct_index,
                                                template_case_decl.payload_tuple_len,
                                                template_case_decl.payload_tuple_types,
                                                template_case_decl.payload_tuple_variant_indices,
                                                template_case_decl.payload_tuple_struct_indices,
                                                template_case_decl.payload_shape_index);
                    bound = bind_named_generic_shape(mod,
                                                     bindings,
                                                     mod.variants[variant_index].type_params.len,
                                                     expected,
                                                     payload.value());
                }
                if (!bound) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
                for (u32 i = 0; i < mod.variants[variant_index].type_params.len; i++) {
                    if (!bindings[i].bound)
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, expr.span, expr.name);
                }
            }
            auto concrete_index = instantiate_variant(const_cast<HirModule*>(&mod),
                                                      variant_index,
                                                      bindings,
                                                      mod.variants[variant_index].type_params.len,
                                                      expr.span);
            if (!concrete_index) return core::make_unexpected(concrete_index.error());
            variant_index = concrete_index.value();
        } else if (expr.type_args.len != 0) {
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        }
        u32 case_index = mod.variants[variant_index].cases.len;
        for (u32 i = 0; i < mod.variants[variant_index].cases.len; i++) {
            if (mod.variants[variant_index].cases[i].name.eq(expr.str_value)) {
                case_index = i;
                break;
            }
        }
        if (case_index == mod.variants[variant_index].cases.len)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.str_value);
        const auto& case_decl = mod.variants[variant_index].cases[case_index];
        if (case_decl.has_payload) {
            if (expr.lhs == nullptr)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            auto payload = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
            if (!payload) return core::make_unexpected(payload.error());
            if (payload->may_nil || payload->may_error)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            const auto expected = make_expected_type_expr(case_decl.payload_type,
                                                          case_decl.payload_variant_index,
                                                          case_decl.payload_struct_index,
                                                          case_decl.payload_tuple_len,
                                                          case_decl.payload_tuple_types,
                                                          case_decl.payload_tuple_variant_indices,
                                                          case_decl.payload_tuple_struct_indices,
                                                          case_decl.payload_shape_index);
            if (!same_hir_type_shape(mod, payload.value(), expected))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            if (!route->exprs.push(payload.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            out.lhs = &route->exprs[route->exprs.len - 1];
        } else if (expr.lhs != nullptr) {
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        }
        out.kind = HirExprKind::VariantCase;
        out.type = HirTypeKind::Variant;
        out.variant_index = variant_index;
        out.case_index = case_index;
        out.int_value = static_cast<i32>(case_index);
        out.str_value = expr.str_value;
        auto variant_shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                                   out.type,
                                                   out.generic_index,
                                                   out.variant_index,
                                                   out.struct_index,
                                                   out.tuple_len,
                                                   out.tuple_types,
                                                   out.tuple_variant_indices,
                                                   out.tuple_struct_indices,
                                                   expr.span);
        if (!variant_shape) return core::make_unexpected(variant_shape.error());
        out.shape_index = variant_shape.value();
        return out;
    }
    if (expr.kind == AstExprKind::Tuple) {
        if (expr.args.len < 2) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        out.kind = HirExprKind::Tuple;
        out.type = HirTypeKind::Tuple;
        out.tuple_len = expr.args.len;
        for (u32 i = 0; i < expr.args.len; i++) {
            auto elem = analyze_expr(*expr.args[i], route, mod, locals, local_count, binding);
            if (!elem) return core::make_unexpected(elem.error());
            if (elem->may_nil || elem->may_error || elem->type == HirTypeKind::Unknown ||
                elem->type == HirTypeKind::Tuple)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            out.tuple_types[i] = elem->type;
            out.tuple_variant_indices[i] = elem->variant_index;
            out.tuple_struct_indices[i] = elem->struct_index;
            if (!route->exprs.push(elem.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            if (!out.args.push(&route->exprs[route->exprs.len - 1]))
                return frontend_error(FrontendError::TooManyItems, expr.span);
        }
        auto tuple_shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                                 out.type,
                                                 out.generic_index,
                                                 out.variant_index,
                                                 out.struct_index,
                                                 out.tuple_len,
                                                 out.tuple_types,
                                                 out.tuple_variant_indices,
                                                 out.tuple_struct_indices,
                                                 expr.span);
        if (!tuple_shape) return core::make_unexpected(tuple_shape.error());
        out.shape_index = tuple_shape.value();
        return out;
    }
    if (expr.kind == AstExprKind::ReqHeader) {
        out.kind = HirExprKind::ReqHeader;
        out.type = HirTypeKind::Str;
        out.may_nil = true;
        out.str_value = expr.str_value;
        return out;
    }
    if (expr.kind == AstExprKind::Nil) {
        out.kind = HirExprKind::Nil;
        out.type = HirTypeKind::Unknown;
        out.may_nil = true;
        return out;
    }
    if (expr.kind == AstExprKind::Error) {
        out.kind = HirExprKind::Error;
        out.type = HirTypeKind::Unknown;
        out.may_error = true;
        out.msg = expr.msg;
        if (expr.name.len != 0) {
            const u32 struct_index = find_struct_index(mod, expr.name);
            if (struct_index == mod.structs.len)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            if (!mod.structs[struct_index].conforms_error)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            out.error_struct_index = struct_index;
            u32 required_extra_fields = 0;
            for (u32 fi = 0; fi < mod.structs[struct_index].fields.len; fi++) {
                if (mod.structs[struct_index].fields[fi].name.eq({"err", 3}) &&
                    mod.structs[struct_index].fields[fi].is_error_type)
                    continue;
                required_extra_fields++;
            }
            if (expr.field_inits.len != required_extra_fields)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
            for (u32 fi = 0; fi < expr.field_inits.len; fi++) {
                for (u32 seen = 0; seen < fi; seen++) {
                    if (expr.field_inits[seen].name.eq(expr.field_inits[fi].name))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
                }
                u32 field_index = mod.structs[struct_index].fields.len;
                for (u32 decl_i = 0; decl_i < mod.structs[struct_index].fields.len; decl_i++) {
                    if (mod.structs[struct_index].fields[decl_i].name.eq(
                            expr.field_inits[fi].name)) {
                        field_index = decl_i;
                        break;
                    }
                }
                if (field_index == mod.structs[struct_index].fields.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
                const auto& field_decl = mod.structs[struct_index].fields[field_index];
                if (field_decl.name.eq({"err", 3}) && field_decl.is_error_type)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, expr.span, expr.field_inits[fi].name);
                auto field_value = analyze_expr(
                    *expr.field_inits[fi].value, route, mod, locals, local_count, binding);
                if (!field_value) return core::make_unexpected(field_value.error());
                if (field_value->may_nil || field_value->may_error)
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          expr.field_inits[fi].value->span);
                const auto expected = make_expected_type_expr(field_decl.type,
                                                              field_decl.variant_index,
                                                              field_decl.struct_index,
                                                              field_decl.tuple_len,
                                                              field_decl.tuple_types,
                                                              field_decl.tuple_variant_indices,
                                                              field_decl.tuple_struct_indices,
                                                              field_decl.shape_index);
                if (!same_hir_type_shape(mod, field_value.value(), expected))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          expr.field_inits[fi].value->span);
                if (!route->exprs.push(field_value.value()))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
                HirExpr::FieldInit field_init{};
                field_init.name = expr.field_inits[fi].name;
                field_init.value = &route->exprs[route->exprs.len - 1];
                if (!out.field_inits.push(field_init))
                    return frontend_error(FrontendError::TooManyItems, expr.span);
            }
        }
        if (expr.lhs == nullptr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (expr.lhs->kind == AstExprKind::IntLit) {
            out.int_value = expr.lhs->int_value;
            return out;
        }
        if (expr.lhs->kind == AstExprKind::VariantCase && expr.lhs->name.len == 0) {
            out.str_value = expr.lhs->str_value;
            if (route != nullptr && route->error_variant_index != 0xffffffffu) {
                out.error_variant_index = route->error_variant_index;
                const auto& variant = mod.variants[route->error_variant_index];
                u32 case_index = variant.cases.len;
                for (u32 i = 0; i < variant.cases.len; i++) {
                    if (variant.cases[i].name.eq(expr.lhs->str_value)) {
                        case_index = i;
                        break;
                    }
                }
                if (case_index == variant.cases.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, expr.span, expr.lhs->str_value);
                out.error_case_index = case_index;
                out.int_value = static_cast<i32>(case_index);
            }
            return out;
        }
        auto kind = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
        if (!kind) return core::make_unexpected(kind.error());
        if (kind->kind == HirExprKind::VariantCase && kind->lhs == nullptr) {
            out.int_value = kind->int_value;
            out.str_value = kind->str_value;
            out.error_variant_index = kind->variant_index;
            out.error_case_index = kind->case_index;
            return out;
        }
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
    }
    if (expr.kind == AstExprKind::Or) {
        auto lhs = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
        if (!lhs) return core::make_unexpected(lhs.error());
        auto rhs = analyze_expr(*expr.rhs, route, mod, locals, local_count, binding);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (rhs->may_nil || rhs->may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.rhs->span);
        if (lhs->type != HirTypeKind::Unknown &&
            !same_hir_type_shape(mod, lhs.value(), rhs.value()))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (lhs->kind == HirExprKind::Nil) {
            HirExpr folded = rhs.value();
            folded.span = expr.span;
            folded.may_nil = false;
            folded.may_error = false;
            return folded;
        }
        if (lhs->kind == HirExprKind::Error) {
            HirExpr folded = rhs.value();
            folded.span = expr.span;
            folded.may_nil = false;
            folded.may_error = false;
            return folded;
        }
        const auto lhs_state = known_value_state(lhs.value(), locals, local_count, 0);
        if (lhs_state == KnownValueState::Nil || lhs_state == KnownValueState::Error) {
            HirExpr folded = rhs.value();
            folded.span = expr.span;
            folded.may_nil = false;
            folded.may_error = false;
            return folded;
        }
        if (lhs_state == KnownValueState::Available) {
            HirExpr folded = lhs.value();
            folded.span = expr.span;
            return folded;
        }
        if (!route->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr* lhs_ptr = &route->exprs[route->exprs.len - 1];
        if (!route->exprs.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr* rhs_ptr = &route->exprs[route->exprs.len - 1];
        out.kind = HirExprKind::Or;
        out.type = lhs->type == HirTypeKind::Unknown ? rhs->type : lhs->type;
        out.lhs = lhs_ptr;
        out.rhs = rhs_ptr;
        auto shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                           out.type,
                                           out.generic_index,
                                           out.variant_index,
                                           out.struct_index,
                                           out.tuple_len,
                                           out.tuple_types,
                                           out.tuple_variant_indices,
                                           out.tuple_struct_indices,
                                           expr.span);
        if (!shape) return core::make_unexpected(shape.error());
        out.shape_index = shape.value();
        return out;
    }
    if (expr.kind == AstExprKind::Pipe) {
        if (expr.lhs == nullptr || expr.rhs == nullptr)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        auto lhs = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
        if (!lhs) return core::make_unexpected(lhs.error());
        if (expr.rhs->kind != AstExprKind::Call)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.rhs->span);
        return analyze_call_expr(*expr.rhs, route, mod, locals, local_count, binding, &lhs.value());
    }
    if (expr.kind == AstExprKind::Eq || expr.kind == AstExprKind::Lt ||
        expr.kind == AstExprKind::Gt) {
        auto lhs = analyze_expr(*expr.lhs, route, mod, locals, local_count, binding);
        if (!lhs) return core::make_unexpected(lhs.error());
        auto rhs = analyze_expr(*expr.rhs, route, mod, locals, local_count, binding);
        if (!rhs) return core::make_unexpected(rhs.error());
        if (lhs->may_nil || lhs->may_error || rhs->may_nil || rhs->may_error)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (lhs->type != rhs->type)
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        if (expr.kind == AstExprKind::Eq) {
            if (lhs->type == HirTypeKind::Generic && !lhs->generic_has_eq_constraint)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
        } else {
            if (lhs->type == HirTypeKind::Generic) {
                if (!lhs->generic_has_ord_constraint)
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            } else {
                bool struct_visiting[HirModule::kMaxStructs]{};
                bool variant_visiting[HirModule::kMaxVariants]{};
                if (!hir_type_shape_satisfies_ord_constraint(mod,
                                                             lhs->type,
                                                             lhs->variant_index,
                                                             lhs->struct_index,
                                                             lhs->tuple_len,
                                                             lhs->tuple_types,
                                                             lhs->tuple_variant_indices,
                                                             lhs->tuple_struct_indices,
                                                             struct_visiting,
                                                             variant_visiting)) {
                    return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
                }
            }
        }
        if (!route->exprs.push(lhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr* lhs_ptr = &route->exprs[route->exprs.len - 1];
        if (!route->exprs.push(rhs.value()))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        HirExpr* rhs_ptr = &route->exprs[route->exprs.len - 1];
        out.kind = expr.kind == AstExprKind::Eq
                       ? HirExprKind::Eq
                       : (expr.kind == AstExprKind::Lt ? HirExprKind::Lt : HirExprKind::Gt);
        out.type = HirTypeKind::Bool;
        out.lhs = lhs_ptr;
        out.rhs = rhs_ptr;
        return out;
    }
    for (u32 i = 0; i < local_count; i++) {
        if (locals[i].name.eq(expr.name)) {
            if (locals[i].type == HirTypeKind::Tuple) {
                HirExpr tuple = locals[i].init;
                tuple.span = expr.span;
                return tuple;
            }
            out.kind = HirExprKind::LocalRef;
            out.type = locals[i].type;
            out.generic_index = locals[i].generic_index;
            out.generic_has_error_constraint = locals[i].generic_has_error_constraint;
            out.generic_has_eq_constraint = locals[i].generic_has_eq_constraint;
            out.generic_has_ord_constraint = locals[i].generic_has_ord_constraint;
            out.generic_protocol_index = locals[i].generic_protocol_index;
            out.generic_protocol_count = locals[i].generic_protocol_count;
            for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                out.generic_protocol_indices[cpi] = locals[i].generic_protocol_indices[cpi];
            out.may_nil = locals[i].may_nil;
            out.may_error = locals[i].may_error;
            out.local_index = locals[i].ref_index;
            out.variant_index = locals[i].variant_index;
            out.shape_index = locals[i].shape_index;
            out.error_struct_index = locals[i].error_struct_index;
            out.struct_index = locals[i].struct_index;
            out.error_variant_index = locals[i].error_variant_index;
            out.shape_index = locals[i].shape_index;
            out.tuple_len = locals[i].tuple_len;
            for (u32 ti = 0; ti < locals[i].tuple_len; ti++) {
                out.tuple_types[ti] = locals[i].tuple_types[ti];
                out.tuple_variant_indices[ti] = locals[i].tuple_variant_indices[ti];
                out.tuple_struct_indices[ti] = locals[i].tuple_struct_indices[ti];
            }
            return out;
        }
    }
    if (binding && binding->subject && expr.name.eq(binding->name)) {
        out.kind = HirExprKind::MatchPayload;
        out.type = binding->type;
        out.generic_index = binding->generic_index;
        out.generic_has_error_constraint = binding->generic_has_error_constraint;
        out.generic_has_eq_constraint = binding->generic_has_eq_constraint;
        out.generic_has_ord_constraint = binding->generic_has_ord_constraint;
        out.generic_protocol_index = binding->generic_protocol_index;
        out.generic_protocol_count = binding->generic_protocol_count;
        for (u32 cpi = 0; cpi < binding->generic_protocol_count; cpi++)
            out.generic_protocol_indices[cpi] = binding->generic_protocol_indices[cpi];
        out.variant_index = binding->variant_index;
        out.struct_index = binding->struct_index;
        out.shape_index = binding->shape_index;
        out.case_index = binding->case_index;
        out.tuple_len = binding->tuple_len;
        for (u32 ti = 0; ti < binding->tuple_len; ti++) {
            out.tuple_types[ti] = binding->tuple_types[ti];
            out.tuple_variant_indices[ti] = binding->tuple_variant_indices[ti];
            out.tuple_struct_indices[ti] = binding->tuple_struct_indices[ti];
        }
        if (!route->exprs.push(*binding->subject))
            return frontend_error(FrontendError::TooManyItems, expr.span);
        out.lhs = &route->exprs[route->exprs.len - 1];
        return out;
    }
    return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
}

static FrontendResult<HirExpr> analyze_call_expr(const AstExpr& expr,
                                                 HirRoute* route,
                                                 const HirModule& mod,
                                                 const HirLocal* locals,
                                                 u32 local_count,
                                                 const MatchPayloadBinding* binding,
                                                 const HirExpr* pipe_lhs) {
    const Str callee_name = resolve_alias_target_leaf(mod, expr.name);
    const u32 fn_index = find_function_index(mod, callee_name);
    if (fn_index == mod.functions.len)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
    const auto& fn = mod.functions[fn_index];
    if (expr.args.len != fn.params.len)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
    if (expr.type_args.len != 0 && expr.type_args.len != fn.type_params.len)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
    GenericBinding generic_bindings[HirFunction::kMaxTypeParams]{};
    for (u32 i = 0; i < expr.type_args.len; i++) {
        u32 variant_index = 0xffffffffu;
        u32 struct_index = 0xffffffffu;
        u32 tuple_len = 0;
        HirTypeKind tuple_types[kMaxTupleSlots]{};
        u32 tuple_variant_indices[kMaxTupleSlots]{};
        u32 tuple_struct_indices[kMaxTupleSlots]{};
        auto type_arg = resolve_func_type_ref(mod,
                                              expr.type_args[i],
                                              nullptr,
                                              nullptr,
                                              variant_index,
                                              struct_index,
                                              tuple_len,
                                              tuple_types,
                                              tuple_variant_indices,
                                              tuple_struct_indices,
                                              expr.span);
        if (!type_arg) return core::make_unexpected(type_arg.error());
        auto filled = fill_bound_binding_from_type_metadata(&generic_bindings[i],
                                                            const_cast<HirModule*>(&mod),
                                                            type_arg.value(),
                                                            0xffffffffu,
                                                            variant_index,
                                                            struct_index,
                                                            tuple_len,
                                                            tuple_types,
                                                            tuple_variant_indices,
                                                            tuple_struct_indices,
                                                            0xffffffffu,
                                                            expr.span);
        if (!filled) return core::make_unexpected(filled.error());
        if (fn.type_params[i].has_error_constraint &&
            !generic_binding_satisfies_error_constraint(mod, generic_bindings[i]))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        if (fn.type_params[i].has_eq_constraint &&
            !generic_binding_satisfies_eq_constraint(mod, generic_bindings[i]))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
        if (fn.type_params[i].has_ord_constraint &&
            !generic_binding_satisfies_ord_constraint(mod, generic_bindings[i]))
            return frontend_error(FrontendError::UnsupportedSyntax, expr.span, expr.name);
    }
    auto concrete_return_expr = [&]() -> FrontendResult<HirExpr> {
        HirExpr expected{};
        expected.type = fn.return_type;
        expected.generic_index = fn.return_generic_index;
        expected.shape_index = fn.return_shape_index;
        if (fn.return_generic_index < fn.type_params.len)
            expected.generic_has_error_constraint =
                fn.type_params[fn.return_generic_index].has_error_constraint;
        if (fn.return_generic_index < fn.type_params.len) {
            expected.generic_has_eq_constraint =
                fn.type_params[fn.return_generic_index].has_eq_constraint;
            expected.generic_has_ord_constraint =
                fn.type_params[fn.return_generic_index].has_ord_constraint;
            expected.generic_protocol_index =
                fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                    ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                    : 0xffffffffu;
            expected.generic_protocol_count =
                fn.type_params[fn.return_generic_index].custom_protocol_count;
            for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
                expected.generic_protocol_indices[cpi] =
                    fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
        }
        expected.variant_index = fn.return_variant_index;
        expected.struct_index = fn.return_struct_index;
        expected.tuple_len = fn.return_tuple_len;
        for (u32 i = 0; i < fn.return_tuple_len; i++) {
            expected.tuple_types[i] = fn.return_tuple_types[i];
            expected.tuple_variant_indices[i] = fn.return_tuple_variant_indices[i];
            expected.tuple_struct_indices[i] = fn.return_tuple_struct_indices[i];
        }
        auto out = apply_generic_binding_to_expr(expected, generic_bindings, fn.type_params.len);
        auto concretized =
            concretize_named_instance_shape(&out, mod, generic_bindings, fn.type_params.len);
        if (!concretized) return core::make_unexpected(concretized.error());
        return out;
    };
    auto placeholder_slot_expr =
        [&](const HirExpr& source, i32 slot, Span span) -> FrontendResult<HirExpr> {
        if (slot <= 0 || slot > static_cast<i32>(kMaxTupleSlots))
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        if (source.type != HirTypeKind::Tuple) {
            if (slot != 1) return frontend_error(FrontendError::UnsupportedSyntax, span);
            return source;
        }
        if (slot > static_cast<i32>(source.tuple_len))
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        const u32 slot_index = static_cast<u32>(slot - 1);
        if (source.args.len == source.tuple_len && source.args[slot_index] != nullptr)
            return *source.args[slot_index];
        HirExpr out{};
        out.kind = HirExprKind::TupleSlot;
        out.type = source.tuple_types[slot_index];
        if (out.type == HirTypeKind::Struct)
            out.struct_index = source.tuple_struct_indices[slot_index];
        else
            out.variant_index = source.tuple_variant_indices[slot_index];
        auto shape = intern_hir_type_shape(const_cast<HirModule*>(&mod),
                                           out.type,
                                           out.generic_index,
                                           out.variant_index,
                                           out.struct_index,
                                           out.tuple_len,
                                           out.tuple_types,
                                           out.tuple_variant_indices,
                                           out.tuple_struct_indices,
                                           span);
        if (!shape) return core::make_unexpected(shape.error());
        out.shape_index = shape.value();
        out.span = span;
        out.int_value = static_cast<i32>(slot_index);
        out.lhs = const_cast<HirExpr*>(&source);
        return out;
    };
    auto concrete_param_expr = [&](const HirFunction::ParamDecl& param) -> FrontendResult<HirExpr> {
        auto expected = make_expected_param_expr(param);
        auto concretized =
            concretize_named_instance_shape(&expected, mod, generic_bindings, fn.type_params.len);
        if (!concretized) return core::make_unexpected(concretized.error());
        return expected;
    };
    if (pipe_lhs != nullptr) {
        const auto lhs_state = known_value_state(*pipe_lhs, locals, local_count, 0);
        if (lhs_state == KnownValueState::Nil) {
            HirExpr folded{};
            folded.kind = HirExprKind::Nil;
            auto ret = concrete_return_expr();
            if (!ret) return core::make_unexpected(ret.error());
            folded.type = ret->type;
            folded.generic_index = ret->generic_index;
            folded.generic_has_error_constraint = ret->generic_has_error_constraint;
            folded.generic_has_eq_constraint = ret->generic_has_eq_constraint;
            folded.generic_has_ord_constraint = ret->generic_has_ord_constraint;
            folded.generic_protocol_index = ret->generic_protocol_index;
            folded.generic_protocol_count = ret->generic_protocol_count;
            for (u32 cpi = 0; cpi < folded.generic_protocol_count; cpi++)
                folded.generic_protocol_indices[cpi] = ret->generic_protocol_indices[cpi];
            folded.variant_index = ret->variant_index;
            folded.struct_index = ret->struct_index;
            folded.shape_index = ret->shape_index;
            folded.tuple_len = ret->tuple_len;
            for (u32 i = 0; i < ret->tuple_len; i++) {
                folded.tuple_types[i] = ret->tuple_types[i];
                folded.tuple_variant_indices[i] = ret->tuple_variant_indices[i];
                folded.tuple_struct_indices[i] = ret->tuple_struct_indices[i];
            }
            folded.span = expr.span;
            folded.may_nil = true;
            return folded;
        }
        if (lhs_state == KnownValueState::Error) {
            const HirExpr* err_expr = known_error_expr(*pipe_lhs, locals, local_count, 0);
            if (!err_expr) return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            HirExpr folded = *err_expr;
            auto ret = concrete_return_expr();
            if (!ret) return core::make_unexpected(ret.error());
            folded.type = ret->type;
            folded.generic_index = ret->generic_index;
            folded.generic_has_error_constraint = ret->generic_has_error_constraint;
            folded.generic_has_eq_constraint = ret->generic_has_eq_constraint;
            folded.generic_has_ord_constraint = ret->generic_has_ord_constraint;
            folded.generic_protocol_index = ret->generic_protocol_index;
            folded.generic_protocol_count = ret->generic_protocol_count;
            for (u32 cpi = 0; cpi < folded.generic_protocol_count; cpi++)
                folded.generic_protocol_indices[cpi] = ret->generic_protocol_indices[cpi];
            folded.variant_index = ret->variant_index;
            folded.struct_index = ret->struct_index;
            folded.shape_index = ret->shape_index;
            folded.tuple_len = ret->tuple_len;
            for (u32 i = 0; i < ret->tuple_len; i++) {
                folded.tuple_types[i] = ret->tuple_types[i];
                folded.tuple_variant_indices[i] = ret->tuple_variant_indices[i];
                folded.tuple_struct_indices[i] = ret->tuple_struct_indices[i];
            }
            folded.span = expr.span;
            folded.may_nil = false;
            folded.may_error = true;
            return folded;
        }
        if (lhs_state == KnownValueState::Unknown && (pipe_lhs->may_nil || pipe_lhs->may_error)) {
            u32 placeholder_count = 0;
            for (u32 i = 0; i < expr.args.len; i++) {
                const auto& arg_expr = *expr.args[i];
                if (arg_expr.kind == AstExprKind::Placeholder) {
                    placeholder_count++;
                    if (arg_expr.int_value != 1)
                        return frontend_error(FrontendError::UnsupportedSyntax, arg_expr.span);
                }
            }
            if (!route->exprs.push(*pipe_lhs))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            HirExpr* lhs_ptr = &route->exprs[route->exprs.len - 1];

            HirExpr unwrapped{};
            unwrapped.kind = HirExprKind::ValueOf;
            unwrapped.type = pipe_lhs->type;
            unwrapped.generic_index = pipe_lhs->generic_index;
            unwrapped.generic_has_error_constraint = pipe_lhs->generic_has_error_constraint;
            unwrapped.generic_has_eq_constraint = pipe_lhs->generic_has_eq_constraint;
            unwrapped.generic_has_ord_constraint = pipe_lhs->generic_has_ord_constraint;
            unwrapped.generic_protocol_index = pipe_lhs->generic_protocol_index;
            unwrapped.generic_protocol_count = pipe_lhs->generic_protocol_count;
            for (u32 cpi = 0; cpi < unwrapped.generic_protocol_count; cpi++)
                unwrapped.generic_protocol_indices[cpi] = pipe_lhs->generic_protocol_indices[cpi];
            unwrapped.variant_index = pipe_lhs->variant_index;
            unwrapped.struct_index = pipe_lhs->struct_index;
            unwrapped.tuple_len = pipe_lhs->tuple_len;
            for (u32 i = 0; i < pipe_lhs->tuple_len; i++) {
                unwrapped.tuple_types[i] = pipe_lhs->tuple_types[i];
                unwrapped.tuple_variant_indices[i] = pipe_lhs->tuple_variant_indices[i];
                unwrapped.tuple_struct_indices[i] = pipe_lhs->tuple_struct_indices[i];
            }
            unwrapped.shape_index = pipe_lhs->shape_index;
            unwrapped.error_struct_index = pipe_lhs->error_struct_index;
            unwrapped.error_variant_index = pipe_lhs->error_variant_index;
            unwrapped.span = expr.span;
            unwrapped.lhs = lhs_ptr;

            HirExpr analyzed_args[AstExpr::kMaxArgs]{};
            for (u32 i = 0; i < expr.args.len; i++) {
                const auto& arg_expr = *expr.args[i];
                if (arg_expr.kind == AstExprKind::Placeholder) {
                    analyzed_args[i] = unwrapped;
                } else {
                    auto arg = analyze_expr(arg_expr, route, mod, locals, local_count, binding);
                    if (!arg) return core::make_unexpected(arg.error());
                    if (arg->may_nil || arg->may_error)
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    analyzed_args[i] = arg.value();
                }
                if (fn.params[i].type == HirTypeKind::Generic) {
                    if (!bind_generic_shape(generic_bindings,
                                            fn.type_params.len,
                                            fn.params[i].generic_index,
                                            analyzed_args[i]))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    if (fn.type_params[fn.params[i].generic_index].has_error_constraint &&
                        !generic_binding_satisfies_error_constraint(
                            mod, generic_bindings[fn.params[i].generic_index]))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    if (fn.type_params[fn.params[i].generic_index].has_eq_constraint &&
                        !generic_binding_satisfies_eq_constraint(
                            mod, generic_bindings[fn.params[i].generic_index]))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    if (fn.type_params[fn.params[i].generic_index].has_ord_constraint &&
                        !generic_binding_satisfies_ord_constraint(
                            mod, generic_bindings[fn.params[i].generic_index]))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    if (fn.type_params[fn.params[i].generic_index].has_constraint &&
                        fn.type_params[fn.params[i].generic_index].constraint_kind ==
                            HirProtocolKind::Custom &&
                        !generic_binding_satisfies_custom_protocol(
                            mod,
                            generic_bindings[fn.params[i].generic_index],
                            find_protocol_index(
                                mod, fn.type_params[fn.params[i].generic_index].constraint)))
                        return frontend_error(
                            FrontendError::UnsupportedSyntax,
                            expr.args[i]->span,
                            fn.type_params[fn.params[i].generic_index].constraint);
                } else if (fn.params[i].template_variant_index != 0xffffffffu ||
                           fn.params[i].template_struct_index != 0xffffffffu) {
                    const auto expected_shape = make_expected_param_expr(fn.params[i]);
                    if (!bind_named_generic_shape(mod,
                                                  generic_bindings,
                                                  fn.type_params.len,
                                                  expected_shape,
                                                  analyzed_args[i]))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                    auto expected = concrete_param_expr(fn.params[i]);
                    if (!expected) return core::make_unexpected(expected.error());
                    if (!same_hir_type_shape(mod, analyzed_args[i], expected.value()))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                } else {
                    auto expected = concrete_param_expr(fn.params[i]);
                    if (!expected) return core::make_unexpected(expected.error());
                    if (!same_hir_type_shape(mod, analyzed_args[i], expected.value()))
                        return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
                }
            }
            if (placeholder_count != 1)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);

            auto ret = concrete_return_expr();
            if (!ret) return core::make_unexpected(ret.error());
            if (ret->type != HirTypeKind::I32 && ret->type != HirTypeKind::Str &&
                ret->type != HirTypeKind::Variant)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);

            auto then_expr = instantiate_function_expr(fn.body,
                                                       route,
                                                       mod,
                                                       analyzed_args,
                                                       expr.args.len,
                                                       generic_bindings,
                                                       fn.type_params.len);
            if (!then_expr) return core::make_unexpected(then_expr.error());
            if (pipe_lhs->may_error && then_expr->may_error &&
                (pipe_lhs->error_struct_index != then_expr->error_struct_index ||
                 pipe_lhs->error_variant_index != then_expr->error_variant_index))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
            if (!route->exprs.push(then_expr.value()))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            HirExpr* then_ptr = &route->exprs[route->exprs.len - 1];

            HirExpr cond{};
            cond.kind = HirExprKind::HasValue;
            cond.type = HirTypeKind::Bool;
            cond.span = expr.span;
            cond.lhs = lhs_ptr;
            if (!route->exprs.push(cond))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            HirExpr* cond_ptr = &route->exprs[route->exprs.len - 1];

            HirExpr missing{};
            missing.kind = HirExprKind::MissingOf;
            missing.type = ret->type;
            missing.generic_index = ret->generic_index;
            missing.generic_has_error_constraint = ret->generic_has_error_constraint;
            missing.generic_has_eq_constraint = ret->generic_has_eq_constraint;
            missing.generic_has_ord_constraint = ret->generic_has_ord_constraint;
            missing.generic_protocol_index = ret->generic_protocol_index;
            missing.generic_protocol_count = ret->generic_protocol_count;
            for (u32 cpi = 0; cpi < missing.generic_protocol_count; cpi++)
                missing.generic_protocol_indices[cpi] = ret->generic_protocol_indices[cpi];
            missing.variant_index = ret->variant_index;
            missing.struct_index = ret->struct_index;
            missing.tuple_len = ret->tuple_len;
            for (u32 i = 0; i < ret->tuple_len; i++) {
                missing.tuple_types[i] = ret->tuple_types[i];
                missing.tuple_variant_indices[i] = ret->tuple_variant_indices[i];
                missing.tuple_struct_indices[i] = ret->tuple_struct_indices[i];
            }
            missing.span = expr.span;
            missing.shape_index = ret->shape_index;
            missing.may_nil = pipe_lhs->may_nil || then_expr->may_nil;
            missing.may_error = pipe_lhs->may_error || then_expr->may_error;
            missing.error_struct_index =
                then_expr->may_error ? then_expr->error_struct_index : pipe_lhs->error_struct_index;
            missing.error_variant_index = then_expr->may_error ? then_expr->error_variant_index
                                                               : pipe_lhs->error_variant_index;
            missing.lhs = lhs_ptr;
            if (!route->exprs.push(missing))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            HirExpr* else_ptr = &route->exprs[route->exprs.len - 1];

            HirExpr out{};
            out.kind = HirExprKind::IfElse;
            out.type = ret->type;
            out.generic_index = ret->generic_index;
            out.generic_has_error_constraint = ret->generic_has_error_constraint;
            out.generic_has_eq_constraint = ret->generic_has_eq_constraint;
            out.generic_has_ord_constraint = ret->generic_has_ord_constraint;
            out.generic_protocol_index = ret->generic_protocol_index;
            out.generic_protocol_count = ret->generic_protocol_count;
            for (u32 cpi = 0; cpi < out.generic_protocol_count; cpi++)
                out.generic_protocol_indices[cpi] = ret->generic_protocol_indices[cpi];
            out.variant_index = ret->variant_index;
            out.struct_index = ret->struct_index;
            out.tuple_len = ret->tuple_len;
            for (u32 i = 0; i < ret->tuple_len; i++) {
                out.tuple_types[i] = ret->tuple_types[i];
                out.tuple_variant_indices[i] = ret->tuple_variant_indices[i];
                out.tuple_struct_indices[i] = ret->tuple_struct_indices[i];
            }
            out.span = expr.span;
            out.shape_index = ret->shape_index;
            out.may_nil = pipe_lhs->may_nil || then_expr->may_nil;
            out.may_error = pipe_lhs->may_error || then_expr->may_error;
            out.error_struct_index =
                then_expr->may_error ? then_expr->error_struct_index : pipe_lhs->error_struct_index;
            out.error_variant_index = then_expr->may_error ? then_expr->error_variant_index
                                                           : pipe_lhs->error_variant_index;
            out.lhs = cond_ptr;
            out.rhs = then_ptr;
            if (!out.args.push(else_ptr))
                return frontend_error(FrontendError::TooManyItems, expr.span);
            return out;
        }
    }
    HirExpr analyzed_args[AstExpr::kMaxArgs]{};
    u32 placeholder_count = 0;
    for (u32 i = 0; i < expr.args.len; i++) {
        const auto& arg_expr = *expr.args[i];
        if (arg_expr.kind == AstExprKind::Placeholder) {
            if (pipe_lhs == nullptr)
                return frontend_error(FrontendError::UnsupportedSyntax, arg_expr.span);
            placeholder_count++;
            auto slot_expr = placeholder_slot_expr(*pipe_lhs, arg_expr.int_value, arg_expr.span);
            if (!slot_expr) return core::make_unexpected(slot_expr.error());
            analyzed_args[i] = slot_expr.value();
        } else {
            auto arg = analyze_expr(arg_expr, route, mod, locals, local_count, binding);
            if (!arg) return core::make_unexpected(arg.error());
            if (arg->may_nil || arg->may_error)
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            analyzed_args[i] = arg.value();
        }
        if (fn.params[i].type == HirTypeKind::Generic) {
            if (!bind_generic_shape(generic_bindings,
                                    fn.type_params.len,
                                    fn.params[i].generic_index,
                                    analyzed_args[i]))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            if (fn.type_params[fn.params[i].generic_index].has_error_constraint &&
                !generic_binding_satisfies_error_constraint(
                    mod, generic_bindings[fn.params[i].generic_index]))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            if (fn.type_params[fn.params[i].generic_index].has_eq_constraint &&
                !generic_binding_satisfies_eq_constraint(
                    mod, generic_bindings[fn.params[i].generic_index]))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            if (fn.type_params[fn.params[i].generic_index].has_ord_constraint &&
                !generic_binding_satisfies_ord_constraint(
                    mod, generic_bindings[fn.params[i].generic_index]))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            for (u32 cpi = 0;
                 cpi < fn.type_params[fn.params[i].generic_index].custom_protocol_count;
                 cpi++) {
                const u32 proto_index =
                    fn.type_params[fn.params[i].generic_index].custom_protocol_indices[cpi];
                if (!generic_binding_satisfies_custom_protocol(
                        mod, generic_bindings[fn.params[i].generic_index], proto_index))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          expr.args[i]->span,
                                          mod.protocols[proto_index].name);
            }
        } else if (fn.params[i].template_variant_index != 0xffffffffu ||
                   fn.params[i].template_struct_index != 0xffffffffu) {
            const auto expected_shape = make_expected_param_expr(fn.params[i]);
            if (!bind_named_generic_shape(
                    mod, generic_bindings, fn.type_params.len, expected_shape, analyzed_args[i]))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
            auto expected = concrete_param_expr(fn.params[i]);
            if (!expected) return core::make_unexpected(expected.error());
            if (!same_hir_type_shape(mod, analyzed_args[i], expected.value()))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
        } else {
            auto expected = concrete_param_expr(fn.params[i]);
            if (!expected) return core::make_unexpected(expected.error());
            if (!same_hir_type_shape(mod, analyzed_args[i], expected.value()))
                return frontend_error(FrontendError::UnsupportedSyntax, expr.args[i]->span);
        }
    }
    if (pipe_lhs != nullptr && placeholder_count == 0)
        return frontend_error(FrontendError::UnsupportedSyntax, expr.span);
    auto inlined = instantiate_function_expr(
        fn.body, route, mod, analyzed_args, expr.args.len, generic_bindings, fn.type_params.len);
    if (!inlined) return core::make_unexpected(inlined.error());
    inlined->span = expr.span;
    return inlined.value();
}

static bool push_named_error_case(FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases,
                                  Str name) {
    for (u32 i = 0; i < cases.len; i++) {
        if (cases[i].name.eq(name)) return true;
    }
    RouteNamedErrorCase entry{};
    entry.name = name;
    return cases.push(entry);
}

static FrontendResult<void> collect_named_error_cases_ast_expr(
    const AstExpr& expr, FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases) {
    if (expr.kind == AstExprKind::Error && expr.lhs != nullptr &&
        expr.lhs->kind == AstExprKind::VariantCase && expr.lhs->name.len == 0 &&
        expr.lhs->str_value.len != 0) {
        if (!push_named_error_case(cases, expr.lhs->str_value))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    if (expr.lhs != nullptr) {
        auto lhs = collect_named_error_cases_ast_expr(*expr.lhs, cases);
        if (!lhs) return lhs;
    }
    if (expr.rhs != nullptr) {
        auto rhs = collect_named_error_cases_ast_expr(*expr.rhs, cases);
        if (!rhs) return rhs;
    }
    for (u32 i = 0; i < expr.field_inits.len; i++) {
        auto value = collect_named_error_cases_ast_expr(*expr.field_inits[i].value, cases);
        if (!value) return value;
    }
    for (u32 i = 0; i < expr.args.len; i++) {
        auto arg = collect_named_error_cases_ast_expr(*expr.args[i], cases);
        if (!arg) return arg;
    }
    return {};
}

static FrontendResult<void> collect_named_error_cases_ast(
    const AstStatement& stmt, FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases) {
    auto expr_cases = collect_named_error_cases_ast_expr(stmt.expr, cases);
    if (!expr_cases) return expr_cases;
    if (stmt.then_stmt != nullptr) {
        auto then_cases = collect_named_error_cases_ast(*stmt.then_stmt, cases);
        if (!then_cases) return then_cases;
    }
    if (stmt.else_stmt != nullptr) {
        auto else_cases = collect_named_error_cases_ast(*stmt.else_stmt, cases);
        if (!else_cases) return else_cases;
    }
    for (u32 i = 0; i < stmt.block_stmts.len; i++) {
        auto block_cases = collect_named_error_cases_ast(*stmt.block_stmts[i], cases);
        if (!block_cases) return block_cases;
    }
    for (u32 i = 0; i < stmt.match_arms.len; i++) {
        auto pattern_cases = collect_named_error_cases_ast_expr(stmt.match_arms[i].pattern, cases);
        if (!pattern_cases) return pattern_cases;
        if (stmt.match_arms[i].stmt != nullptr) {
            auto body_cases = collect_named_error_cases_ast(*stmt.match_arms[i].stmt, cases);
            if (!body_cases) return body_cases;
        }
    }
    return {};
}

static FrontendResult<void> collect_named_error_cases(
    const HirExpr& expr, FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases) {
    if (expr.kind == HirExprKind::Error && expr.error_variant_index == 0xffffffffu &&
        expr.str_value.len != 0) {
        if (!push_named_error_case(cases, expr.str_value))
            return frontend_error(FrontendError::TooManyItems, expr.span);
    }
    if (expr.lhs != nullptr) {
        auto lhs = collect_named_error_cases(*expr.lhs, cases);
        if (!lhs) return lhs;
    }
    if (expr.rhs != nullptr) {
        auto rhs = collect_named_error_cases(*expr.rhs, cases);
        if (!rhs) return rhs;
    }
    return {};
}

static void patch_named_error_variant(
    HirExpr* expr,
    u32 variant_index,
    const FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases>& cases) {
    if (expr->kind == HirExprKind::VariantCase && expr->variant_index == 0xffffffffu &&
        expr->str_value.len != 0) {
        expr->variant_index = variant_index;
        for (u32 i = 0; i < cases.len; i++) {
            if (cases[i].name.eq(expr->str_value)) {
                expr->case_index = i;
                expr->int_value = static_cast<i32>(i);
                break;
            }
        }
    }
    if (expr->kind == HirExprKind::Error && expr->error_variant_index == 0xffffffffu &&
        expr->str_value.len != 0) {
        expr->error_variant_index = variant_index;
        for (u32 i = 0; i < cases.len; i++) {
            if (cases[i].name.eq(expr->str_value)) {
                expr->error_case_index = i;
                expr->int_value = static_cast<i32>(i);
                break;
            }
        }
    }
    if (expr->lhs != nullptr) patch_named_error_variant(expr->lhs, variant_index, cases);
    if (expr->rhs != nullptr) patch_named_error_variant(expr->rhs, variant_index, cases);
}

static void patch_error_variant_refs(HirExpr* expr, u32 variant_index) {
    if (expr->may_error && expr->error_variant_index == 0xffffffffu &&
        expr->type != HirTypeKind::Variant)
        expr->error_variant_index = variant_index;
    if (expr->lhs != nullptr) patch_error_variant_refs(expr->lhs, variant_index);
    if (expr->rhs != nullptr) patch_error_variant_refs(expr->rhs, variant_index);
}

static FrontendResult<HirTerminator> analyze_term(const AstStatement& stmt, const HirModule& mod) {
    HirTerminator term{};
    term.span = stmt.span;
    if (stmt.kind == AstStmtKind::ReturnStatus) {
        if (stmt.status_code < 100 || stmt.status_code > 999)
            return frontend_error(FrontendError::InvalidStatusCode, stmt.span);
        term.kind = HirTerminatorKind::ReturnStatus;
        term.status_code = stmt.status_code;
        return term;
    }

    u32 upstream_index = mod.upstreams.len;
    for (u32 j = 0; j < mod.upstreams.len; j++) {
        if (mod.upstreams[j].name.eq(stmt.name)) {
            upstream_index = j;
            break;
        }
    }
    if (upstream_index == mod.upstreams.len)
        return frontend_error(FrontendError::UnknownUpstream, stmt.span, stmt.name);
    term.kind = HirTerminatorKind::ForwardUpstream;
    term.upstream_index = upstream_index;
    return term;
}

static FrontendResult<void> analyze_guard_match_arms(
    const FixedVec<AstStatement::MatchArm, AstStatement::kMaxMatchArms>& ast_arms,
    const HirExpr& subject,
    HirRoute* route,
    const HirModule& mod,
    const HirLocal* locals,
    u32 local_count,
    FixedVec<HirGuardMatchArm, HirModule::kMaxGuardMatchArms>* guard_match_arms,
    HirGuard* guard) {
    bool seen_wildcard = false;
    guard->fail_match_start = guard_match_arms->len;
    guard->fail_match_count = 0;
    for (u32 ai = 0; ai < ast_arms.len; ai++) {
        const auto& arm = ast_arms[ai];
        HirGuardMatchArm hir_arm{};
        hir_arm.span = arm.span;
        hir_arm.is_wildcard = arm.is_wildcard;
        if (seen_wildcard) return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
        if (!arm.is_wildcard) {
            HirExpr pattern{};
            if (subject.may_error && subject.error_variant_index == 0xffffffffu &&
                arm.pattern.kind == AstExprKind::VariantCase && arm.pattern.name.len == 0) {
                pattern.kind = HirExprKind::VariantCase;
                pattern.type = HirTypeKind::Variant;
                pattern.span = arm.pattern.span;
                pattern.variant_index = 0xffffffffu;
                pattern.case_index = 0xffffffffu;
                pattern.int_value = -1;
                pattern.str_value = arm.pattern.str_value;
            } else {
                auto analyzed_pattern =
                    analyze_match_pattern(arm.pattern, subject, route, mod, locals, local_count);
                if (!analyzed_pattern) return core::make_unexpected(analyzed_pattern.error());
                pattern = analyzed_pattern.value();
                if (pattern.kind != HirExprKind::VariantCase)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                if (!(subject.may_error && subject.error_variant_index != 0xffffffffu) ||
                    pattern.variant_index != subject.error_variant_index)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            }
            const u32 case_index = static_cast<u32>(pattern.int_value);
            if (pattern.variant_index != 0xffffffffu && case_index >= HirVariant::kMaxCases)
                return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            bool duplicate = false;
            for (u32 i = 0; i < guard->fail_match_count; i++) {
                const auto& seen = (*guard_match_arms)[guard->fail_match_start + i];
                if (!seen.is_wildcard &&
                    ((pattern.variant_index == 0xffffffffu &&
                      seen.pattern.str_value.eq(pattern.str_value)) ||
                     (pattern.variant_index != 0xffffffffu &&
                      static_cast<u32>(seen.pattern.int_value) == case_index))) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            hir_arm.pattern = pattern;
        } else {
            seen_wildcard = true;
        }
        auto term = analyze_term(*arm.stmt, mod);
        if (!term) return core::make_unexpected(term.error());
        hir_arm.direct_term = term.value();
        if (!guard_match_arms->push(hir_arm))
            return frontend_error(FrontendError::TooManyItems, arm.span);
        guard->fail_match_count++;
    }
    if (!seen_wildcard) return frontend_error(FrontendError::UnsupportedSyntax, subject.span);
    return {};
}

static KnownValueState known_value_state(const HirExpr& expr,
                                         const HirLocal* locals,
                                         u32 local_count,
                                         u32 depth) {
    if (depth > local_count) return KnownValueState::Unknown;
    if (expr.kind == HirExprKind::Nil) return KnownValueState::Nil;
    if (expr.kind == HirExprKind::Error) return KnownValueState::Error;
    if (!expr.may_nil && !expr.may_error) return KnownValueState::Available;
    if (expr.kind == HirExprKind::LocalRef && expr.local_index < local_count) {
        return known_value_state(locals[expr.local_index].init, locals, local_count, depth + 1);
    }
    return KnownValueState::Unknown;
}

static KnownErrorCase known_error_case(const HirExpr& expr,
                                       const HirLocal* locals,
                                       u32 local_count,
                                       u32 depth) {
    if (depth > local_count) return {};
    if (expr.kind == HirExprKind::Error && expr.error_variant_index != 0xffffffffu &&
        expr.error_case_index != 0xffffffffu) {
        KnownErrorCase out{};
        out.known = true;
        out.variant_index = expr.error_variant_index;
        out.case_index = expr.error_case_index;
        return out;
    }
    if (expr.kind == HirExprKind::LocalRef && expr.local_index < local_count) {
        return known_error_case(locals[expr.local_index].init, locals, local_count, depth + 1);
    }
    return {};
}

static Str known_error_name(const HirExpr& expr,
                            const HirLocal* locals,
                            u32 local_count,
                            u32 depth) {
    if (depth > local_count) return {};
    if (expr.kind == HirExprKind::Error && expr.str_value.len != 0) return expr.str_value;
    if (expr.kind == HirExprKind::LocalRef && expr.local_index < local_count) {
        return known_error_name(locals[expr.local_index].init, locals, local_count, depth + 1);
    }
    return {};
}

static const HirExpr* known_error_expr(const HirExpr& expr,
                                       const HirLocal* locals,
                                       u32 local_count,
                                       u32 depth) {
    if (depth > local_count) return nullptr;
    if (expr.kind == HirExprKind::Error) return &expr;
    if (expr.kind == HirExprKind::LocalRef && expr.local_index < local_count) {
        return known_error_expr(locals[expr.local_index].init, locals, local_count, depth + 1);
    }
    return nullptr;
}

static bool const_eval_expr(
    const HirExpr& expr, const HirLocal* locals, u32 local_count, ConstValue* out, u32 depth) {
    if (depth > local_count) return false;
    if (expr.kind == HirExprKind::BoolLit) {
        out->type = HirTypeKind::Bool;
        out->bool_value = expr.bool_value;
        return true;
    }
    if (expr.kind == HirExprKind::IntLit) {
        out->type = HirTypeKind::I32;
        out->int_value = expr.int_value;
        return true;
    }
    if (expr.kind == HirExprKind::VariantCase) {
        out->type = HirTypeKind::Variant;
        out->variant_index = expr.variant_index;
        out->case_index = expr.case_index;
        return true;
    }
    if (expr.kind == HirExprKind::LocalRef && expr.local_index < local_count) {
        return const_eval_expr(locals[expr.local_index].init, locals, local_count, out, depth + 1);
    }
    if ((expr.kind == HirExprKind::Eq || expr.kind == HirExprKind::Lt ||
         expr.kind == HirExprKind::Gt) &&
        expr.lhs != nullptr && expr.rhs != nullptr) {
        ConstValue lhs{};
        ConstValue rhs{};
        if (!const_eval_expr(*expr.lhs, locals, local_count, &lhs, depth + 1)) return false;
        if (!const_eval_expr(*expr.rhs, locals, local_count, &rhs, depth + 1)) return false;
        if (lhs.type != rhs.type) return false;
        out->type = HirTypeKind::Bool;
        if (expr.kind == HirExprKind::Eq) {
            if (lhs.type == HirTypeKind::Bool) {
                out->bool_value = lhs.bool_value == rhs.bool_value;
                return true;
            }
            if (lhs.type == HirTypeKind::I32) {
                out->bool_value = lhs.int_value == rhs.int_value;
                return true;
            }
            if (lhs.type == HirTypeKind::Variant) {
                out->bool_value =
                    lhs.variant_index == rhs.variant_index && lhs.case_index == rhs.case_index;
                return true;
            }
            return false;
        }
        if (lhs.type == HirTypeKind::I32) {
            out->bool_value = expr.kind == HirExprKind::Lt ? (lhs.int_value < rhs.int_value)
                                                           : (lhs.int_value > rhs.int_value);
            return true;
        }
    }
    return false;
}

static FrontendResult<HirExpr> analyze_guard_cond(const AstExpr& expr,
                                                  HirRoute* route,
                                                  const HirModule& mod,
                                                  const HirLocal* locals,
                                                  u32 local_count) {
    auto analyzed = analyze_expr(expr, route, mod, locals, local_count, nullptr);
    if (!analyzed) return core::make_unexpected(analyzed.error());

    HirExpr cond{};
    cond.span = expr.span;
    cond.kind = HirExprKind::BoolLit;
    cond.type = HirTypeKind::Bool;

    if (!analyzed->may_error) {
        cond.bool_value = true;
        return cond;
    }

    const auto state = known_value_state(analyzed.value(), locals, local_count, 0);
    if (state == KnownValueState::Error) {
        cond.bool_value = false;
        return cond;
    }
    if (state == KnownValueState::Available || state == KnownValueState::Nil) {
        cond.bool_value = true;
        return cond;
    }

    if (!route->exprs.push(analyzed.value()))
        return frontend_error(FrontendError::TooManyItems, expr.span);
    cond.kind = HirExprKind::NoError;
    cond.lhs = &route->exprs[route->exprs.len - 1];
    return cond;
}

static FrontendResult<void> analyze_match_arm_body(const AstStatement& stmt,
                                                   HirMatchArm* arm,
                                                   HirRoute* route,
                                                   const HirModule& mod,
                                                   const HirLocal* locals,
                                                   u32 local_count,
                                                   const MatchPayloadBinding* binding) {
    if (stmt.kind == AstStmtKind::Block) {
        FixedVec<HirLocal, HirRoute::kMaxLocals> scoped_locals;
        for (u32 i = 0; i < local_count; i++) {
            if (!scoped_locals.push(locals[i]))
                return frontend_error(FrontendError::TooManyItems, stmt.span);
        }
        for (u32 si = 0; si < stmt.block_stmts.len; si++) {
            const auto& inner = *stmt.block_stmts[si];
            const bool is_last = si + 1 == stmt.block_stmts.len;
            if (inner.kind == AstStmtKind::Let) {
                if (is_last) return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
                HirLocal local{};
                local.span = inner.span;
                local.name = inner.name;
                local.ref_index =
                    next_local_ref_index(route, scoped_locals.data, scoped_locals.len);
                auto init = analyze_expr(
                    inner.expr, route, mod, scoped_locals.data, scoped_locals.len, binding);
                if (!init) return core::make_unexpected(init.error());
                auto typed = apply_declared_type_to_expr(&init.value(), mod, inner);
                if (!typed) return core::make_unexpected(typed.error());
                local.type = init->type;
                local.generic_index = init->generic_index;
                local.generic_has_error_constraint = init->generic_has_error_constraint;
                local.generic_has_eq_constraint = init->generic_has_eq_constraint;
                local.generic_has_ord_constraint = init->generic_has_ord_constraint;
                local.generic_protocol_index = init->generic_protocol_index;
                local.generic_protocol_count = init->generic_protocol_count;
                for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                    local.generic_protocol_indices[cpi] = init->generic_protocol_indices[cpi];
                local.may_nil = init->may_nil;
                local.may_error = init->may_error;
                local.variant_index = init->variant_index;
                local.struct_index = init->struct_index;
                local.tuple_len = init->tuple_len;
                for (u32 ti = 0; ti < init->tuple_len; ti++) {
                    local.tuple_types[ti] = init->tuple_types[ti];
                    local.tuple_variant_indices[ti] = init->tuple_variant_indices[ti];
                    local.tuple_struct_indices[ti] = init->tuple_struct_indices[ti];
                }
                local.error_struct_index = init->error_struct_index;
                local.error_variant_index = init->error_variant_index;
                local.shape_index = init->shape_index;
                local.init = init.value();
                if (!route->locals.push(local))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (local.ref_index >= HirRoute::kMaxLocals)
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (scoped_locals.len <= local.ref_index) scoped_locals.len = local.ref_index + 1;
                scoped_locals[local.ref_index] = local;
                continue;
            }
            if (inner.kind == AstStmtKind::Guard) {
                if (is_last) return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
                HirGuard guard{};
                guard.span = inner.span;
                auto bound = analyze_expr(
                    inner.expr, route, mod, scoped_locals.data, scoped_locals.len, binding);
                if (!bound) return core::make_unexpected(bound.error());
                auto cond = analyze_guard_cond(
                    inner.expr, route, mod, scoped_locals.data, scoped_locals.len);
                if (!cond) return core::make_unexpected(cond.error());
                guard.cond = cond.value();
                if (inner.match_arms.len != 0) {
                    if (!bound->may_error) {
                        u32 fallback_arm = 0;
                        for (u32 ai = 0; ai < inner.match_arms.len; ai++) {
                            if (inner.match_arms[ai].is_wildcard) {
                                fallback_arm = ai;
                                break;
                            }
                        }
                        auto fail_term = analyze_term(*inner.match_arms[fallback_arm].stmt, mod);
                        if (!fail_term) return core::make_unexpected(fail_term.error());
                        guard.fail_term = fail_term.value();
                    } else {
                        guard.fail_kind = HirGuard::FailKind::Match;
                        guard.fail_match_expr = bound.value();
                        auto* guard_match_arms =
                            const_cast<FixedVec<HirGuardMatchArm, HirModule::kMaxGuardMatchArms>*>(
                                &mod.guard_match_arms);
                        auto fail_match = analyze_guard_match_arms(inner.match_arms,
                                                                   bound.value(),
                                                                   route,
                                                                   mod,
                                                                   scoped_locals.data,
                                                                   scoped_locals.len,
                                                                   guard_match_arms,
                                                                   &guard);
                        if (!fail_match) return core::make_unexpected(fail_match.error());
                    }
                } else {
                    if (inner.else_stmt->kind == AstStmtKind::ReturnStatus ||
                        inner.else_stmt->kind == AstStmtKind::ForwardUpstream) {
                        auto fail_term = analyze_term(*inner.else_stmt, mod);
                        if (!fail_term) return core::make_unexpected(fail_term.error());
                        guard.fail_term = fail_term.value();
                    } else {
                        guard.fail_kind = HirGuard::FailKind::Body;
                        auto fail_body = analyze_guard_fail_body(*inner.else_stmt,
                                                                 &guard.fail_body,
                                                                 route,
                                                                 mod,
                                                                 scoped_locals.data,
                                                                 scoped_locals.len,
                                                                 binding);
                        if (!fail_body) return core::make_unexpected(fail_body.error());
                    }
                }
                if (!arm->guards.push(guard))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (inner.bind_value) {
                    if (known_value_state(
                            bound.value(), scoped_locals.data, scoped_locals.len, 0) ==
                        KnownValueState::Error)
                        return frontend_error(FrontendError::UnsupportedSyntax, inner.expr.span);
                    HirLocal local{};
                    local.span = inner.span;
                    local.name = inner.name;
                    local.ref_index =
                        next_local_ref_index(route, scoped_locals.data, scoped_locals.len);
                    local.type = bound->type;
                    local.generic_index = bound->generic_index;
                    local.generic_has_error_constraint = bound->generic_has_error_constraint;
                    local.generic_has_eq_constraint = bound->generic_has_eq_constraint;
                    local.generic_has_ord_constraint = bound->generic_has_ord_constraint;
                    local.generic_protocol_index = bound->generic_protocol_index;
                    local.generic_protocol_count = bound->generic_protocol_count;
                    for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                        local.generic_protocol_indices[cpi] = bound->generic_protocol_indices[cpi];
                    local.may_nil = bound->may_nil;
                    local.may_error = false;
                    local.variant_index = bound->variant_index;
                    local.struct_index = bound->struct_index;
                    local.tuple_len = bound->tuple_len;
                    for (u32 ti = 0; ti < bound->tuple_len; ti++) {
                        local.tuple_types[ti] = bound->tuple_types[ti];
                        local.tuple_variant_indices[ti] = bound->tuple_variant_indices[ti];
                        local.tuple_struct_indices[ti] = bound->tuple_struct_indices[ti];
                    }
                    local.error_struct_index = bound->error_struct_index;
                    local.error_variant_index = 0xffffffffu;
                    local.shape_index = bound->shape_index;
                    auto init = make_guard_bound_init(route, bound.value(), inner.span);
                    if (!init) return core::make_unexpected(init.error());
                    local.init = init.value();
                    if (!route->locals.push(local))
                        return frontend_error(FrontendError::TooManyItems, inner.span);
                    if (local.ref_index >= HirRoute::kMaxLocals)
                        return frontend_error(FrontendError::TooManyItems, inner.span);
                    if (scoped_locals.len <= local.ref_index)
                        scoped_locals.len = local.ref_index + 1;
                    scoped_locals[local.ref_index] = local;
                }
                continue;
            }
            if (!is_last) return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
            return analyze_match_arm_body(
                inner, arm, route, mod, scoped_locals.data, scoped_locals.len, binding);
        }
        return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
    }
    if (stmt.kind == AstStmtKind::If) {
        arm->body_kind = HirMatchArm::BodyKind::If;
        auto cond = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!cond) return core::make_unexpected(cond.error());
        if (cond->type != HirTypeKind::Bool)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        arm->cond = cond.value();
        auto then_term = analyze_term(*stmt.then_stmt, mod);
        if (!then_term) return core::make_unexpected(then_term.error());
        auto else_term = analyze_term(*stmt.else_stmt, mod);
        if (!else_term) return core::make_unexpected(else_term.error());
        arm->then_term = then_term.value();
        arm->else_term = else_term.value();
        return {};
    }

    arm->body_kind = HirMatchArm::BodyKind::Direct;
    auto term = analyze_term(stmt, mod);
    if (!term) return core::make_unexpected(term.error());
    arm->direct_term = term.value();
    return {};
}

static FrontendResult<void> analyze_guard_fail_body(const AstStatement& stmt,
                                                    HirGuardBody* body,
                                                    HirRoute* route,
                                                    const HirModule& mod,
                                                    const HirLocal* locals,
                                                    u32 local_count,
                                                    const MatchPayloadBinding* binding) {
    if (stmt.kind == AstStmtKind::Block) {
        FixedVec<HirLocal, HirRoute::kMaxLocals> scoped_locals;
        for (u32 i = 0; i < local_count; i++) {
            if (!scoped_locals.push(locals[i]))
                return frontend_error(FrontendError::TooManyItems, stmt.span);
        }
        for (u32 si = 0; si < stmt.block_stmts.len; si++) {
            const auto& inner = *stmt.block_stmts[si];
            const bool is_last = si + 1 == stmt.block_stmts.len;
            if (inner.kind == AstStmtKind::Let) {
                if (is_last) return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
                HirLocal local{};
                local.span = inner.span;
                local.name = inner.name;
                local.ref_index =
                    next_local_ref_index(route, scoped_locals.data, scoped_locals.len);
                auto init = analyze_expr(
                    inner.expr, route, mod, scoped_locals.data, scoped_locals.len, binding);
                if (!init) return core::make_unexpected(init.error());
                auto typed = apply_declared_type_to_expr(&init.value(), mod, inner);
                if (!typed) return core::make_unexpected(typed.error());
                local.type = init->type;
                local.generic_index = init->generic_index;
                local.generic_has_error_constraint = init->generic_has_error_constraint;
                local.generic_has_eq_constraint = init->generic_has_eq_constraint;
                local.generic_has_ord_constraint = init->generic_has_ord_constraint;
                local.generic_protocol_index = init->generic_protocol_index;
                local.generic_protocol_count = init->generic_protocol_count;
                for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                    local.generic_protocol_indices[cpi] = init->generic_protocol_indices[cpi];
                local.may_nil = init->may_nil;
                local.may_error = init->may_error;
                local.variant_index = init->variant_index;
                local.struct_index = init->struct_index;
                local.tuple_len = init->tuple_len;
                for (u32 ti = 0; ti < init->tuple_len; ti++) {
                    local.tuple_types[ti] = init->tuple_types[ti];
                    local.tuple_variant_indices[ti] = init->tuple_variant_indices[ti];
                    local.tuple_struct_indices[ti] = init->tuple_struct_indices[ti];
                }
                local.error_struct_index = init->error_struct_index;
                local.error_variant_index = init->error_variant_index;
                local.shape_index = init->shape_index;
                local.init = init.value();
                if (!route->locals.push(local))
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (local.ref_index >= HirRoute::kMaxLocals)
                    return frontend_error(FrontendError::TooManyItems, inner.span);
                if (scoped_locals.len <= local.ref_index) scoped_locals.len = local.ref_index + 1;
                scoped_locals[local.ref_index] = local;
                continue;
            }
            if (!is_last) return frontend_error(FrontendError::UnsupportedSyntax, inner.span);
            return analyze_guard_fail_body(
                inner, body, route, mod, scoped_locals.data, scoped_locals.len, binding);
        }
        return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
    }
    if (stmt.kind == AstStmtKind::If) {
        body->body_kind = HirGuardBody::BodyKind::If;
        auto cond = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!cond) return core::make_unexpected(cond.error());
        if (cond->type != HirTypeKind::Bool)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        body->cond = cond.value();
        auto then_term = analyze_term(*stmt.then_stmt, mod);
        if (!then_term) return core::make_unexpected(then_term.error());
        auto else_term = analyze_term(*stmt.else_stmt, mod);
        if (!else_term) return core::make_unexpected(else_term.error());
        body->then_term = then_term.value();
        body->else_term = else_term.value();
        return {};
    }
    body->body_kind = HirGuardBody::BodyKind::Direct;
    auto term = analyze_term(stmt, mod);
    if (!term) return core::make_unexpected(term.error());
    body->direct_term = term.value();
    return {};
}

static FrontendResult<void> analyze_control_stmt(const AstStatement& stmt,
                                                 HirRoute* route,
                                                 const HirModule& mod,
                                                 const MatchPayloadBinding* binding) {
    const HirLocal* locals = route->locals.data;
    const u32 local_count = route->locals.len;

    if (stmt.kind == AstStmtKind::If && stmt.is_const) {
        auto cond = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!cond) return core::make_unexpected(cond.error());
        ConstValue value{};
        if (!const_eval_expr(cond.value(), locals, local_count, &value, 0) ||
            value.type != HirTypeKind::Bool)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        return analyze_control_stmt(
            value.bool_value ? *stmt.then_stmt : *stmt.else_stmt, route, mod, binding);
    }

    if (stmt.kind == AstStmtKind::Match && stmt.is_const) {
        auto subject = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!subject) return core::make_unexpected(subject.error());
        ConstValue subject_value{};
        if (!const_eval_expr(subject.value(), locals, local_count, &subject_value, 0))
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        if (!route->exprs.push(subject.value()))
            return frontend_error(FrontendError::TooManyItems, stmt.span);
        const HirExpr* subject_ptr = &route->exprs[route->exprs.len - 1];

        const AstStatement::MatchArm* wildcard_arm = nullptr;
        const AstStatement::MatchArm* selected_arm = nullptr;
        MatchPayloadBinding selected_binding{};
        const MatchPayloadBinding* selected_binding_ptr = binding;

        for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
            const auto& arm = stmt.match_arms[ai];
            if (arm.is_wildcard) {
                wildcard_arm = &arm;
                continue;
            }
            auto pattern = analyze_match_pattern(
                arm.pattern, subject.value(), route, mod, locals, local_count);
            if (!pattern) return core::make_unexpected(pattern.error());

            bool matched = false;
            if (pattern->kind == HirExprKind::BoolLit && subject_value.type == HirTypeKind::Bool) {
                matched = pattern->bool_value == subject_value.bool_value;
            } else if (pattern->kind == HirExprKind::IntLit &&
                       subject_value.type == HirTypeKind::I32) {
                matched = pattern->int_value == subject_value.int_value;
            } else if (pattern->kind == HirExprKind::VariantCase &&
                       subject_value.type == HirTypeKind::Variant) {
                matched = pattern->variant_index == subject_value.variant_index &&
                          pattern->case_index == subject_value.case_index;
            }

            if (!matched) continue;
            selected_arm = &arm;
            if (pattern->kind == HirExprKind::VariantCase &&
                mod.variants[pattern->variant_index].cases[pattern->case_index].has_payload &&
                arm.pattern.lhs != nullptr) {
                selected_binding = {};
                selected_binding.name = arm.pattern.lhs->name;
                selected_binding.type =
                    mod.variants[pattern->variant_index].cases[pattern->case_index].payload_type;
                selected_binding.generic_index = mod.variants[pattern->variant_index]
                                                     .cases[pattern->case_index]
                                                     .payload_generic_index;
                selected_binding.generic_has_error_constraint =
                    mod.variants[pattern->variant_index]
                        .cases[pattern->case_index]
                        .payload_generic_has_error_constraint;
                selected_binding.generic_has_eq_constraint = mod.variants[pattern->variant_index]
                                                                 .cases[pattern->case_index]
                                                                 .payload_generic_has_eq_constraint;
                selected_binding.generic_has_ord_constraint =
                    mod.variants[pattern->variant_index]
                        .cases[pattern->case_index]
                        .payload_generic_has_ord_constraint;
                selected_binding.generic_protocol_index = mod.variants[pattern->variant_index]
                                                              .cases[pattern->case_index]
                                                              .payload_generic_protocol_index;
                selected_binding.generic_protocol_count = mod.variants[pattern->variant_index]
                                                              .cases[pattern->case_index]
                                                              .payload_generic_protocol_count;
                for (u32 cpi = 0; cpi < selected_binding.generic_protocol_count; cpi++) {
                    selected_binding.generic_protocol_indices[cpi] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_generic_protocol_indices[cpi];
                }
                selected_binding.variant_index = mod.variants[pattern->variant_index]
                                                     .cases[pattern->case_index]
                                                     .payload_variant_index;
                selected_binding.struct_index = mod.variants[pattern->variant_index]
                                                    .cases[pattern->case_index]
                                                    .payload_struct_index;
                selected_binding.shape_index = mod.variants[pattern->variant_index]
                                                   .cases[pattern->case_index]
                                                   .payload_shape_index;
                selected_binding.tuple_len = mod.variants[pattern->variant_index]
                                                 .cases[pattern->case_index]
                                                 .payload_tuple_len;
                for (u32 ti = 0; ti < selected_binding.tuple_len; ti++) {
                    selected_binding.tuple_types[ti] = mod.variants[pattern->variant_index]
                                                           .cases[pattern->case_index]
                                                           .payload_tuple_types[ti];
                    selected_binding.tuple_variant_indices[ti] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_tuple_variant_indices[ti];
                    selected_binding.tuple_struct_indices[ti] =
                        mod.variants[pattern->variant_index]
                            .cases[pattern->case_index]
                            .payload_tuple_struct_indices[ti];
                }
                selected_binding.case_index = pattern->case_index;
                selected_binding.subject = subject_ptr;
                selected_binding_ptr = &selected_binding;
            }
            break;
        }

        if (selected_arm == nullptr) selected_arm = wildcard_arm;
        if (selected_arm == nullptr)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        route->control.match_expr = subject.value();
        return analyze_control_stmt(*selected_arm->stmt, route, mod, selected_binding_ptr);
    }

    if (stmt.kind == AstStmtKind::If) {
        auto cond = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!cond) return core::make_unexpected(cond.error());
        if (cond->type != HirTypeKind::Bool)
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
        const auto simple_branch = [](const AstStatement& branch) {
            return branch.kind == AstStmtKind::ReturnStatus ||
                   branch.kind == AstStmtKind::ForwardUpstream;
        };
        if (simple_branch(*stmt.then_stmt) && simple_branch(*stmt.else_stmt)) {
            route->control.kind = HirControlKind::If;
            route->control.cond = cond.value();
            auto then_term = analyze_term(*stmt.then_stmt, mod);
            if (!then_term) return core::make_unexpected(then_term.error());
            auto else_term = analyze_term(*stmt.else_stmt, mod);
            if (!else_term) return core::make_unexpected(else_term.error());
            route->control.then_term = then_term.value();
            route->control.else_term = else_term.value();
            return {};
        }

        const auto supported_branch = [](const AstStatement& branch) {
            return branch.kind == AstStmtKind::ReturnStatus ||
                   branch.kind == AstStmtKind::ForwardUpstream || branch.kind == AstStmtKind::If ||
                   branch.kind == AstStmtKind::Block;
        };
        if (!supported_branch(*stmt.then_stmt) || !supported_branch(*stmt.else_stmt))
            return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);

        route->control.kind = HirControlKind::Match;
        route->control.match_expr = cond.value();
        route->control.match_arms.len = 0;

        auto make_bool_pattern = [&](bool value, Span span) {
            HirExpr pattern{};
            pattern.kind = HirExprKind::BoolLit;
            pattern.type = HirTypeKind::Bool;
            pattern.bool_value = value;
            pattern.span = span;
            return pattern;
        };

        HirMatchArm then_arm{};
        then_arm.span = stmt.then_stmt->span;
        then_arm.pattern = make_bool_pattern(true, stmt.then_stmt->span);
        auto then_body = analyze_match_arm_body(
            *stmt.then_stmt, &then_arm, route, mod, locals, local_count, binding);
        if (!then_body) return core::make_unexpected(then_body.error());
        if (!route->control.match_arms.push(then_arm))
            return frontend_error(FrontendError::TooManyItems, stmt.then_stmt->span);

        HirMatchArm else_arm{};
        else_arm.span = stmt.else_stmt->span;
        else_arm.pattern = make_bool_pattern(false, stmt.else_stmt->span);
        auto else_body = analyze_match_arm_body(
            *stmt.else_stmt, &else_arm, route, mod, locals, local_count, binding);
        if (!else_body) return core::make_unexpected(else_body.error());
        if (!route->control.match_arms.push(else_arm))
            return frontend_error(FrontendError::TooManyItems, stmt.else_stmt->span);
        return {};
    }

    if (stmt.kind == AstStmtKind::Match) {
        auto subject = analyze_expr(stmt.expr, route, mod, locals, local_count, binding);
        if (!subject) return core::make_unexpected(subject.error());
        const auto state = known_value_state(subject.value(), locals, local_count, 0);
        const auto err_name = known_error_name(subject.value(), locals, local_count, 0);
        const bool subject_is_error_kind = subject->type != HirTypeKind::Variant &&
                                           subject->may_error &&
                                           (subject->error_variant_index != 0xffffffffu ||
                                            (state == KnownValueState::Error && err_name.len != 0));
        if (subject->type != HirTypeKind::Variant && subject->may_error &&
            (subject->error_variant_index != 0xffffffffu ||
             (state == KnownValueState::Error && err_name.len != 0))) {
            if (state != KnownValueState::Unknown) {
                const AstStatement::MatchArm* wildcard_arm = nullptr;
                const AstStatement::MatchArm* selected_arm = nullptr;
                if (state == KnownValueState::Error) {
                    const auto err_case = known_error_case(subject.value(), locals, local_count, 0);
                    if (!err_case.known)
                        if (err_name.len == 0)
                            return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
                    for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
                        const auto& arm = stmt.match_arms[ai];
                        if (arm.is_wildcard) {
                            wildcard_arm = &arm;
                            continue;
                        }
                        if (arm.pattern.kind != AstExprKind::VariantCase)
                            return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                        bool matched = false;
                        if (err_case.known && arm.pattern.name.len != 0) {
                            auto pattern = analyze_match_pattern(
                                arm.pattern, subject.value(), route, mod, locals, local_count);
                            if (!pattern) return core::make_unexpected(pattern.error());
                            matched = pattern->kind == HirExprKind::VariantCase &&
                                      pattern->variant_index == err_case.variant_index &&
                                      pattern->case_index == err_case.case_index;
                        } else {
                            matched = arm.pattern.str_value.eq(err_name);
                        }
                        if (matched) {
                            selected_arm = &arm;
                            break;
                        }
                    }
                } else {
                    for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
                        if (stmt.match_arms[ai].is_wildcard) {
                            wildcard_arm = &stmt.match_arms[ai];
                            break;
                        }
                    }
                }

                if (selected_arm == nullptr) selected_arm = wildcard_arm;
                if (selected_arm == nullptr)
                    return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
                return analyze_control_stmt(*selected_arm->stmt, route, mod, binding);
            }
        }

        route->control.kind = HirControlKind::Match;
        route->control.match_expr = subject.value();
        bool seen_wildcard = false;
        bool seen_variant_cases[HirVariant::kMaxCases]{};
        u32 seen_variant_case_count = 0;
        route->control.match_arms.len = 0;
        for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
            const auto& arm = stmt.match_arms[ai];
            HirMatchArm hir_arm{};
            hir_arm.span = arm.span;
            hir_arm.is_wildcard = arm.is_wildcard;
            MatchPayloadBinding arm_binding{};
            const MatchPayloadBinding* arm_binding_ptr = binding;
            if (seen_wildcard) return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
            if (!arm.is_wildcard) {
                auto analyzed_pattern = analyze_match_pattern(
                    arm.pattern, subject.value(), route, mod, locals, local_count);
                if (!analyzed_pattern) return core::make_unexpected(analyzed_pattern.error());
                HirExpr pattern = analyzed_pattern.value();
                if (pattern.kind != HirExprKind::BoolLit && pattern.kind != HirExprKind::IntLit &&
                    pattern.kind != HirExprKind::VariantCase)
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                if (pattern.type != subject->type &&
                    !(subject_is_error_kind && pattern.kind == HirExprKind::VariantCase))
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                if (pattern.type == HirTypeKind::Variant &&
                    ((subject_is_error_kind &&
                      pattern.variant_index != subject->error_variant_index) ||
                     (!subject_is_error_kind && pattern.variant_index != subject->variant_index)))
                    return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                if (pattern.kind == HirExprKind::VariantCase) {
                    const u32 case_index = static_cast<u32>(pattern.int_value);
                    if (case_index >= HirVariant::kMaxCases)
                        return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                    if (seen_variant_cases[case_index])
                        return frontend_error(FrontendError::UnsupportedSyntax, arm.span);
                    seen_variant_cases[case_index] = true;
                    seen_variant_case_count++;
                    const auto& variant = mod.variants[pattern.variant_index];
                    const auto& case_decl = variant.cases[case_index];
                    if (case_decl.has_payload && arm.pattern.lhs != nullptr) {
                        hir_arm.bind_payload = true;
                        hir_arm.bind_name = arm.pattern.lhs->name;
                        hir_arm.bind_type = case_decl.payload_type;
                        hir_arm.bind_variant_index = case_decl.payload_variant_index;
                        hir_arm.bind_struct_index = case_decl.payload_struct_index;
                        hir_arm.bind_tuple_len = case_decl.payload_tuple_len;
                        for (u32 ti = 0; ti < case_decl.payload_tuple_len; ti++) {
                            hir_arm.bind_tuple_types[ti] = case_decl.payload_tuple_types[ti];
                            hir_arm.bind_tuple_variant_indices[ti] =
                                case_decl.payload_tuple_variant_indices[ti];
                            hir_arm.bind_tuple_struct_indices[ti] =
                                case_decl.payload_tuple_struct_indices[ti];
                        }
                        arm_binding.name = hir_arm.bind_name;
                        arm_binding.type = hir_arm.bind_type;
                        arm_binding.generic_index = case_decl.payload_generic_index;
                        arm_binding.generic_has_error_constraint =
                            case_decl.payload_generic_has_error_constraint;
                        arm_binding.generic_has_eq_constraint =
                            case_decl.payload_generic_has_eq_constraint;
                        arm_binding.generic_has_ord_constraint =
                            case_decl.payload_generic_has_ord_constraint;
                        arm_binding.generic_protocol_index =
                            case_decl.payload_generic_protocol_index;
                        arm_binding.generic_protocol_count =
                            case_decl.payload_generic_protocol_count;
                        for (u32 cpi = 0; cpi < arm_binding.generic_protocol_count; cpi++)
                            arm_binding.generic_protocol_indices[cpi] =
                                case_decl.payload_generic_protocol_indices[cpi];
                        arm_binding.variant_index = hir_arm.bind_variant_index;
                        arm_binding.struct_index = hir_arm.bind_struct_index;
                        arm_binding.shape_index = case_decl.payload_shape_index;
                        arm_binding.case_index = case_index;
                        arm_binding.tuple_len = case_decl.payload_tuple_len;
                        for (u32 ti = 0; ti < case_decl.payload_tuple_len; ti++) {
                            arm_binding.tuple_types[ti] = case_decl.payload_tuple_types[ti];
                            arm_binding.tuple_variant_indices[ti] =
                                case_decl.payload_tuple_variant_indices[ti];
                            arm_binding.tuple_struct_indices[ti] =
                                case_decl.payload_tuple_struct_indices[ti];
                        }
                        arm_binding.subject = &route->control.match_expr;
                        arm_binding_ptr = &arm_binding;
                    }
                }
                hir_arm.pattern = pattern;
            } else {
                seen_wildcard = true;
            }
            auto body = analyze_match_arm_body(
                *arm.stmt, &hir_arm, route, mod, locals, local_count, arm_binding_ptr);
            if (!body) return core::make_unexpected(body.error());
            if (!route->control.match_arms.push(hir_arm))
                return frontend_error(FrontendError::TooManyItems, arm.span);
        }
        if (!seen_wildcard) {
            if (subject->type != HirTypeKind::Variant && !subject_is_error_kind)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
            const auto& variant = mod.variants[subject_is_error_kind ? subject->error_variant_index
                                                                     : subject->variant_index];
            if (seen_variant_case_count != variant.cases.len)
                return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
        }
        return {};
    }

    route->control.kind = HirControlKind::Direct;
    auto term = analyze_term(stmt, mod);
    if (!term) return core::make_unexpected(term.error());
    route->control.direct_term = term.value();
    return {};
}

}  // namespace

static FrontendResult<HirModule*> analyze_file_internal(
    const AstFile& file,
    Str source_path,
    std::vector<std::string>& import_stack,
    std::deque<std::string>* shared_owned_strings);

static Str import_visible_name(const ImportedModuleInfo& imported, Str original_name) {
    if (imported.has_namespace_alias) {
        return intern_generated_name(str_to_std_string(imported.namespace_alias) + "__" +
                                     str_to_std_string(original_name));
    }
    if (!imported.selective) return original_name;
    for (u32 i = 0; i < imported.selected_names.len; i++) {
        const auto& selected = imported.selected_names[i];
        if (!selected.name.eq(original_name)) continue;
        if (selected.has_alias) return selected.alias;
        return selected.name;
    }
    return original_name;
}

static bool is_hidden_import_runtime_function(Str name) {
    return (name.len >= 7 && __builtin_memcmp(name.ptr, "__impl_", 7) == 0) ||
           (name.len >= 8 && __builtin_memcmp(name.ptr, "__proto_", 8) == 0);
}

static bool imported_module_exports_function_name(const HirModule& mod, Str name) {
    for (u32 fi = 0; fi < mod.functions.len; fi++) {
        const auto& fn = mod.functions[fi];
        if (!is_hidden_import_runtime_function(fn.name) && fn.name.eq(name)) return true;
    }
    return false;
}

static bool imported_module_exports_selected_name(const HirModule& mod, Str name) {
    if (imported_module_exports_function_name(mod, name)) return true;
    for (u32 pi = 0; pi < mod.protocols.len; pi++) {
        const auto& proto = mod.protocols[pi];
        if (proto.kind == HirProtocolKind::Custom && proto.name.eq(name)) return true;
    }
    for (u32 si = 0; si < mod.structs.len; si++) {
        const auto& st = mod.structs[si];
        if (st.template_struct_index == 0xffffffffu && st.name.eq(name)) return true;
    }
    for (u32 vi = 0; vi < mod.variants.len; vi++) {
        const auto& variant = mod.variants[vi];
        if (variant.template_variant_index == 0xffffffffu && variant.name.eq(name)) return true;
    }
    return false;
}

static FrontendResult<void> validate_imported_selected_names(
    const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    for (u32 ii = 0; ii < imports.len; ii++) {
        const auto& imported = imports[ii];
        if (!imported.selective) continue;
        for (u32 si = 0; si < imported.selected_names.len; si++) {
            const auto& selected = imported.selected_names[si];
            if (!imported_module_exports_selected_name(*imported.module, selected.name))
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, selected.name);
        }
    }
    return {};
}

static FrontendResult<void> merge_imported_functions(
    HirModule* mod, const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    auto refresh_imported_function_signature_shapes = [&](HirFunction* fn,
                                                          Span span) -> FrontendResult<void> {
        for (u32 pi = 0; pi < fn->params.len; pi++) {
            auto& param = fn->params[pi];
            for (u32 ai = 0; ai < param.type_arg_count; ai++) {
                auto shape = intern_hir_type_shape(mod,
                                                   param.type_args[ai].type,
                                                   param.type_args[ai].generic_index,
                                                   param.type_args[ai].variant_index,
                                                   param.type_args[ai].struct_index,
                                                   param.type_args[ai].tuple_len,
                                                   param.type_args[ai].tuple_types,
                                                   param.type_args[ai].tuple_variant_indices,
                                                   param.type_args[ai].tuple_struct_indices,
                                                   span);
                if (!shape) return core::make_unexpected(shape.error());
                param.type_args[ai].shape_index = shape.value();
            }
            auto shape = intern_hir_type_shape(mod,
                                               param.type,
                                               param.generic_index,
                                               param.variant_index,
                                               param.struct_index,
                                               param.tuple_len,
                                               param.tuple_types,
                                               param.tuple_variant_indices,
                                               param.tuple_struct_indices,
                                               span);
            if (!shape) return core::make_unexpected(shape.error());
            param.shape_index = shape.value();
        }
        for (u32 ai = 0; ai < fn->return_type_arg_count; ai++) {
            auto shape = intern_hir_type_shape(mod,
                                               fn->return_type_args[ai].type,
                                               fn->return_type_args[ai].generic_index,
                                               fn->return_type_args[ai].variant_index,
                                               fn->return_type_args[ai].struct_index,
                                               fn->return_type_args[ai].tuple_len,
                                               fn->return_type_args[ai].tuple_types,
                                               fn->return_type_args[ai].tuple_variant_indices,
                                               fn->return_type_args[ai].tuple_struct_indices,
                                               span);
            if (!shape) return core::make_unexpected(shape.error());
            fn->return_type_args[ai].shape_index = shape.value();
        }
        if (fn->return_type != HirTypeKind::Unknown) {
            auto shape = intern_hir_type_shape(mod,
                                               fn->return_type,
                                               fn->return_generic_index,
                                               fn->return_variant_index,
                                               fn->return_struct_index,
                                               fn->return_tuple_len,
                                               fn->return_tuple_types,
                                               fn->return_tuple_variant_indices,
                                               fn->return_tuple_struct_indices,
                                               span);
            if (!shape) return core::make_unexpected(shape.error());
            fn->return_shape_index = shape.value();
        }
        return {};
    };
    auto refresh_imported_expr_shape = [&](HirExpr* expr, Span span) -> FrontendResult<void> {
        const bool should_refresh =
            expr->shape_index != 0xffffffffu || expr->kind == HirExprKind::StructInit ||
            expr->kind == HirExprKind::VariantCase || expr->kind == HirExprKind::Tuple ||
            expr->kind == HirExprKind::TupleSlot || expr->kind == HirExprKind::Field ||
            expr->kind == HirExprKind::IfElse || expr->kind == HirExprKind::Or ||
            expr->kind == HirExprKind::LocalRef || expr->kind == HirExprKind::MatchPayload ||
            expr->kind == HirExprKind::ValueOf || expr->kind == HirExprKind::MissingOf ||
            expr->kind == HirExprKind::ProtocolCall;
        if (!should_refresh || expr->type == HirTypeKind::Unknown) return {};
        auto shape = intern_hir_type_shape(mod,
                                           expr->type,
                                           expr->generic_index,
                                           expr->variant_index,
                                           expr->struct_index,
                                           expr->tuple_len,
                                           expr->tuple_types,
                                           expr->tuple_variant_indices,
                                           expr->tuple_struct_indices,
                                           span);
        if (!shape) return core::make_unexpected(shape.error());
        expr->shape_index = shape.value();
        return {};
    };
    auto refresh_imported_function_body_shapes = [&](HirFunction* fn,
                                                     Span span) -> FrontendResult<void> {
        for (u32 ei = 0; ei < fn->exprs.len; ei++) {
            auto refreshed = refresh_imported_expr_shape(&fn->exprs[ei], span);
            if (!refreshed) return core::make_unexpected(refreshed.error());
        }
        auto refreshed = refresh_imported_expr_shape(&fn->body, span);
        if (!refreshed) return core::make_unexpected(refreshed.error());
        return {};
    };

    for (u32 ii = 0; ii < imports.len; ii++) {
        const auto& imported = imports[ii];
        for (u32 fi = 0; fi < imported.module->functions.len; fi++) {
            const auto& fn = imported.module->functions[fi];
            if (is_hidden_import_runtime_function(fn.name)) {
                if (find_function_index(*mod, fn.name) != mod->functions.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, imported.span, fn.name);
                if (!mod->functions.push(fn))
                    return frontend_error(FrontendError::TooManyItems, imported.span);
                continue;
            }
            if (!imported.selective && !imported.has_namespace_alias) {
                if (find_function_index(*mod, fn.name) != mod->functions.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, imported.span, fn.name);
                HirFunction imported_fn = fn;
                auto refreshed =
                    refresh_imported_function_signature_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                refreshed = refresh_imported_function_body_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                if (!mod->functions.push(imported_fn))
                    return frontend_error(FrontendError::TooManyItems, imported.span);
                continue;
            }
            if (imported.has_namespace_alias) {
                HirFunction imported_fn = fn;
                imported_fn.name = import_visible_name(imported, fn.name);
                auto refreshed =
                    refresh_imported_function_signature_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                refreshed = refresh_imported_function_body_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                if (find_function_index(*mod, imported_fn.name) != mod->functions.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, imported_fn.name);
                if (!mod->functions.push(imported_fn))
                    return frontend_error(FrontendError::TooManyItems, imported.span);
                continue;
            }
            for (u32 si = 0; si < imported.selected_names.len; si++) {
                const auto& selected = imported.selected_names[si];
                if (!selected.name.eq(fn.name)) continue;
                HirFunction imported_fn = fn;
                if (selected.has_alias) imported_fn.name = selected.alias;
                auto refreshed =
                    refresh_imported_function_signature_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                refreshed = refresh_imported_function_body_shapes(&imported_fn, imported.span);
                if (!refreshed) return core::make_unexpected(refreshed.error());
                if (find_function_index(*mod, imported_fn.name) != mod->functions.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, imported_fn.name);
                if (!mod->functions.push(imported_fn))
                    return frontend_error(FrontendError::TooManyItems, imported.span);
            }
        }
    }
    return {};
}

static FrontendResult<void> merge_imported_simple_decls(
    HirModule* mod, const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    auto refresh_type_arg_shape = [&](HirVariant::TypeArgRef* arg,
                                      Span span) -> FrontendResult<void> {
        auto shape = intern_hir_type_shape(mod,
                                           arg->type,
                                           arg->generic_index,
                                           arg->variant_index,
                                           arg->struct_index,
                                           arg->tuple_len,
                                           arg->tuple_types,
                                           arg->tuple_variant_indices,
                                           arg->tuple_struct_indices,
                                           span);
        if (!shape) return core::make_unexpected(shape.error());
        arg->shape_index = shape.value();
        return {};
    };
    for (u32 ii = 0; ii < imports.len; ii++) {
        const auto& imported = imports[ii];
        for (u32 pi = 0; pi < imported.module->protocols.len; pi++) {
            const auto& proto = imported.module->protocols[pi];
            if (proto.kind != HirProtocolKind::Custom) continue;
            if (imported.selective) {
                bool selected = false;
                for (u32 si = 0; si < imported.selected_names.len; si++) {
                    if (imported.selected_names[si].name.eq(proto.name)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
            }
            HirProtocol imported_proto = proto;
            imported_proto.name = import_visible_name(imported, proto.name);
            for (u32 mi = 0; mi < imported_proto.methods.len; mi++) {
                auto& method = imported_proto.methods[mi];
                for (u32 ai = 0; ai < method.params.len; ai++) {
                    auto shape = intern_hir_type_shape(mod,
                                                       method.params[ai].type,
                                                       method.params[ai].generic_index,
                                                       method.params[ai].variant_index,
                                                       method.params[ai].struct_index,
                                                       method.params[ai].tuple_len,
                                                       method.params[ai].tuple_types,
                                                       method.params[ai].tuple_variant_indices,
                                                       method.params[ai].tuple_struct_indices,
                                                       imported.span);
                    if (!shape) return core::make_unexpected(shape.error());
                    method.params[ai].shape_index = shape.value();
                }
                if (method.has_return_type && method.return_type != HirTypeKind::Unknown) {
                    auto shape = intern_hir_type_shape(mod,
                                                       method.return_type,
                                                       method.return_generic_index,
                                                       method.return_variant_index,
                                                       method.return_struct_index,
                                                       method.return_tuple_len,
                                                       method.return_tuple_types,
                                                       method.return_tuple_variant_indices,
                                                       method.return_tuple_struct_indices,
                                                       imported.span);
                    if (!shape) return core::make_unexpected(shape.error());
                    method.return_shape_index = shape.value();
                }
            }
            if (find_protocol_index(*mod, imported_proto.name) != mod->protocols.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, imported_proto.name);
            if (!mod->protocols.push(imported_proto))
                return frontend_error(FrontendError::TooManyItems, imported.span);
        }
        for (u32 si = 0; si < imported.module->structs.len; si++) {
            const auto& st = imported.module->structs[si];
            if (st.template_struct_index != 0xffffffffu) continue;
            if (imported.selective) {
                bool selected = false;
                for (u32 xi = 0; xi < imported.selected_names.len; xi++) {
                    if (imported.selected_names[xi].name.eq(st.name)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
            }
            HirStruct imported_struct = st;
            imported_struct.name = import_visible_name(imported, st.name);
            for (u32 fi = 0; fi < imported_struct.fields.len; fi++) {
                if (imported_struct.fields[fi].is_error_type) continue;
                for (u32 ai = 0; ai < imported_struct.fields[fi].type_arg_count; ai++) {
                    auto refreshed = refresh_type_arg_shape(
                        &imported_struct.fields[fi].type_args[ai], imported.span);
                    if (!refreshed) return core::make_unexpected(refreshed.error());
                }
                auto shape = intern_hir_type_shape(mod,
                                                   imported_struct.fields[fi].type,
                                                   imported_struct.fields[fi].generic_index,
                                                   imported_struct.fields[fi].variant_index,
                                                   imported_struct.fields[fi].struct_index,
                                                   imported_struct.fields[fi].tuple_len,
                                                   imported_struct.fields[fi].tuple_types,
                                                   imported_struct.fields[fi].tuple_variant_indices,
                                                   imported_struct.fields[fi].tuple_struct_indices,
                                                   imported.span);
                if (!shape) return core::make_unexpected(shape.error());
                imported_struct.fields[fi].shape_index = shape.value();
            }
            if (find_struct_index(*mod, imported_struct.name) != mod->structs.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, imported_struct.name);
            bool ok = true;
            for (u32 fi = 0; fi < imported_struct.fields.len; fi++) {
                const auto& field = imported_struct.fields[fi];
                if (field.is_error_type) continue;
                if (!type_shape_is_simple_importable(field.type,
                                                     field.variant_index,
                                                     field.struct_index,
                                                     field.tuple_len,
                                                     field.tuple_types,
                                                     field.tuple_variant_indices,
                                                     field.tuple_struct_indices)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (!mod->structs.push(imported_struct))
                return frontend_error(FrontendError::TooManyItems, imported.span);
        }
        for (u32 vi = 0; vi < imported.module->variants.len; vi++) {
            const auto& variant = imported.module->variants[vi];
            if (variant.template_variant_index != 0xffffffffu) continue;
            if (imported.selective) {
                bool selected = false;
                for (u32 xi = 0; xi < imported.selected_names.len; xi++) {
                    if (imported.selected_names[xi].name.eq(variant.name)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
            }
            HirVariant imported_variant = variant;
            imported_variant.name = import_visible_name(imported, variant.name);
            for (u32 ci = 0; ci < imported_variant.cases.len; ci++) {
                if (!imported_variant.cases[ci].has_payload) continue;
                for (u32 ai = 0; ai < imported_variant.cases[ci].payload_type_arg_count; ai++) {
                    auto refreshed = refresh_type_arg_shape(
                        &imported_variant.cases[ci].payload_type_args[ai], imported.span);
                    if (!refreshed) return core::make_unexpected(refreshed.error());
                }
                auto shape =
                    intern_hir_type_shape(mod,
                                          imported_variant.cases[ci].payload_type,
                                          imported_variant.cases[ci].payload_generic_index,
                                          imported_variant.cases[ci].payload_variant_index,
                                          imported_variant.cases[ci].payload_struct_index,
                                          imported_variant.cases[ci].payload_tuple_len,
                                          imported_variant.cases[ci].payload_tuple_types,
                                          imported_variant.cases[ci].payload_tuple_variant_indices,
                                          imported_variant.cases[ci].payload_tuple_struct_indices,
                                          imported.span);
                if (!shape) return core::make_unexpected(shape.error());
                imported_variant.cases[ci].payload_shape_index = shape.value();
            }
            if (find_variant_index(*mod, imported_variant.name) != mod->variants.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, imported_variant.name);
            bool ok = true;
            for (u32 ci = 0; ci < imported_variant.cases.len; ci++) {
                const auto& c = imported_variant.cases[ci];
                if (!c.has_payload) continue;
                if (!type_shape_is_simple_importable(c.payload_type,
                                                     c.payload_variant_index,
                                                     c.payload_struct_index,
                                                     c.payload_tuple_len,
                                                     c.payload_tuple_types,
                                                     c.payload_tuple_variant_indices,
                                                     c.payload_tuple_struct_indices)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            if (!mod->variants.push(imported_variant))
                return frontend_error(FrontendError::TooManyItems, imported.span);
        }
    }
    return {};
}

static FrontendResult<void> refresh_imported_protocol_method_functions(
    HirModule* mod, const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    for (u32 ii = 0; ii < imports.len; ii++) {
        const auto& imported = imports[ii];
        for (u32 pi = 0; pi < imported.module->protocols.len; pi++) {
            const auto& imported_proto = imported.module->protocols[pi];
            if (imported_proto.kind != HirProtocolKind::Custom) continue;
            if (imported.selective) {
                bool selected = false;
                for (u32 si = 0; si < imported.selected_names.len; si++) {
                    if (imported.selected_names[si].name.eq(imported_proto.name)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
            }
            const Str visible_name = import_visible_name(imported, imported_proto.name);
            const u32 proto_index = find_protocol_index(*mod, visible_name);
            if (proto_index >= mod->protocols.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, visible_name);
            auto& proto = mod->protocols[proto_index];
            for (u32 mi = 0; mi < imported_proto.methods.len; mi++) {
                const auto& imported_method = imported_proto.methods[mi];
                auto* method = find_protocol_method_mut(proto, imported_method.name);
                if (method == nullptr)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, imported_method.name);
                if (imported_method.function_index == 0xffffffffu) continue;
                if (imported_method.function_index >= imported.module->functions.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, imported_method.name);
                const Str fn_name = imported.module->functions[imported_method.function_index].name;
                const u32 mapped_fn_index = find_function_index(*mod, fn_name);
                if (mapped_fn_index >= mod->functions.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, imported.span, fn_name);
                method->function_index = mapped_fn_index;
                method->return_may_nil = mod->functions[mapped_fn_index].body.may_nil;
                method->return_may_error = mod->functions[mapped_fn_index].body.may_error;
                method->return_error_struct_index =
                    mod->functions[mapped_fn_index].body.error_struct_index;
                method->return_error_variant_index =
                    mod->functions[mapped_fn_index].body.error_variant_index;
            }
        }
    }
    return {};
}

static FrontendResult<void> remap_imported_impl_target(HirModule* mod,
                                                       const ImportedModuleInfo& imported,
                                                       const HirModule& imported_mod,
                                                       const HirImpl& imported_impl,
                                                       HirTypeKind& out_type,
                                                       u32& out_struct_index,
                                                       bool& out_is_generic_template,
                                                       Span span) {
    std::function<FrontendResult<void>(HirTypeKind,
                                       u32,
                                       u32,
                                       u32,
                                       const HirTypeKind*,
                                       const u32*,
                                       const u32*,
                                       u32&,
                                       u32&,
                                       u32*,
                                       u32*)>
        remap_imported_type_metadata;
    std::function<FrontendResult<void>(u32, u32&)> remap_imported_struct_index;
    std::function<FrontendResult<void>(u32, u32&)> remap_imported_variant_index;

    remap_imported_struct_index = [&](u32 imported_struct_index,
                                      u32& mapped_struct_index) -> FrontendResult<void> {
        if (imported_struct_index >= imported_mod.structs.len)
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        const auto& imported_struct = imported_mod.structs[imported_struct_index];
        if (imported_struct.template_struct_index == 0xffffffffu) {
            mapped_struct_index =
                find_struct_index(*mod, import_visible_name(imported, imported_struct.name));
            if (mapped_struct_index >= mod->structs.len)
                return frontend_error(FrontendError::UnsupportedSyntax, span, imported_struct.name);
            return {};
        }
        if (imported_struct.template_struct_index >= imported_mod.structs.len)
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        u32 mapped_template_index = 0xffffffffu;
        auto remapped_template = remap_imported_struct_index(imported_struct.template_struct_index,
                                                             mapped_template_index);
        if (!remapped_template) return core::make_unexpected(remapped_template.error());
        GenericBinding bindings[HirStruct::kMaxTypeParams]{};
        for (u32 ai = 0; ai < imported_struct.instance_type_arg_count; ai++) {
            u32 mapped_variant_index = 0xffffffffu;
            u32 mapped_arg_struct_index = 0xffffffffu;
            u32 mapped_tuple_variant_indices[kMaxTupleSlots]{};
            u32 mapped_tuple_struct_indices[kMaxTupleSlots]{};
            for (u32 ti = 0; ti < imported_struct.instance_tuple_lens[ai]; ti++) {
                mapped_tuple_variant_indices[ti] = 0xffffffffu;
                mapped_tuple_struct_indices[ti] = 0xffffffffu;
            }
            auto remapped =
                remap_imported_type_metadata(imported_struct.instance_type_args[ai],
                                             imported_struct.instance_variant_indices[ai],
                                             imported_struct.instance_struct_indices[ai],
                                             imported_struct.instance_tuple_lens[ai],
                                             imported_struct.instance_tuple_types[ai],
                                             imported_struct.instance_tuple_variant_indices[ai],
                                             imported_struct.instance_tuple_struct_indices[ai],
                                             mapped_variant_index,
                                             mapped_arg_struct_index,
                                             mapped_tuple_variant_indices,
                                             mapped_tuple_struct_indices);
            if (!remapped) return core::make_unexpected(remapped.error());
            auto filled =
                fill_bound_binding_from_type_metadata(&bindings[ai],
                                                      mod,
                                                      imported_struct.instance_type_args[ai],
                                                      imported_struct.instance_generic_indices[ai],
                                                      mapped_variant_index,
                                                      mapped_arg_struct_index,
                                                      imported_struct.instance_tuple_lens[ai],
                                                      imported_struct.instance_tuple_types[ai],
                                                      mapped_tuple_variant_indices,
                                                      mapped_tuple_struct_indices,
                                                      0xffffffffu,
                                                      span);
            if (!filled) return core::make_unexpected(filled.error());
        }
        auto concrete = instantiate_struct(
            mod, mapped_template_index, bindings, imported_struct.instance_type_arg_count, span);
        if (!concrete) return core::make_unexpected(concrete.error());
        mapped_struct_index = concrete.value();
        return {};
    };
    remap_imported_variant_index = [&](u32 imported_variant_index,
                                       u32& mapped_variant_index) -> FrontendResult<void> {
        if (imported_variant_index >= imported_mod.variants.len)
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        const auto& imported_variant = imported_mod.variants[imported_variant_index];
        if (imported_variant.template_variant_index == 0xffffffffu) {
            mapped_variant_index =
                find_variant_index(*mod, import_visible_name(imported, imported_variant.name));
            if (mapped_variant_index >= mod->variants.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, span, imported_variant.name);
            return {};
        }
        if (imported_variant.template_variant_index >= imported_mod.variants.len)
            return frontend_error(FrontendError::UnsupportedSyntax, span);
        u32 mapped_template_index = 0xffffffffu;
        auto remapped_template = remap_imported_variant_index(
            imported_variant.template_variant_index, mapped_template_index);
        if (!remapped_template) return core::make_unexpected(remapped_template.error());
        GenericBinding bindings[HirVariant::kMaxTypeParams]{};
        for (u32 ai = 0; ai < imported_variant.instance_type_arg_count; ai++) {
            u32 mapped_arg_variant_index = 0xffffffffu;
            u32 mapped_struct_index = 0xffffffffu;
            u32 mapped_tuple_variant_indices[kMaxTupleSlots]{};
            u32 mapped_tuple_struct_indices[kMaxTupleSlots]{};
            for (u32 ti = 0; ti < imported_variant.instance_tuple_lens[ai]; ti++) {
                mapped_tuple_variant_indices[ti] = 0xffffffffu;
                mapped_tuple_struct_indices[ti] = 0xffffffffu;
            }
            auto remapped =
                remap_imported_type_metadata(imported_variant.instance_type_args[ai],
                                             imported_variant.instance_variant_indices[ai],
                                             imported_variant.instance_struct_indices[ai],
                                             imported_variant.instance_tuple_lens[ai],
                                             imported_variant.instance_tuple_types[ai],
                                             imported_variant.instance_tuple_variant_indices[ai],
                                             imported_variant.instance_tuple_struct_indices[ai],
                                             mapped_arg_variant_index,
                                             mapped_struct_index,
                                             mapped_tuple_variant_indices,
                                             mapped_tuple_struct_indices);
            if (!remapped) return core::make_unexpected(remapped.error());
            auto filled =
                fill_bound_binding_from_type_metadata(&bindings[ai],
                                                      mod,
                                                      imported_variant.instance_type_args[ai],
                                                      imported_variant.instance_generic_indices[ai],
                                                      mapped_arg_variant_index,
                                                      mapped_struct_index,
                                                      imported_variant.instance_tuple_lens[ai],
                                                      imported_variant.instance_tuple_types[ai],
                                                      mapped_tuple_variant_indices,
                                                      mapped_tuple_struct_indices,
                                                      0xffffffffu,
                                                      span);
            if (!filled) return core::make_unexpected(filled.error());
        }
        auto concrete = instantiate_variant(
            mod, mapped_template_index, bindings, imported_variant.instance_type_arg_count, span);
        if (!concrete) return core::make_unexpected(concrete.error());
        mapped_variant_index = concrete.value();
        return {};
    };
    remap_imported_type_metadata = [&](HirTypeKind imported_type,
                                       u32 imported_variant_index,
                                       u32 imported_struct_index,
                                       u32 tuple_len,
                                       const HirTypeKind* tuple_types,
                                       const u32* tuple_variant_indices,
                                       const u32* tuple_struct_indices,
                                       u32& mapped_variant_index,
                                       u32& mapped_struct_index,
                                       u32* mapped_tuple_variant_indices,
                                       u32* mapped_tuple_struct_indices) -> FrontendResult<void> {
        mapped_variant_index = 0xffffffffu;
        mapped_struct_index = 0xffffffffu;
        for (u32 ti = 0; ti < tuple_len; ti++) {
            mapped_tuple_variant_indices[ti] = 0xffffffffu;
            mapped_tuple_struct_indices[ti] = 0xffffffffu;
            if (tuple_types[ti] == HirTypeKind::Variant &&
                tuple_variant_indices[ti] != 0xffffffffu) {
                auto remapped_tuple_variant = remap_imported_variant_index(
                    tuple_variant_indices[ti], mapped_tuple_variant_indices[ti]);
                if (!remapped_tuple_variant)
                    return core::make_unexpected(remapped_tuple_variant.error());
            }
            if (tuple_types[ti] == HirTypeKind::Struct && tuple_struct_indices[ti] != 0xffffffffu) {
                auto remapped_tuple_struct = remap_imported_struct_index(
                    tuple_struct_indices[ti], mapped_tuple_struct_indices[ti]);
                if (!remapped_tuple_struct)
                    return core::make_unexpected(remapped_tuple_struct.error());
            }
        }
        if (imported_type == HirTypeKind::Variant && imported_variant_index != 0xffffffffu) {
            auto remapped_variant =
                remap_imported_variant_index(imported_variant_index, mapped_variant_index);
            if (!remapped_variant) return core::make_unexpected(remapped_variant.error());
        }
        if (imported_type == HirTypeKind::Struct && imported_struct_index != 0xffffffffu) {
            auto remapped_struct =
                remap_imported_struct_index(imported_struct_index, mapped_struct_index);
            if (!remapped_struct) return core::make_unexpected(remapped_struct.error());
        }
        return {};
    };
    out_type = imported_impl.type;
    out_is_generic_template = imported_impl.is_generic_template;
    out_struct_index = imported_impl.struct_index;
    if (imported_impl.type != HirTypeKind::Struct) return {};
    if (imported_impl.struct_index >= imported_mod.structs.len)
        return frontend_error(FrontendError::UnsupportedSyntax, span);
    const auto& imported_target = imported_mod.structs[imported_impl.struct_index];
    if (imported_impl.is_generic_template || imported_target.template_struct_index == 0xffffffffu) {
        const u32 mapped =
            find_struct_index(*mod, import_visible_name(imported, imported_target.name));
        if (mapped >= mod->structs.len)
            return frontend_error(FrontendError::UnsupportedSyntax, span, imported_target.name);
        out_struct_index = mapped;
        return {};
    }
    if (imported_target.template_struct_index >= imported_mod.structs.len)
        return frontend_error(FrontendError::UnsupportedSyntax, span);
    const auto& imported_template = imported_mod.structs[imported_target.template_struct_index];
    const u32 mapped_template =
        find_struct_index(*mod, import_visible_name(imported, imported_template.name));
    if (mapped_template >= mod->structs.len)
        return frontend_error(FrontendError::UnsupportedSyntax, span, imported_template.name);
    GenericBinding bindings[HirStruct::kMaxTypeParams]{};
    const u32 binding_count = imported_target.instance_type_arg_count;
    for (u32 bi = 0; bi < binding_count; bi++) {
        u32 mapped_tuple_variant_indices[kMaxTupleSlots]{};
        u32 mapped_tuple_struct_indices[kMaxTupleSlots]{};
        u32 mapped_variant_index = 0xffffffffu;
        u32 mapped_arg_struct_index = 0xffffffffu;
        auto remapped =
            remap_imported_type_metadata(imported_target.instance_type_args[bi],
                                         imported_target.instance_variant_indices[bi],
                                         imported_target.instance_struct_indices[bi],
                                         imported_target.instance_tuple_lens[bi],
                                         imported_target.instance_tuple_types[bi],
                                         imported_target.instance_tuple_variant_indices[bi],
                                         imported_target.instance_tuple_struct_indices[bi],
                                         mapped_variant_index,
                                         mapped_arg_struct_index,
                                         mapped_tuple_variant_indices,
                                         mapped_tuple_struct_indices);
        if (!remapped) return core::make_unexpected(remapped.error());
        auto filled =
            fill_bound_binding_from_type_metadata(&bindings[bi],
                                                  mod,
                                                  imported_target.instance_type_args[bi],
                                                  imported_target.instance_generic_indices[bi],
                                                  mapped_variant_index,
                                                  mapped_arg_struct_index,
                                                  imported_target.instance_tuple_lens[bi],
                                                  imported_target.instance_tuple_types[bi],
                                                  mapped_tuple_variant_indices,
                                                  mapped_tuple_struct_indices,
                                                  0xffffffffu,
                                                  span);
        if (!filled) return core::make_unexpected(filled.error());
    }
    auto concrete = instantiate_struct(mod, mapped_template, bindings, binding_count, span);
    if (!concrete) return core::make_unexpected(concrete.error());
    out_struct_index = concrete.value();
    return {};
}

static FrontendResult<void> merge_imported_impls(
    HirModule* mod, const FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& imports) {
    for (u32 ii = 0; ii < imports.len; ii++) {
        const auto& imported = imports[ii];
        for (u32 impl_i = 0; impl_i < imported.module->impls.len; impl_i++) {
            const auto& imported_impl = imported.module->impls[impl_i];
            if (imported_impl.protocol_index >= imported.module->protocols.len)
                return frontend_error(FrontendError::UnsupportedSyntax, imported.span);
            const Str protocol_name = import_visible_name(
                imported, imported.module->protocols[imported_impl.protocol_index].name);
            if (imported.selective) {
                bool selected = false;
                for (u32 si = 0; si < imported.selected_names.len; si++) {
                    if (imported.selected_names[si].name.eq(
                            imported.module->protocols[imported_impl.protocol_index].name)) {
                        selected = true;
                        break;
                    }
                }
                if (!selected) continue;
            }
            const u32 mapped_protocol_index = find_protocol_index(*mod, protocol_name);
            if (mapped_protocol_index >= mod->protocols.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, imported.span, protocol_name);
            HirTypeKind mapped_type = HirTypeKind::Unknown;
            u32 mapped_struct_index = 0xffffffffu;
            bool mapped_is_generic_template = false;
            auto remapped = remap_imported_impl_target(mod,
                                                       imported,
                                                       *imported.module,
                                                       imported_impl,
                                                       mapped_type,
                                                       mapped_struct_index,
                                                       mapped_is_generic_template,
                                                       imported.span);
            if (!remapped) return core::make_unexpected(remapped.error());
            for (u32 existing_i = 0; existing_i < mod->impls.len; existing_i++) {
                const auto& existing = mod->impls[existing_i];
                if (existing.protocol_index == mapped_protocol_index &&
                    impl_targets_overlap(*mod,
                                         existing,
                                         mapped_type,
                                         mapped_struct_index,
                                         mapped_is_generic_template))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, protocol_name);
            }
            HirConformance conf{};
            conf.span = imported_impl.span;
            conf.protocol_index = mapped_protocol_index;
            conf.type = mapped_type;
            conf.struct_index = mapped_struct_index;
            conf.is_generic_template = mapped_is_generic_template;
            for (u32 ci = 0; ci < mod->conformances.len; ci++) {
                const auto& existing = mod->conformances[ci];
                if (existing.protocol_index == conf.protocol_index && existing.type == conf.type &&
                    existing.struct_index == conf.struct_index &&
                    existing.is_generic_template == conf.is_generic_template)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, protocol_name);
            }
            if (!mod->conformances.push(conf))
                return frontend_error(FrontendError::TooManyItems, imported.span);
            HirImpl impl = imported_impl;
            impl.span = imported_impl.span;
            impl.protocol_index = mapped_protocol_index;
            impl.type = mapped_type;
            impl.struct_index = mapped_struct_index;
            impl.is_generic_template = mapped_is_generic_template;
            impl.methods.len = 0;
            for (u32 mi = 0; mi < imported_impl.methods.len; mi++) {
                const auto& imported_method = imported_impl.methods[mi];
                if (imported_method.function_index >= imported.module->functions.len)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, imported.span, imported_method.name);
                const Str fn_name = imported.module->functions[imported_method.function_index].name;
                const u32 mapped_fn_index = find_function_index(*mod, fn_name);
                if (mapped_fn_index >= mod->functions.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, imported.span, fn_name);
                HirImplMethod method{};
                method.name = imported_method.name;
                method.function_index = mapped_fn_index;
                if (!impl.methods.push(method))
                    return frontend_error(FrontendError::TooManyItems, imported.span);
            }
            if (!mod->impls.push(impl))
                return frontend_error(FrontendError::TooManyItems, imported.span);
        }
    }
    return {};
}

static FrontendResult<void> load_imported_modules(
    FixedVec<ImportedModuleInfo, AstFile::kMaxItems>& out,
    const AstFile& file,
    Str source_path,
    std::deque<std::string>& owned_strings,
    std::vector<std::string>& import_stack,
    std::vector<std::unique_ptr<HirModule>>& imported_storage) {
    if (source_path.len == 0) return {};
    const auto base_dir = std::filesystem::path(str_to_std_string(source_path)).parent_path();
    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Import) continue;
        const auto normalized =
            (base_dir / str_to_std_string(item.import_decl.path)).lexically_normal().string();
        ImportedModuleInfo* existing_info = nullptr;
        for (u32 ii = 0; ii < out.len; ii++) {
            if (out[ii].path.eq({normalized.c_str(), static_cast<u32>(normalized.size())}) &&
                out[ii].has_namespace_alias == item.import_decl.has_namespace_alias &&
                (!out[ii].has_namespace_alias ||
                 out[ii].namespace_alias.eq(item.import_decl.namespace_alias))) {
                existing_info = &out[ii];
                break;
            }
        }
        if (existing_info != nullptr) {
            if (!item.import_decl.selective) {
                existing_info->selective = false;
                existing_info->selected_names.len = 0;
            } else if (existing_info->selective) {
                for (u32 si = 0; si < item.import_decl.selected_names.len; si++) {
                    const auto& selected = item.import_decl.selected_names[si];
                    bool seen = false;
                    for (u32 ei = 0; ei < existing_info->selected_names.len; ei++) {
                        const auto& existing = existing_info->selected_names[ei];
                        if (existing.name.eq(selected.name) &&
                            existing.has_alias == selected.has_alias &&
                            (!existing.has_alias || existing.alias.eq(selected.alias))) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        if (!existing_info->selected_names.push(selected))
                            return frontend_error(FrontendError::TooManyItems,
                                                  item.import_decl.span);
                    }
                }
            }
            continue;
        }
        std::string content;
        if (!read_text_file(normalized, content))
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.import_decl.span, item.import_decl.path);
        auto kept_source = stash_owned_string(owned_strings, content);
        auto lexed = lex(kept_source);
        if (!lexed) return core::make_unexpected(lexed.error());
        auto ast = parse_file(lexed.value());
        if (!ast) return core::make_unexpected(ast.error());
        // parse_file returns a raw pointer via unique_ptr::release(); take
        // ownership immediately so the imported AstFile is freed after analyze
        // consumes it. Without this wrapper an import-heavy test suite leaks
        // ~58 MB per imported file (confirmed via RSS growth).
        std::unique_ptr<AstFile> ast_owned(ast.value());
        auto kept_path = stash_owned_string(owned_strings, normalized);
        g_import_analysis_counter++;
        auto imported =
            analyze_file_internal(*ast_owned, kept_path, import_stack, &owned_strings);
        if (!imported) return core::make_unexpected(imported.error());
        imported_storage.push_back(std::unique_ptr<HirModule>(imported.value()));
        ImportedModuleInfo info{};
        info.span = item.import_decl.span;
        info.path = kept_path;
        info.module = imported_storage.back().get();
        info.selective = item.import_decl.selective;
        info.has_namespace_alias = item.import_decl.has_namespace_alias;
        info.namespace_alias = item.import_decl.namespace_alias;
        for (u32 si = 0; si < item.import_decl.selected_names.len; si++) {
            if (!info.selected_names.push(item.import_decl.selected_names[si]))
                return frontend_error(FrontendError::TooManyItems, item.import_decl.span);
        }
        if (!out.push(info))
            return frontend_error(FrontendError::TooManyItems, item.import_decl.span);
    }
    return {};
}

static FrontendResult<HirModule*> analyze_file_internal(
    const AstFile& file,
    Str source_path,
    std::vector<std::string>& import_stack,
    std::deque<std::string>* shared_owned_strings) {
    auto mod_ptr = std::make_unique<HirModule>();
    HirModule& mod = *mod_ptr;
    mod.has_package_decl = file.has_package_decl;
    mod.package_span = file.package_span;
    mod.package_name = file.package_name;
    std::string normalized_source;
    if (source_path.len != 0) {
        normalized_source =
            std::filesystem::path(str_to_std_string(source_path)).lexically_normal().string();
        for (const auto& existing : import_stack) {
            if (existing == normalized_source)
                return frontend_error(FrontendError::UnsupportedSyntax, {}, source_path);
        }
        import_stack.push_back(normalized_source);
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Upstream) continue;
        for (u32 j = 0; j < mod.upstreams.len; j++) {
            if (mod.upstreams[j].name.eq(item.upstream.name))
                return frontend_error(
                    FrontendError::DuplicateUpstream, item.upstream.span, item.upstream.name);
        }
        HirUpstream up{};
        up.span = item.upstream.span;
        up.name = item.upstream.name;
        up.id = static_cast<u16>(mod.upstreams.len + 1);
        if (!mod.upstreams.push(up))
            return frontend_error(FrontendError::TooManyItems, item.upstream.span);
    }

    {
        HirProtocol p{};
        p.name = {"Error", 5};
        p.kind = HirProtocolKind::Error;
        if (!mod.protocols.push(p)) return frontend_error(FrontendError::TooManyItems, {});
        HirProtocol q{};
        q.name = {"Eq", 2};
        q.kind = HirProtocolKind::Eq;
        if (!mod.protocols.push(q)) return frontend_error(FrontendError::TooManyItems, {});
        HirProtocol r{};
        r.name = {"Ord", 3};
        r.kind = HirProtocolKind::Ord;
        if (!mod.protocols.push(r)) return frontend_error(FrontendError::TooManyItems, {});
    }

    auto* owned_strings =
        shared_owned_strings != nullptr ? shared_owned_strings : &mod.owned_strings;
    std::vector<std::unique_ptr<HirModule>> imported_storage;
    FixedVec<ImportedModuleInfo, AstFile::kMaxItems> imported_modules;
    auto loaded_imports = load_imported_modules(
        imported_modules, file, source_path, *owned_strings, import_stack, imported_storage);
    if (!loaded_imports) return core::make_unexpected(loaded_imports.error());
    auto validated_namespaces = validate_import_namespaces(imported_modules);
    if (!validated_namespaces) return core::make_unexpected(validated_namespaces.error());
    auto validated_namespace_bindings = validate_import_namespace_bindings(file, imported_modules);
    if (!validated_namespace_bindings)
        return core::make_unexpected(validated_namespace_bindings.error());
    auto validated_imports = validate_imported_selected_names(imported_modules);
    if (!validated_imports) return core::make_unexpected(validated_imports.error());

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Import) continue;
        bool exists = false;
        for (u32 ii = 0; ii < mod.imports.len; ii++) {
            if (mod.imports[ii].path.eq(item.import_decl.path) &&
                mod.imports[ii].has_namespace_alias == item.import_decl.has_namespace_alias &&
                (!mod.imports[ii].has_namespace_alias ||
                 mod.imports[ii].namespace_alias.eq(item.import_decl.namespace_alias))) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            HirImport imp{};
            imp.span = item.import_decl.span;
            imp.path = item.import_decl.path;
            imp.selective = item.import_decl.selective;
            imp.has_namespace_alias = item.import_decl.has_namespace_alias;
            imp.namespace_alias = item.import_decl.namespace_alias;
            std::string normalized_import_path;
            if (source_path.len != 0) {
                normalized_import_path =
                    (std::filesystem::path(str_to_std_string(source_path)).parent_path() /
                     str_to_std_string(item.import_decl.path))
                        .lexically_normal()
                        .string();
            }
            for (u32 ii = 0; ii < imported_modules.len; ii++) {
                if (!normalized_import_path.empty() &&
                    !imported_modules[ii].path.eq(
                        {normalized_import_path.c_str(),
                         static_cast<u32>(normalized_import_path.size())}))
                    continue;
                imp.has_package_decl = imported_modules[ii].module->has_package_decl;
                imp.package_name = imported_modules[ii].module->package_name;
                imp.same_package = mod.has_package_decl &&
                                   imported_modules[ii].module->has_package_decl &&
                                   mod.package_name.eq(imported_modules[ii].module->package_name);
                break;
            }
            if (!mod.imports.push(imp))
                return frontend_error(FrontendError::TooManyItems, item.import_decl.span);
        }
    }

    auto imported_decls = merge_imported_simple_decls(&mod, imported_modules);
    if (!imported_decls) return core::make_unexpected(imported_decls.error());

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Protocol) continue;
        if (find_protocol_index(mod, item.protocol.name) < mod.protocols.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.protocol.span, item.protocol.name);
        HirProtocol proto{};
        proto.span = item.protocol.span;
        proto.name = item.protocol.name;
        proto.kind = HirProtocolKind::Custom;
        for (u32 mi = 0; mi < item.protocol.methods.len; mi++) {
            for (u32 seen = 0; seen < proto.methods.len; seen++) {
                if (proto.methods[seen].name.eq(item.protocol.methods[mi].name))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          item.protocol.span,
                                          item.protocol.methods[mi].name);
            }
            HirProtocol::MethodDecl method{};
            method.name = item.protocol.methods[mi].name;
            for (u32 pi = 0; pi < item.protocol.methods[mi].params.len; pi++) {
                HirProtocol::MethodDecl::ParamDecl param{};
                param.type_name = item.protocol.methods[mi].params[pi].type.name;
                auto param_type = resolve_func_type_ref(mod,
                                                        item.protocol.methods[mi].params[pi].type,
                                                        nullptr,
                                                        &param.generic_index,
                                                        param.variant_index,
                                                        param.struct_index,
                                                        param.tuple_len,
                                                        param.tuple_types,
                                                        param.tuple_variant_indices,
                                                        param.tuple_struct_indices,
                                                        item.protocol.span);
                if (!param_type) return core::make_unexpected(param_type.error());
                param.type = param_type.value();
                auto param_shape = intern_hir_type_shape(&mod,
                                                         param.type,
                                                         param.generic_index,
                                                         param.variant_index,
                                                         param.struct_index,
                                                         param.tuple_len,
                                                         param.tuple_types,
                                                         param.tuple_variant_indices,
                                                         param.tuple_struct_indices,
                                                         item.protocol.span);
                if (!param_shape) return core::make_unexpected(param_shape.error());
                param.shape_index = param_shape.value();
                if (!method.params.push(param))
                    return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            }
            method.has_return_type = item.protocol.methods[mi].has_return_type;
            method.return_type_name = item.protocol.methods[mi].return_type.name;
            if (method.has_return_type) {
                auto return_type = resolve_func_type_ref(mod,
                                                         item.protocol.methods[mi].return_type,
                                                         nullptr,
                                                         &method.return_generic_index,
                                                         method.return_variant_index,
                                                         method.return_struct_index,
                                                         method.return_tuple_len,
                                                         method.return_tuple_types,
                                                         method.return_tuple_variant_indices,
                                                         method.return_tuple_struct_indices,
                                                         item.protocol.span);
                if (!return_type) return core::make_unexpected(return_type.error());
                method.return_type = return_type.value();
                auto return_shape = intern_hir_type_shape(&mod,
                                                          method.return_type,
                                                          method.return_generic_index,
                                                          method.return_variant_index,
                                                          method.return_struct_index,
                                                          method.return_tuple_len,
                                                          method.return_tuple_types,
                                                          method.return_tuple_variant_indices,
                                                          method.return_tuple_struct_indices,
                                                          item.protocol.span);
                if (!return_shape) return core::make_unexpected(return_shape.error());
                method.return_shape_index = return_shape.value();
            }
            if (!proto.methods.push(method))
                return frontend_error(FrontendError::TooManyItems, item.protocol.span);
        }
        if (!mod.protocols.push(proto))
            return frontend_error(FrontendError::TooManyItems, item.protocol.span);
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Struct) continue;
        if (item.struct_decl.name.eq({"Error", 5}))
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.struct_decl.span, item.struct_decl.name);
        if (find_struct_index(mod, item.struct_decl.name) != mod.structs.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.struct_decl.span, item.struct_decl.name);
        HirStruct decl{};
        decl.span = item.struct_decl.span;
        decl.name = item.struct_decl.name;
        decl.type_params = item.struct_decl.type_params;
        if (!mod.structs.push(decl))
            return frontend_error(FrontendError::TooManyItems, item.struct_decl.span);
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Variant) continue;
        if (find_variant_index(mod, item.variant.name) != mod.variants.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.variant.span, item.variant.name);
        HirVariant variant{};
        variant.span = item.variant.span;
        variant.name = item.variant.name;
        variant.type_params = item.variant.type_params;
        if (!mod.variants.push(variant))
            return frontend_error(FrontendError::TooManyItems, item.variant.span);
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Variant) continue;
        const u32 variant_decl_index = find_variant_index(mod, item.variant.name);
        if (variant_decl_index == mod.variants.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.variant.span, item.variant.name);
        HirVariant& variant = mod.variants[variant_decl_index];
        for (u32 ci = 0; ci < item.variant.cases.len; ci++) {
            for (u32 seen = 0; seen < variant.cases.len; seen++) {
                if (variant.cases[seen].name.eq(item.variant.cases[ci].name))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          item.variant.span,
                                          item.variant.cases[ci].name);
            }
            HirVariant::CaseDecl case_decl{};
            case_decl.name = item.variant.cases[ci].name;
            case_decl.has_payload = item.variant.cases[ci].has_payload;
            if (case_decl.has_payload) {
                FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>
                    variant_type_params;
                for (u32 ti = 0; ti < variant.type_params.len; ti++) {
                    HirFunction::TypeParamDecl tp{};
                    tp.name = variant.type_params[ti];
                    if (!variant_type_params.push(tp))
                        return frontend_error(FrontendError::TooManyItems, item.variant.span);
                }
                const auto& payload_ref = item.variant.cases[ci].payload_type;
                if (!payload_ref.is_tuple && payload_ref.type_arg_names.len != 0) {
                    u32 template_variant_index = 0xffffffffu;
                    u32 template_struct_index = 0xffffffffu;
                    const auto named_kind = resolve_named_type(
                        mod, payload_ref.name, template_variant_index, template_struct_index);
                    const bool is_generic_named =
                        (named_kind == HirTypeKind::Variant &&
                         template_variant_index < mod.variants.len &&
                         mod.variants[template_variant_index].type_params.len ==
                             payload_ref.type_arg_names.len) ||
                        (named_kind == HirTypeKind::Struct &&
                         template_struct_index < mod.structs.len &&
                         mod.structs[template_struct_index].type_params.len ==
                             payload_ref.type_arg_names.len);
                    if (is_generic_named) {
                        bool has_generic_arg = false;
                        case_decl.payload_type = named_kind;
                        case_decl.payload_template_variant_index = template_variant_index;
                        case_decl.payload_template_struct_index = template_struct_index;
                        case_decl.payload_type_arg_count = payload_ref.type_arg_names.len;
                        for (u32 ai = 0; ai < payload_ref.type_arg_names.len; ai++) {
                            AstTypeRef arg_ref = get_ast_type_arg_ref(payload_ref, ai);
                            auto arg_type = resolve_func_type_ref(
                                mod,
                                arg_ref,
                                &variant_type_params,
                                &case_decl.payload_type_args[ai].generic_index,
                                case_decl.payload_type_args[ai].variant_index,
                                case_decl.payload_type_args[ai].struct_index,
                                case_decl.payload_type_args[ai].tuple_len,
                                case_decl.payload_type_args[ai].tuple_types,
                                case_decl.payload_type_args[ai].tuple_variant_indices,
                                case_decl.payload_type_args[ai].tuple_struct_indices,
                                item.variant.span);
                            if (!arg_type) return core::make_unexpected(arg_type.error());
                            case_decl.payload_type_args[ai].type = arg_type.value();
                            auto arg_shape = intern_hir_type_shape(
                                &mod,
                                case_decl.payload_type_args[ai].type,
                                case_decl.payload_type_args[ai].generic_index,
                                case_decl.payload_type_args[ai].variant_index,
                                case_decl.payload_type_args[ai].struct_index,
                                case_decl.payload_type_args[ai].tuple_len,
                                case_decl.payload_type_args[ai].tuple_types,
                                case_decl.payload_type_args[ai].tuple_variant_indices,
                                case_decl.payload_type_args[ai].tuple_struct_indices,
                                item.variant.span);
                            if (!arg_shape) return core::make_unexpected(arg_shape.error());
                            case_decl.payload_type_args[ai].shape_index = arg_shape.value();
                            if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                        }
                        if (has_generic_arg) {
                            GenericBinding bindings[HirVariant::kMaxTypeParams]{};
                            for (u32 ai = 0; ai < payload_ref.type_arg_names.len; ai++) {
                                auto filled = fill_bound_binding_from_type_metadata(
                                    &bindings[ai],
                                    &mod,
                                    case_decl.payload_type_args[ai].type,
                                    case_decl.payload_type_args[ai].generic_index,
                                    case_decl.payload_type_args[ai].variant_index,
                                    case_decl.payload_type_args[ai].struct_index,
                                    case_decl.payload_type_args[ai].tuple_len,
                                    case_decl.payload_type_args[ai].tuple_types,
                                    case_decl.payload_type_args[ai].tuple_variant_indices,
                                    case_decl.payload_type_args[ai].tuple_struct_indices,
                                    case_decl.payload_type_args[ai].shape_index,
                                    item.variant.span);
                                if (!filled) return core::make_unexpected(filled.error());
                            }
                            if (named_kind == HirTypeKind::Variant) {
                                auto concrete = instantiate_variant(&mod,
                                                                    template_variant_index,
                                                                    bindings,
                                                                    payload_ref.type_arg_names.len,
                                                                    item.variant.span);
                                if (!concrete) return core::make_unexpected(concrete.error());
                                case_decl.payload_variant_index = concrete.value();
                                case_decl.payload_struct_index = 0xffffffffu;
                            } else {
                                auto concrete = instantiate_struct(&mod,
                                                                   template_struct_index,
                                                                   bindings,
                                                                   payload_ref.type_arg_names.len,
                                                                   item.variant.span);
                                if (!concrete) return core::make_unexpected(concrete.error());
                                case_decl.payload_struct_index = concrete.value();
                                case_decl.payload_variant_index = 0xffffffffu;
                            }
                            auto payload_shape =
                                intern_hir_type_shape(&mod,
                                                      case_decl.payload_type,
                                                      case_decl.payload_generic_index,
                                                      case_decl.payload_variant_index,
                                                      case_decl.payload_struct_index,
                                                      case_decl.payload_tuple_len,
                                                      case_decl.payload_tuple_types,
                                                      case_decl.payload_tuple_variant_indices,
                                                      case_decl.payload_tuple_struct_indices,
                                                      item.variant.span);
                            if (!payload_shape) return core::make_unexpected(payload_shape.error());
                            case_decl.payload_shape_index = payload_shape.value();
                            if (!variant.cases.push(case_decl))
                                return frontend_error(FrontendError::TooManyItems,
                                                      item.variant.span);
                            continue;
                        }
                    }
                }
                auto payload_type = resolve_func_type_ref(mod,
                                                          payload_ref,
                                                          &variant_type_params,
                                                          &case_decl.payload_generic_index,
                                                          case_decl.payload_variant_index,
                                                          case_decl.payload_struct_index,
                                                          case_decl.payload_tuple_len,
                                                          case_decl.payload_tuple_types,
                                                          case_decl.payload_tuple_variant_indices,
                                                          case_decl.payload_tuple_struct_indices,
                                                          item.variant.span);
                if (!payload_type) return core::make_unexpected(payload_type.error());
                case_decl.payload_type = payload_type.value();
                auto payload_shape = intern_hir_type_shape(&mod,
                                                           case_decl.payload_type,
                                                           case_decl.payload_generic_index,
                                                           case_decl.payload_variant_index,
                                                           case_decl.payload_struct_index,
                                                           case_decl.payload_tuple_len,
                                                           case_decl.payload_tuple_types,
                                                           case_decl.payload_tuple_variant_indices,
                                                           case_decl.payload_tuple_struct_indices,
                                                           item.variant.span);
                if (!payload_shape) return core::make_unexpected(payload_shape.error());
                case_decl.payload_shape_index = payload_shape.value();
            }
            if (!variant.cases.push(case_decl))
                return frontend_error(FrontendError::TooManyItems, item.variant.span);
        }
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Struct) continue;
        const u32 struct_decl_index = find_struct_index(mod, item.struct_decl.name);
        if (struct_decl_index == mod.structs.len)
            return frontend_error(
                FrontendError::UnsupportedSyntax, item.struct_decl.span, item.struct_decl.name);
        HirStruct& decl = mod.structs[struct_decl_index];
        for (u32 fi = 0; fi < item.struct_decl.fields.len; fi++) {
            for (u32 seen = 0; seen < decl.fields.len; seen++) {
                if (decl.fields[seen].name.eq(item.struct_decl.fields[fi].name))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          item.struct_decl.span,
                                          item.struct_decl.fields[fi].name);
            }
            HirStruct::FieldDecl field{};
            field.name = item.struct_decl.fields[fi].name;
            if (!item.struct_decl.fields[fi].type.is_tuple &&
                item.struct_decl.fields[fi].type.name.eq({"Error", 5})) {
                field.type_name = item.struct_decl.fields[fi].type.name;
                field.is_error_type = true;
            } else {
                FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>
                    struct_type_params;
                for (u32 ti = 0; ti < decl.type_params.len; ti++) {
                    HirFunction::TypeParamDecl tp{};
                    tp.name = decl.type_params[ti];
                    if (!struct_type_params.push(tp))
                        return frontend_error(FrontendError::TooManyItems, item.struct_decl.span);
                }
                const auto& type_ref = item.struct_decl.fields[fi].type;
                if (!type_ref.is_tuple && type_ref.type_arg_names.len != 0) {
                    u32 template_variant_index = 0xffffffffu;
                    u32 template_struct_index = 0xffffffffu;
                    const auto named_kind = resolve_named_type(
                        mod, type_ref.name, template_variant_index, template_struct_index);
                    const bool is_generic_named =
                        (named_kind == HirTypeKind::Variant &&
                         template_variant_index < mod.variants.len &&
                         mod.variants[template_variant_index].type_params.len ==
                             type_ref.type_arg_names.len) ||
                        (named_kind == HirTypeKind::Struct &&
                         template_struct_index < mod.structs.len &&
                         mod.structs[template_struct_index].type_params.len ==
                             type_ref.type_arg_names.len);
                    if (is_generic_named) {
                        bool has_generic_arg = false;
                        field.type = named_kind;
                        field.template_variant_index = template_variant_index;
                        field.template_struct_index = template_struct_index;
                        field.type_arg_count = type_ref.type_arg_names.len;
                        for (u32 ai = 0; ai < type_ref.type_arg_names.len; ai++) {
                            AstTypeRef arg_ref = get_ast_type_arg_ref(type_ref, ai);
                            auto arg_type =
                                resolve_func_type_ref(mod,
                                                      arg_ref,
                                                      &struct_type_params,
                                                      &field.type_args[ai].generic_index,
                                                      field.type_args[ai].variant_index,
                                                      field.type_args[ai].struct_index,
                                                      field.type_args[ai].tuple_len,
                                                      field.type_args[ai].tuple_types,
                                                      field.type_args[ai].tuple_variant_indices,
                                                      field.type_args[ai].tuple_struct_indices,
                                                      item.struct_decl.span);
                            if (!arg_type) return core::make_unexpected(arg_type.error());
                            field.type_args[ai].type = arg_type.value();
                            auto arg_shape =
                                intern_hir_type_shape(&mod,
                                                      field.type_args[ai].type,
                                                      field.type_args[ai].generic_index,
                                                      field.type_args[ai].variant_index,
                                                      field.type_args[ai].struct_index,
                                                      field.type_args[ai].tuple_len,
                                                      field.type_args[ai].tuple_types,
                                                      field.type_args[ai].tuple_variant_indices,
                                                      field.type_args[ai].tuple_struct_indices,
                                                      item.struct_decl.span);
                            if (!arg_shape) return core::make_unexpected(arg_shape.error());
                            field.type_args[ai].shape_index = arg_shape.value();
                            if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                        }
                        if (has_generic_arg) {
                            GenericBinding bindings[HirStruct::kMaxTypeParams]{};
                            for (u32 ai = 0; ai < type_ref.type_arg_names.len; ai++) {
                                auto filled = fill_bound_binding_from_type_metadata(
                                    &bindings[ai],
                                    &mod,
                                    field.type_args[ai].type,
                                    field.type_args[ai].generic_index,
                                    field.type_args[ai].variant_index,
                                    field.type_args[ai].struct_index,
                                    field.type_args[ai].tuple_len,
                                    field.type_args[ai].tuple_types,
                                    field.type_args[ai].tuple_variant_indices,
                                    field.type_args[ai].tuple_struct_indices,
                                    field.type_args[ai].shape_index,
                                    item.struct_decl.span);
                                if (!filled) return core::make_unexpected(filled.error());
                            }
                            if (named_kind == HirTypeKind::Variant) {
                                auto concrete = instantiate_variant(&mod,
                                                                    template_variant_index,
                                                                    bindings,
                                                                    type_ref.type_arg_names.len,
                                                                    item.struct_decl.span);
                                if (!concrete) return core::make_unexpected(concrete.error());
                                field.variant_index = concrete.value();
                                field.struct_index = 0xffffffffu;
                            } else {
                                auto concrete = instantiate_struct(&mod,
                                                                   template_struct_index,
                                                                   bindings,
                                                                   type_ref.type_arg_names.len,
                                                                   item.struct_decl.span);
                                if (!concrete) return core::make_unexpected(concrete.error());
                                field.struct_index = concrete.value();
                                field.variant_index = 0xffffffffu;
                            }
                            field.type_name = type_ref.name;
                            if (!decl.fields.push(field))
                                return frontend_error(FrontendError::TooManyItems,
                                                      item.struct_decl.span);
                            continue;
                        }
                    }
                }
                u32 variant_index = 0xffffffffu;
                u32 struct_index = 0xffffffffu;
                u32 tuple_len = 0;
                HirTypeKind tuple_types[kMaxTupleSlots]{};
                u32 tuple_variant_indices[kMaxTupleSlots]{};
                u32 tuple_struct_indices[kMaxTupleSlots]{};
                auto resolved = resolve_func_type_ref(mod,
                                                      type_ref,
                                                      &struct_type_params,
                                                      &field.generic_index,
                                                      variant_index,
                                                      struct_index,
                                                      tuple_len,
                                                      tuple_types,
                                                      tuple_variant_indices,
                                                      tuple_struct_indices,
                                                      item.struct_decl.span);
                if (!resolved) return core::make_unexpected(resolved.error());
                field.type = resolved.value();
                if (field.type == HirTypeKind::Generic &&
                    field.generic_index < struct_type_params.len) {
                    field.generic_has_error_constraint =
                        struct_type_params[field.generic_index].has_error_constraint;
                    field.generic_has_eq_constraint =
                        struct_type_params[field.generic_index].has_eq_constraint;
                    field.generic_has_ord_constraint =
                        struct_type_params[field.generic_index].has_ord_constraint;
                    field.generic_protocol_index =
                        struct_type_params[field.generic_index].custom_protocol_count != 0
                            ? struct_type_params[field.generic_index].custom_protocol_indices[0]
                            : 0xffffffffu;
                    field.generic_protocol_count =
                        struct_type_params[field.generic_index].custom_protocol_count;
                    for (u32 cpi = 0; cpi < field.generic_protocol_count; cpi++)
                        field.generic_protocol_indices[cpi] =
                            struct_type_params[field.generic_index].custom_protocol_indices[cpi];
                }
                field.variant_index = variant_index;
                field.struct_index = struct_index;
                field.tuple_len = tuple_len;
                field.type_name = item.struct_decl.fields[fi].type.name;
                for (u32 ti = 0; ti < tuple_len; ti++) {
                    field.tuple_types[ti] = tuple_types[ti];
                    field.tuple_variant_indices[ti] = tuple_variant_indices[ti];
                    field.tuple_struct_indices[ti] = tuple_struct_indices[ti];
                }
            }
            if (!field.is_error_type) {
                auto field_shape = intern_hir_type_shape(&mod,
                                                         field.type,
                                                         field.generic_index,
                                                         field.variant_index,
                                                         field.struct_index,
                                                         field.tuple_len,
                                                         field.tuple_types,
                                                         field.tuple_variant_indices,
                                                         field.tuple_struct_indices,
                                                         item.struct_decl.span);
                if (!field_shape) return core::make_unexpected(field_shape.error());
                field.shape_index = field_shape.value();
            }
            if (!decl.fields.push(field))
                return frontend_error(FrontendError::TooManyItems, item.struct_decl.span);
        }
        for (u32 fi = 0; fi < decl.fields.len; fi++) {
            if (decl.fields[fi].name.eq({"err", 3}) && decl.fields[fi].is_error_type) {
                decl.conforms_error = true;
                break;
            }
        }
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind == AstItemKind::Import) continue;
        if (item.kind != AstItemKind::Using) continue;
        for (u32 ai = 0; ai < mod.aliases.len; ai++) {
            if (mod.aliases[ai].name.eq(item.using_decl.name))
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.using_decl.span, item.using_decl.name);
        }
        HirAlias alias{};
        alias.span = item.using_decl.span;
        alias.name = item.using_decl.name;
        for (u32 pi = 0; pi < item.using_decl.target_parts.len; pi++) {
            if (!alias.target_parts.push(item.using_decl.target_parts[pi]))
                return frontend_error(FrontendError::TooManyItems, item.using_decl.span);
        }
        if (!mod.aliases.push(alias))
            return frontend_error(FrontendError::TooManyItems, item.using_decl.span);
    }

    auto imported = merge_imported_functions(&mod, imported_modules);
    if (!imported) return core::make_unexpected(imported.error());
    auto imported_proto_methods =
        refresh_imported_protocol_method_functions(&mod, imported_modules);
    if (!imported_proto_methods) return core::make_unexpected(imported_proto_methods.error());
    auto imported_impls = merge_imported_impls(&mod, imported_modules);
    if (!imported_impls) return core::make_unexpected(imported_impls.error());

    auto refreshed_structs = refresh_concrete_struct_instances(&mod);
    if (!refreshed_structs) {
        if (source_path.len != 0 && !import_stack.empty()) import_stack.pop_back();
        return core::make_unexpected(refreshed_structs.error());
    }
    auto refreshed_variants = refresh_concrete_variant_instances(&mod);
    if (!refreshed_variants) {
        if (source_path.len != 0 && !import_stack.empty()) import_stack.pop_back();
        return core::make_unexpected(refreshed_variants.error());
    }

    auto declare_function_like =
        [&](const AstFunctionDecl& ast_func,
            Span span,
            Str fn_name,
            const FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>*
                extra_type_params) -> FrontendResult<HirFunction> {
        HirFunction fn{};
        fn.span = span;
        fn.name = fn_name;
        if (extra_type_params != nullptr) {
            for (u32 ti = 0; ti < extra_type_params->len; ti++) {
                if (!fn.type_params.push((*extra_type_params)[ti]))
                    return frontend_error(FrontendError::TooManyItems, span);
            }
        }
        for (u32 ti = 0; ti < ast_func.type_params.len; ti++) {
            for (u32 seen = 0; seen < fn.type_params.len; seen++) {
                if (fn.type_params[seen].name.eq(ast_func.type_params[ti].name))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, span, ast_func.type_params[ti].name);
            }
            HirFunction::TypeParamDecl type_param{};
            type_param.name = ast_func.type_params[ti].name;
            if (ast_func.type_params[ti].has_constraint) {
                type_param.has_constraint = true;
                type_param.constraint = ast_func.type_params[ti].constraint;
                for (u32 ci = 0; ci < ast_func.type_params[ti].constraints.len; ci++) {
                    const auto constraint = ast_func.type_params[ti].constraints[ci];
                    Str resolved_constraint = constraint;
                    if (ci < ast_func.type_params[ti].constraint_namespaces.len &&
                        ast_func.type_params[ti].constraint_namespaces[ci].len != 0) {
                        if (!resolve_import_namespace_type_name(
                                mod,
                                ast_func.type_params[ti].constraint_namespaces[ci],
                                constraint,
                                resolved_constraint))
                            return frontend_error(
                                FrontendError::UnsupportedSyntax, span, constraint);
                    }
                    const auto kind = resolve_protocol_kind(mod, resolved_constraint);
                    if (kind == static_cast<HirProtocolKind>(0xff))
                        return frontend_error(FrontendError::UnsupportedSyntax, span, constraint);
                    type_param.constraints[ci] = resolved_constraint;
                    type_param.constraint_kinds[ci] = kind;
                    if (kind == HirProtocolKind::Error) type_param.has_error_constraint = true;
                    if (kind == HirProtocolKind::Eq) type_param.has_eq_constraint = true;
                    if (kind == HirProtocolKind::Ord) type_param.has_ord_constraint = true;
                    if (kind == HirProtocolKind::Custom) {
                        const u32 pi = find_protocol_index(mod, resolved_constraint);
                        if (pi >= mod.protocols.len)
                            return frontend_error(
                                FrontendError::UnsupportedSyntax, span, constraint);
                        type_param.custom_protocol_indices[type_param.custom_protocol_count++] = pi;
                    }
                }
                type_param.constraint_kind = ast_func.type_params[ti].constraints.len != 0
                                                 ? type_param.constraint_kinds[0]
                                                 : HirProtocolKind::Custom;
            }
            if (!fn.type_params.push(type_param))
                return frontend_error(FrontendError::TooManyItems, span);
        }
        if (ast_func.has_return_type) {
            const auto& ret_ref = ast_func.return_type;
            if (!ret_ref.is_tuple && ret_ref.type_arg_names.len != 0) {
                u32 template_variant_index = 0xffffffffu;
                u32 template_struct_index = 0xffffffffu;
                const auto named_kind = resolve_named_type(
                    mod, ret_ref.name, template_variant_index, template_struct_index);
                const bool is_generic_named =
                    (named_kind == HirTypeKind::Variant &&
                     template_variant_index < mod.variants.len &&
                     mod.variants[template_variant_index].type_params.len ==
                         ret_ref.type_arg_names.len) ||
                    (named_kind == HirTypeKind::Struct && template_struct_index < mod.structs.len &&
                     mod.structs[template_struct_index].type_params.len ==
                         ret_ref.type_arg_names.len);
                if (is_generic_named) {
                    bool has_generic_arg = false;
                    GenericBinding bindings[HirFunction::kMaxTypeParams]{};
                    fn.return_type = named_kind;
                    fn.return_template_variant_index = template_variant_index;
                    fn.return_template_struct_index = template_struct_index;
                    fn.return_type_arg_count = ret_ref.type_arg_names.len;
                    for (u32 ai = 0; ai < ret_ref.type_arg_names.len; ai++) {
                        AstTypeRef arg_ref = get_ast_type_arg_ref(ret_ref, ai);
                        auto arg_type =
                            resolve_func_type_ref(mod,
                                                  arg_ref,
                                                  &fn.type_params,
                                                  &fn.return_type_args[ai].generic_index,
                                                  fn.return_type_args[ai].variant_index,
                                                  fn.return_type_args[ai].struct_index,
                                                  fn.return_type_args[ai].tuple_len,
                                                  fn.return_type_args[ai].tuple_types,
                                                  fn.return_type_args[ai].tuple_variant_indices,
                                                  fn.return_type_args[ai].tuple_struct_indices,
                                                  span);
                        if (!arg_type) return core::make_unexpected(arg_type.error());
                        fn.return_type_args[ai].type = arg_type.value();
                        auto arg_shape =
                            intern_hir_type_shape(&mod,
                                                  fn.return_type_args[ai].type,
                                                  fn.return_type_args[ai].generic_index,
                                                  fn.return_type_args[ai].variant_index,
                                                  fn.return_type_args[ai].struct_index,
                                                  fn.return_type_args[ai].tuple_len,
                                                  fn.return_type_args[ai].tuple_types,
                                                  fn.return_type_args[ai].tuple_variant_indices,
                                                  fn.return_type_args[ai].tuple_struct_indices,
                                                  span);
                        if (!arg_shape) return core::make_unexpected(arg_shape.error());
                        fn.return_type_args[ai].shape_index = arg_shape.value();
                        auto filled = fill_bound_binding_from_type_metadata(
                            &bindings[ai],
                            &mod,
                            fn.return_type_args[ai].type,
                            fn.return_type_args[ai].generic_index,
                            fn.return_type_args[ai].variant_index,
                            fn.return_type_args[ai].struct_index,
                            fn.return_type_args[ai].tuple_len,
                            fn.return_type_args[ai].tuple_types,
                            fn.return_type_args[ai].tuple_variant_indices,
                            fn.return_type_args[ai].tuple_struct_indices,
                            fn.return_type_args[ai].shape_index,
                            span);
                        if (!filled) return core::make_unexpected(filled.error());
                        copy_binding_constraints_from_type_params(&bindings[ai], fn.type_params);
                        if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                    }
                    if (has_generic_arg) {
                        if (named_kind == HirTypeKind::Variant) {
                            auto concrete = instantiate_variant(&mod,
                                                                template_variant_index,
                                                                bindings,
                                                                ret_ref.type_arg_names.len,
                                                                span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            fn.return_variant_index = concrete.value();
                        } else {
                            auto concrete = instantiate_struct(&mod,
                                                               template_struct_index,
                                                               bindings,
                                                               ret_ref.type_arg_names.len,
                                                               span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            fn.return_struct_index = concrete.value();
                        }
                    } else {
                        auto ret_type = resolve_func_type_ref(mod,
                                                              ret_ref,
                                                              &fn.type_params,
                                                              &fn.return_generic_index,
                                                              fn.return_variant_index,
                                                              fn.return_struct_index,
                                                              fn.return_tuple_len,
                                                              fn.return_tuple_types,
                                                              fn.return_tuple_variant_indices,
                                                              fn.return_tuple_struct_indices,
                                                              span);
                        if (!ret_type) return core::make_unexpected(ret_type.error());
                        fn.return_type = ret_type.value();
                    }
                } else {
                    auto ret_type = resolve_func_type_ref(mod,
                                                          ret_ref,
                                                          &fn.type_params,
                                                          &fn.return_generic_index,
                                                          fn.return_variant_index,
                                                          fn.return_struct_index,
                                                          fn.return_tuple_len,
                                                          fn.return_tuple_types,
                                                          fn.return_tuple_variant_indices,
                                                          fn.return_tuple_struct_indices,
                                                          span);
                    if (!ret_type) return core::make_unexpected(ret_type.error());
                    fn.return_type = ret_type.value();
                }
            } else {
                auto ret_type = resolve_func_type_ref(mod,
                                                      ast_func.return_type,
                                                      &fn.type_params,
                                                      &fn.return_generic_index,
                                                      fn.return_variant_index,
                                                      fn.return_struct_index,
                                                      fn.return_tuple_len,
                                                      fn.return_tuple_types,
                                                      fn.return_tuple_variant_indices,
                                                      fn.return_tuple_struct_indices,
                                                      span);
                if (!ret_type) return core::make_unexpected(ret_type.error());
                fn.return_type = ret_type.value();
            }
        }
        for (u32 pi = 0; pi < ast_func.params.len; pi++) {
            for (u32 seen = 0; seen < fn.params.len; seen++) {
                if (fn.params[seen].name.eq(ast_func.params[pi].name))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, span, ast_func.params[pi].name);
            }
            HirFunction::ParamDecl param{};
            param.name = ast_func.params[pi].name;
            param.has_underscore_label = ast_func.params[pi].has_underscore_label;
            const auto& param_ref = ast_func.params[pi].type;
            if (!param_ref.is_tuple && param_ref.type_arg_names.len != 0) {
                u32 template_variant_index = 0xffffffffu;
                u32 template_struct_index = 0xffffffffu;
                const auto named_kind = resolve_named_type(
                    mod, param_ref.name, template_variant_index, template_struct_index);
                const bool is_generic_named =
                    (named_kind == HirTypeKind::Variant &&
                     template_variant_index < mod.variants.len &&
                     mod.variants[template_variant_index].type_params.len ==
                         param_ref.type_arg_names.len) ||
                    (named_kind == HirTypeKind::Struct && template_struct_index < mod.structs.len &&
                     mod.structs[template_struct_index].type_params.len ==
                         param_ref.type_arg_names.len);
                if (is_generic_named) {
                    bool has_generic_arg = false;
                    GenericBinding bindings[HirFunction::kMaxTypeParams]{};
                    param.type = named_kind;
                    param.template_variant_index = template_variant_index;
                    param.template_struct_index = template_struct_index;
                    param.type_arg_count = param_ref.type_arg_names.len;
                    for (u32 ai = 0; ai < param_ref.type_arg_names.len; ai++) {
                        AstTypeRef arg_ref = get_ast_type_arg_ref(param_ref, ai);
                        auto arg_type =
                            resolve_func_type_ref(mod,
                                                  arg_ref,
                                                  &fn.type_params,
                                                  &param.type_args[ai].generic_index,
                                                  param.type_args[ai].variant_index,
                                                  param.type_args[ai].struct_index,
                                                  param.type_args[ai].tuple_len,
                                                  param.type_args[ai].tuple_types,
                                                  param.type_args[ai].tuple_variant_indices,
                                                  param.type_args[ai].tuple_struct_indices,
                                                  span);
                        if (!arg_type) return core::make_unexpected(arg_type.error());
                        param.type_args[ai].type = arg_type.value();
                        auto arg_shape =
                            intern_hir_type_shape(&mod,
                                                  param.type_args[ai].type,
                                                  param.type_args[ai].generic_index,
                                                  param.type_args[ai].variant_index,
                                                  param.type_args[ai].struct_index,
                                                  param.type_args[ai].tuple_len,
                                                  param.type_args[ai].tuple_types,
                                                  param.type_args[ai].tuple_variant_indices,
                                                  param.type_args[ai].tuple_struct_indices,
                                                  span);
                        if (!arg_shape) return core::make_unexpected(arg_shape.error());
                        param.type_args[ai].shape_index = arg_shape.value();
                        auto filled = fill_bound_binding_from_type_metadata(
                            &bindings[ai],
                            &mod,
                            param.type_args[ai].type,
                            param.type_args[ai].generic_index,
                            param.type_args[ai].variant_index,
                            param.type_args[ai].struct_index,
                            param.type_args[ai].tuple_len,
                            param.type_args[ai].tuple_types,
                            param.type_args[ai].tuple_variant_indices,
                            param.type_args[ai].tuple_struct_indices,
                            param.type_args[ai].shape_index,
                            span);
                        if (!filled) return core::make_unexpected(filled.error());
                        copy_binding_constraints_from_type_params(&bindings[ai], fn.type_params);
                        if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                    }
                    if (has_generic_arg) {
                        if (named_kind == HirTypeKind::Variant) {
                            auto concrete = instantiate_variant(&mod,
                                                                template_variant_index,
                                                                bindings,
                                                                param_ref.type_arg_names.len,
                                                                span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            param.variant_index = concrete.value();
                        } else {
                            auto concrete = instantiate_struct(&mod,
                                                               template_struct_index,
                                                               bindings,
                                                               param_ref.type_arg_names.len,
                                                               span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            param.struct_index = concrete.value();
                        }
                    } else {
                        auto param_type = resolve_func_type_ref(mod,
                                                                param_ref,
                                                                &fn.type_params,
                                                                &param.generic_index,
                                                                param.variant_index,
                                                                param.struct_index,
                                                                param.tuple_len,
                                                                param.tuple_types,
                                                                param.tuple_variant_indices,
                                                                param.tuple_struct_indices,
                                                                span);
                        if (!param_type) return core::make_unexpected(param_type.error());
                        param.type = param_type.value();
                    }
                } else {
                    auto param_type = resolve_func_type_ref(mod,
                                                            param_ref,
                                                            &fn.type_params,
                                                            &param.generic_index,
                                                            param.variant_index,
                                                            param.struct_index,
                                                            param.tuple_len,
                                                            param.tuple_types,
                                                            param.tuple_variant_indices,
                                                            param.tuple_struct_indices,
                                                            span);
                    if (!param_type) return core::make_unexpected(param_type.error());
                    param.type = param_type.value();
                }
            } else {
                auto param_type = resolve_func_type_ref(mod,
                                                        param_ref,
                                                        &fn.type_params,
                                                        &param.generic_index,
                                                        param.variant_index,
                                                        param.struct_index,
                                                        param.tuple_len,
                                                        param.tuple_types,
                                                        param.tuple_variant_indices,
                                                        param.tuple_struct_indices,
                                                        span);
                if (!param_type) return core::make_unexpected(param_type.error());
                param.type = param_type.value();
            }
            if (param.type == HirTypeKind::Generic && param.generic_index < fn.type_params.len)
                param.generic_has_error_constraint =
                    fn.type_params[param.generic_index].has_error_constraint;
            if (param.type == HirTypeKind::Generic && param.generic_index < fn.type_params.len) {
                param.generic_has_eq_constraint =
                    fn.type_params[param.generic_index].has_eq_constraint;
                param.generic_has_ord_constraint =
                    fn.type_params[param.generic_index].has_ord_constraint;
                param.generic_protocol_index =
                    fn.type_params[param.generic_index].custom_protocol_count != 0
                        ? fn.type_params[param.generic_index].custom_protocol_indices[0]
                        : 0xffffffffu;
                param.generic_protocol_count =
                    fn.type_params[param.generic_index].custom_protocol_count;
                for (u32 cpi = 0; cpi < param.generic_protocol_count; cpi++)
                    param.generic_protocol_indices[cpi] =
                        fn.type_params[param.generic_index].custom_protocol_indices[cpi];
            }
            if (!fn.params.push(param)) return frontend_error(FrontendError::TooManyItems, span);
        }
        if (fn.return_type != HirTypeKind::Unknown) {
            auto return_shape = intern_hir_type_shape(&mod,
                                                      fn.return_type,
                                                      fn.return_generic_index,
                                                      fn.return_variant_index,
                                                      fn.return_struct_index,
                                                      fn.return_tuple_len,
                                                      fn.return_tuple_types,
                                                      fn.return_tuple_variant_indices,
                                                      fn.return_tuple_struct_indices,
                                                      span);
            if (!return_shape) return core::make_unexpected(return_shape.error());
            fn.return_shape_index = return_shape.value();
        }
        for (u32 pi = 0; pi < fn.params.len; pi++) {
            auto param_shape = intern_hir_type_shape(&mod,
                                                     fn.params[pi].type,
                                                     fn.params[pi].generic_index,
                                                     fn.params[pi].variant_index,
                                                     fn.params[pi].struct_index,
                                                     fn.params[pi].tuple_len,
                                                     fn.params[pi].tuple_types,
                                                     fn.params[pi].tuple_variant_indices,
                                                     fn.params[pi].tuple_struct_indices,
                                                     span);
            if (!param_shape) return core::make_unexpected(param_shape.error());
            fn.params[pi].shape_index = param_shape.value();
        }
        return fn;
    };

    auto analyze_function_body_like =
        [&](HirFunction& fn, const AstFunctionDecl& ast_func, Span span) -> FrontendResult<void> {
        HirRoute scratch{};
        FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases> ast_named_error_cases;
        auto ast_collected = collect_named_error_cases_ast(*ast_func.body, ast_named_error_cases);
        if (!ast_collected) return core::make_unexpected(ast_collected.error());
        if (ast_named_error_cases.len != 0) {
            HirVariant error_variant{};
            error_variant.span = span;
            error_variant.name = {"__error_func", 12};
            for (u32 ci = 0; ci < ast_named_error_cases.len; ci++) {
                HirVariant::CaseDecl case_decl{};
                case_decl.name = ast_named_error_cases[ci].name;
                if (!error_variant.cases.push(case_decl))
                    return frontend_error(FrontendError::TooManyItems, span);
            }
            if (!mod.variants.push(error_variant))
                return frontend_error(FrontendError::TooManyItems, span);
            scratch.error_variant_index = mod.variants.len - 1;
        }
        HirLocal param_locals[AstFunctionDecl::kMaxParams]{};
        for (u32 pi = 0; pi < fn.params.len; pi++) {
            param_locals[pi].span = span;
            param_locals[pi].name = fn.params[pi].name;
            param_locals[pi].ref_index = pi;
            param_locals[pi].type = fn.params[pi].type;
            param_locals[pi].generic_index = fn.params[pi].generic_index;
            param_locals[pi].generic_has_error_constraint =
                fn.params[pi].generic_has_error_constraint;
            param_locals[pi].generic_has_eq_constraint = fn.params[pi].generic_has_eq_constraint;
            param_locals[pi].generic_has_ord_constraint = fn.params[pi].generic_has_ord_constraint;
            param_locals[pi].generic_protocol_index = fn.params[pi].generic_protocol_index;
            param_locals[pi].generic_protocol_count = fn.params[pi].generic_protocol_count;
            for (u32 cpi = 0; cpi < param_locals[pi].generic_protocol_count; cpi++)
                param_locals[pi].generic_protocol_indices[cpi] =
                    fn.params[pi].generic_protocol_indices[cpi];
            param_locals[pi].variant_index = fn.params[pi].variant_index;
            param_locals[pi].struct_index = fn.params[pi].struct_index;
            param_locals[pi].shape_index = fn.params[pi].shape_index;
            param_locals[pi].tuple_len = fn.params[pi].tuple_len;
            for (u32 ti = 0; ti < fn.params[pi].tuple_len; ti++) {
                param_locals[pi].tuple_types[ti] = fn.params[pi].tuple_types[ti];
                param_locals[pi].tuple_variant_indices[ti] =
                    fn.params[pi].tuple_variant_indices[ti];
                param_locals[pi].tuple_struct_indices[ti] = fn.params[pi].tuple_struct_indices[ti];
            }
            param_locals[pi].init.kind = HirExprKind::LocalRef;
            param_locals[pi].init.type = fn.params[pi].type;
            param_locals[pi].init.generic_index = fn.params[pi].generic_index;
            param_locals[pi].init.generic_has_error_constraint =
                fn.params[pi].generic_has_error_constraint;
            param_locals[pi].init.generic_has_eq_constraint =
                fn.params[pi].generic_has_eq_constraint;
            param_locals[pi].init.generic_has_ord_constraint =
                fn.params[pi].generic_has_ord_constraint;
            param_locals[pi].init.generic_protocol_index = fn.params[pi].generic_protocol_index;
            param_locals[pi].init.generic_protocol_count = fn.params[pi].generic_protocol_count;
            for (u32 cpi = 0; cpi < param_locals[pi].init.generic_protocol_count; cpi++)
                param_locals[pi].init.generic_protocol_indices[cpi] =
                    fn.params[pi].generic_protocol_indices[cpi];
            param_locals[pi].init.local_index = pi;
            param_locals[pi].init.variant_index = fn.params[pi].variant_index;
            param_locals[pi].init.struct_index = fn.params[pi].struct_index;
            param_locals[pi].init.shape_index = fn.params[pi].shape_index;
            param_locals[pi].init.tuple_len = fn.params[pi].tuple_len;
            for (u32 ti = 0; ti < fn.params[pi].tuple_len; ti++) {
                param_locals[pi].init.tuple_types[ti] = fn.params[pi].tuple_types[ti];
                param_locals[pi].init.tuple_variant_indices[ti] =
                    fn.params[pi].tuple_variant_indices[ti];
                param_locals[pi].init.tuple_struct_indices[ti] =
                    fn.params[pi].tuple_struct_indices[ti];
            }
        }
        auto body = analyze_function_body_stmt(
            *ast_func.body, &scratch, mod, param_locals, fn.params.len, nullptr);
        if (!body) return core::make_unexpected(body.error());
        FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases> named_error_cases;
        for (u32 li = 0; li < scratch.locals.len; li++) {
            auto collected = collect_named_error_cases(scratch.locals[li].init, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
        }
        auto collected = collect_named_error_cases(body.value(), named_error_cases);
        if (!collected) return core::make_unexpected(collected.error());
        if (named_error_cases.len != 0 && scratch.error_variant_index == 0xffffffffu) {
            HirVariant error_variant{};
            error_variant.span = span;
            error_variant.name = {"__error_func", 12};
            for (u32 ci = 0; ci < named_error_cases.len; ci++) {
                HirVariant::CaseDecl case_decl{};
                case_decl.name = named_error_cases[ci].name;
                if (!error_variant.cases.push(case_decl))
                    return frontend_error(FrontendError::TooManyItems, span);
            }
            if (!mod.variants.push(error_variant))
                return frontend_error(FrontendError::TooManyItems, span);
            scratch.error_variant_index = mod.variants.len - 1;
        }
        if (named_error_cases.len != 0 && scratch.error_variant_index != 0xffffffffu) {
            const u32 error_variant_index = scratch.error_variant_index;
            for (u32 li = 0; li < scratch.locals.len; li++) {
                patch_named_error_variant(
                    &scratch.locals[li].init, error_variant_index, named_error_cases);
                patch_error_variant_refs(&scratch.locals[li].init, error_variant_index);
                if (scratch.locals[li].may_error &&
                    scratch.locals[li].error_variant_index == 0xffffffffu)
                    scratch.locals[li].error_variant_index = error_variant_index;
            }
            patch_named_error_variant(&body.value(), error_variant_index, named_error_cases);
            patch_error_variant_refs(&body.value(), error_variant_index);
        }
        if (fn.return_type == HirTypeKind::Unknown) {
            if (body->type == HirTypeKind::Unknown)
                return frontend_error(FrontendError::UnsupportedSyntax, ast_func.body->span);
            fn.return_type = body->type;
            fn.return_generic_index = body->generic_index;
            if (body->type == HirTypeKind::Variant) fn.return_variant_index = body->variant_index;
            if (body->type == HirTypeKind::Struct) fn.return_struct_index = body->struct_index;
            if (body->type == HirTypeKind::Tuple) {
                fn.return_tuple_len = body->tuple_len;
                for (u32 ti = 0; ti < body->tuple_len; ti++) {
                    fn.return_tuple_types[ti] = body->tuple_types[ti];
                    fn.return_tuple_variant_indices[ti] = body->tuple_variant_indices[ti];
                    fn.return_tuple_struct_indices[ti] = body->tuple_struct_indices[ti];
                }
            }
        } else {
            if (body->type == HirTypeKind::Unknown && (body->may_nil || body->may_error)) {
                body->type = fn.return_type;
                body->generic_index = fn.return_generic_index;
                body->shape_index = fn.return_shape_index;
                if (fn.return_generic_index < fn.type_params.len) {
                    body->generic_has_error_constraint =
                        fn.type_params[fn.return_generic_index].has_error_constraint;
                    body->generic_has_eq_constraint =
                        fn.type_params[fn.return_generic_index].has_eq_constraint;
                    body->generic_has_ord_constraint =
                        fn.type_params[fn.return_generic_index].has_ord_constraint;
                    body->generic_protocol_index =
                        fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                            ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                            : 0xffffffffu;
                    body->generic_protocol_count =
                        fn.type_params[fn.return_generic_index].custom_protocol_count;
                    for (u32 cpi = 0; cpi < body->generic_protocol_count; cpi++)
                        body->generic_protocol_indices[cpi] =
                            fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
                }
                if (fn.return_type == HirTypeKind::Variant)
                    body->variant_index = fn.return_variant_index;
                if (fn.return_type == HirTypeKind::Struct)
                    body->struct_index = fn.return_struct_index;
                if (fn.return_type == HirTypeKind::Tuple) {
                    body->tuple_len = fn.return_tuple_len;
                    for (u32 ti = 0; ti < fn.return_tuple_len; ti++) {
                        body->tuple_types[ti] = fn.return_tuple_types[ti];
                        body->tuple_variant_indices[ti] = fn.return_tuple_variant_indices[ti];
                        body->tuple_struct_indices[ti] = fn.return_tuple_struct_indices[ti];
                    }
                }
            }
            auto expected = make_expected_type_expr(fn.return_type,
                                                    fn.return_variant_index,
                                                    fn.return_struct_index,
                                                    fn.return_tuple_len,
                                                    fn.return_tuple_types,
                                                    fn.return_tuple_variant_indices,
                                                    fn.return_tuple_struct_indices,
                                                    fn.return_shape_index);
            expected.generic_index = fn.return_generic_index;
            if (fn.return_generic_index < fn.type_params.len) {
                expected.generic_has_error_constraint =
                    fn.type_params[fn.return_generic_index].has_error_constraint;
                expected.generic_has_eq_constraint =
                    fn.type_params[fn.return_generic_index].has_eq_constraint;
                expected.generic_has_ord_constraint =
                    fn.type_params[fn.return_generic_index].has_ord_constraint;
                expected.generic_protocol_index =
                    fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                        ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                        : 0xffffffffu;
                expected.generic_protocol_count =
                    fn.type_params[fn.return_generic_index].custom_protocol_count;
                for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
                    expected.generic_protocol_indices[cpi] =
                        fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
            }
            if (!same_hir_type_shape(mod, body.value(), expected))
                return frontend_error(FrontendError::UnsupportedSyntax, ast_func.body->span);
            if (body->type == HirTypeKind::Variant &&
                body->variant_index != fn.return_variant_index)
                return frontend_error(FrontendError::UnsupportedSyntax, ast_func.body->span);
        }
        if (fn.return_type != HirTypeKind::Unknown) {
            auto return_shape = intern_hir_type_shape(&mod,
                                                      fn.return_type,
                                                      fn.return_generic_index,
                                                      fn.return_variant_index,
                                                      fn.return_struct_index,
                                                      fn.return_tuple_len,
                                                      fn.return_tuple_types,
                                                      fn.return_tuple_variant_indices,
                                                      fn.return_tuple_struct_indices,
                                                      span);
            if (!return_shape) return core::make_unexpected(return_shape.error());
            fn.return_shape_index = return_shape.value();
        }
        HirLocal all_locals[AstFunctionDecl::kMaxParams + HirRoute::kMaxLocals]{};
        u32 all_local_count = 0;
        for (u32 pi = 0; pi < fn.params.len; pi++) all_locals[all_local_count++] = param_locals[pi];
        for (u32 li = 0; li < scratch.locals.len; li++)
            all_locals[all_local_count++] = scratch.locals[li];
        fn.exprs.len = 0;
        auto normalized =
            normalize_function_expr(body.value(), &fn, all_locals, all_local_count, fn.params.len);
        if (!normalized) return core::make_unexpected(normalized.error());
        fn.body = normalized.value();
        return {};
    };

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Func) continue;
        if (find_function_index(mod, item.func.name) != mod.functions.len)
            return frontend_error(FrontendError::UnsupportedSyntax, item.func.span, item.func.name);
        HirFunction fn{};
        fn.span = item.func.span;
        fn.name = item.func.name;
        for (u32 ti = 0; ti < item.func.type_params.len; ti++) {
            for (u32 seen = 0; seen < fn.type_params.len; seen++) {
                if (fn.type_params[seen].name.eq(item.func.type_params[ti].name))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          item.func.span,
                                          item.func.type_params[ti].name);
            }
            HirFunction::TypeParamDecl type_param{};
            type_param.name = item.func.type_params[ti].name;
            if (item.func.type_params[ti].has_constraint) {
                type_param.has_constraint = true;
                type_param.constraint = item.func.type_params[ti].constraint;
                for (u32 ci = 0; ci < item.func.type_params[ti].constraints.len; ci++) {
                    const auto constraint = item.func.type_params[ti].constraints[ci];
                    Str resolved_constraint = constraint;
                    if (ci < item.func.type_params[ti].constraint_namespaces.len &&
                        item.func.type_params[ti].constraint_namespaces[ci].len != 0) {
                        if (!resolve_import_namespace_type_name(
                                mod,
                                item.func.type_params[ti].constraint_namespaces[ci],
                                constraint,
                                resolved_constraint)) {
                            return frontend_error(
                                FrontendError::UnsupportedSyntax, item.func.span, constraint);
                        }
                    }
                    const auto kind = resolve_protocol_kind(mod, resolved_constraint);
                    if (kind == static_cast<HirProtocolKind>(0xff)) {
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, item.func.span, constraint);
                    }
                    type_param.constraints[ci] = resolved_constraint;
                    type_param.constraint_kinds[ci] = kind;
                    if (kind == HirProtocolKind::Error) type_param.has_error_constraint = true;
                    if (kind == HirProtocolKind::Eq) type_param.has_eq_constraint = true;
                    if (kind == HirProtocolKind::Ord) type_param.has_ord_constraint = true;
                    if (kind == HirProtocolKind::Custom) {
                        const u32 pi = find_protocol_index(mod, resolved_constraint);
                        if (pi >= mod.protocols.len)
                            return frontend_error(
                                FrontendError::UnsupportedSyntax, item.func.span, constraint);
                        type_param.custom_protocol_indices[type_param.custom_protocol_count++] = pi;
                    }
                }
                type_param.constraint_kind = item.func.type_params[ti].constraints.len != 0
                                                 ? type_param.constraint_kinds[0]
                                                 : HirProtocolKind::Custom;
            }
            if (!fn.type_params.push(type_param))
                return frontend_error(FrontendError::TooManyItems, item.func.span);
        }
        if (item.func.has_return_type) {
            const auto& ret_ref = item.func.return_type;
            if (!ret_ref.is_tuple && ret_ref.type_arg_names.len != 0) {
                u32 template_variant_index = 0xffffffffu;
                u32 template_struct_index = 0xffffffffu;
                const auto named_kind = resolve_named_type(
                    mod, ret_ref.name, template_variant_index, template_struct_index);
                const bool is_generic_named =
                    (named_kind == HirTypeKind::Variant &&
                     template_variant_index < mod.variants.len &&
                     mod.variants[template_variant_index].type_params.len ==
                         ret_ref.type_arg_names.len) ||
                    (named_kind == HirTypeKind::Struct && template_struct_index < mod.structs.len &&
                     mod.structs[template_struct_index].type_params.len ==
                         ret_ref.type_arg_names.len);
                if (is_generic_named) {
                    bool has_generic_arg = false;
                    GenericBinding bindings[HirFunction::kMaxTypeParams]{};
                    fn.return_type = named_kind;
                    fn.return_template_variant_index = template_variant_index;
                    fn.return_template_struct_index = template_struct_index;
                    fn.return_type_arg_count = ret_ref.type_arg_names.len;
                    for (u32 ai = 0; ai < ret_ref.type_arg_names.len; ai++) {
                        AstTypeRef arg_ref = get_ast_type_arg_ref(ret_ref, ai);
                        auto arg_type =
                            resolve_func_type_ref(mod,
                                                  arg_ref,
                                                  &fn.type_params,
                                                  &fn.return_type_args[ai].generic_index,
                                                  fn.return_type_args[ai].variant_index,
                                                  fn.return_type_args[ai].struct_index,
                                                  fn.return_type_args[ai].tuple_len,
                                                  fn.return_type_args[ai].tuple_types,
                                                  fn.return_type_args[ai].tuple_variant_indices,
                                                  fn.return_type_args[ai].tuple_struct_indices,
                                                  item.func.span);
                        if (!arg_type) return core::make_unexpected(arg_type.error());
                        fn.return_type_args[ai].type = arg_type.value();
                        auto arg_shape =
                            intern_hir_type_shape(&mod,
                                                  fn.return_type_args[ai].type,
                                                  fn.return_type_args[ai].generic_index,
                                                  fn.return_type_args[ai].variant_index,
                                                  fn.return_type_args[ai].struct_index,
                                                  fn.return_type_args[ai].tuple_len,
                                                  fn.return_type_args[ai].tuple_types,
                                                  fn.return_type_args[ai].tuple_variant_indices,
                                                  fn.return_type_args[ai].tuple_struct_indices,
                                                  item.func.span);
                        if (!arg_shape) return core::make_unexpected(arg_shape.error());
                        fn.return_type_args[ai].shape_index = arg_shape.value();
                        auto filled = fill_bound_binding_from_type_metadata(
                            &bindings[ai],
                            &mod,
                            fn.return_type_args[ai].type,
                            fn.return_type_args[ai].generic_index,
                            fn.return_type_args[ai].variant_index,
                            fn.return_type_args[ai].struct_index,
                            fn.return_type_args[ai].tuple_len,
                            fn.return_type_args[ai].tuple_types,
                            fn.return_type_args[ai].tuple_variant_indices,
                            fn.return_type_args[ai].tuple_struct_indices,
                            fn.return_type_args[ai].shape_index,
                            item.func.span);
                        if (!filled) return core::make_unexpected(filled.error());
                        copy_binding_constraints_from_type_params(&bindings[ai], fn.type_params);
                        if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                    }
                    if (has_generic_arg) {
                        if (named_kind == HirTypeKind::Variant) {
                            auto concrete = instantiate_variant(&mod,
                                                                template_variant_index,
                                                                bindings,
                                                                ret_ref.type_arg_names.len,
                                                                item.func.span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            fn.return_variant_index = concrete.value();
                        } else {
                            auto concrete = instantiate_struct(&mod,
                                                               template_struct_index,
                                                               bindings,
                                                               ret_ref.type_arg_names.len,
                                                               item.func.span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            fn.return_struct_index = concrete.value();
                        }
                    } else {
                        auto ret_type = resolve_func_type_ref(mod,
                                                              ret_ref,
                                                              &fn.type_params,
                                                              &fn.return_generic_index,
                                                              fn.return_variant_index,
                                                              fn.return_struct_index,
                                                              fn.return_tuple_len,
                                                              fn.return_tuple_types,
                                                              fn.return_tuple_variant_indices,
                                                              fn.return_tuple_struct_indices,
                                                              item.func.span);
                        if (!ret_type) return core::make_unexpected(ret_type.error());
                        fn.return_type = ret_type.value();
                    }
                } else {
                    auto ret_type = resolve_func_type_ref(mod,
                                                          ret_ref,
                                                          &fn.type_params,
                                                          &fn.return_generic_index,
                                                          fn.return_variant_index,
                                                          fn.return_struct_index,
                                                          fn.return_tuple_len,
                                                          fn.return_tuple_types,
                                                          fn.return_tuple_variant_indices,
                                                          fn.return_tuple_struct_indices,
                                                          item.func.span);
                    if (!ret_type) return core::make_unexpected(ret_type.error());
                    fn.return_type = ret_type.value();
                }
            } else {
                auto ret_type = resolve_func_type_ref(mod,
                                                      ret_ref,
                                                      &fn.type_params,
                                                      &fn.return_generic_index,
                                                      fn.return_variant_index,
                                                      fn.return_struct_index,
                                                      fn.return_tuple_len,
                                                      fn.return_tuple_types,
                                                      fn.return_tuple_variant_indices,
                                                      fn.return_tuple_struct_indices,
                                                      item.func.span);
                if (!ret_type) return core::make_unexpected(ret_type.error());
                fn.return_type = ret_type.value();
            }
        }
        for (u32 pi = 0; pi < item.func.params.len; pi++) {
            for (u32 seen = 0; seen < fn.params.len; seen++) {
                if (fn.params[seen].name.eq(item.func.params[pi].name))
                    return frontend_error(FrontendError::UnsupportedSyntax,
                                          item.func.span,
                                          item.func.params[pi].name);
            }
            HirFunction::ParamDecl param{};
            param.name = item.func.params[pi].name;
            param.has_underscore_label = item.func.params[pi].has_underscore_label;
            const auto& param_ref = item.func.params[pi].type;
            if (!param_ref.is_tuple && param_ref.type_arg_names.len != 0) {
                u32 template_variant_index = 0xffffffffu;
                u32 template_struct_index = 0xffffffffu;
                const auto named_kind = resolve_named_type(
                    mod, param_ref.name, template_variant_index, template_struct_index);
                const bool is_generic_named =
                    (named_kind == HirTypeKind::Variant &&
                     template_variant_index < mod.variants.len &&
                     mod.variants[template_variant_index].type_params.len ==
                         param_ref.type_arg_names.len) ||
                    (named_kind == HirTypeKind::Struct && template_struct_index < mod.structs.len &&
                     mod.structs[template_struct_index].type_params.len ==
                         param_ref.type_arg_names.len);
                if (is_generic_named) {
                    bool has_generic_arg = false;
                    GenericBinding bindings[HirFunction::kMaxTypeParams]{};
                    param.type = named_kind;
                    param.template_variant_index = template_variant_index;
                    param.template_struct_index = template_struct_index;
                    param.type_arg_count = param_ref.type_arg_names.len;
                    for (u32 ai = 0; ai < param_ref.type_arg_names.len; ai++) {
                        AstTypeRef arg_ref = get_ast_type_arg_ref(param_ref, ai);
                        auto arg_type =
                            resolve_func_type_ref(mod,
                                                  arg_ref,
                                                  &fn.type_params,
                                                  &param.type_args[ai].generic_index,
                                                  param.type_args[ai].variant_index,
                                                  param.type_args[ai].struct_index,
                                                  param.type_args[ai].tuple_len,
                                                  param.type_args[ai].tuple_types,
                                                  param.type_args[ai].tuple_variant_indices,
                                                  param.type_args[ai].tuple_struct_indices,
                                                  item.func.span);
                        if (!arg_type) return core::make_unexpected(arg_type.error());
                        param.type_args[ai].type = arg_type.value();
                        auto arg_shape =
                            intern_hir_type_shape(&mod,
                                                  param.type_args[ai].type,
                                                  param.type_args[ai].generic_index,
                                                  param.type_args[ai].variant_index,
                                                  param.type_args[ai].struct_index,
                                                  param.type_args[ai].tuple_len,
                                                  param.type_args[ai].tuple_types,
                                                  param.type_args[ai].tuple_variant_indices,
                                                  param.type_args[ai].tuple_struct_indices,
                                                  item.func.span);
                        if (!arg_shape) return core::make_unexpected(arg_shape.error());
                        param.type_args[ai].shape_index = arg_shape.value();
                        auto filled = fill_bound_binding_from_type_metadata(
                            &bindings[ai],
                            &mod,
                            param.type_args[ai].type,
                            param.type_args[ai].generic_index,
                            param.type_args[ai].variant_index,
                            param.type_args[ai].struct_index,
                            param.type_args[ai].tuple_len,
                            param.type_args[ai].tuple_types,
                            param.type_args[ai].tuple_variant_indices,
                            param.type_args[ai].tuple_struct_indices,
                            param.type_args[ai].shape_index,
                            item.func.span);
                        if (!filled) return core::make_unexpected(filled.error());
                        copy_binding_constraints_from_type_params(&bindings[ai], fn.type_params);
                        if (arg_type.value() == HirTypeKind::Generic) has_generic_arg = true;
                    }
                    if (has_generic_arg) {
                        if (named_kind == HirTypeKind::Variant) {
                            auto concrete = instantiate_variant(&mod,
                                                                template_variant_index,
                                                                bindings,
                                                                param_ref.type_arg_names.len,
                                                                item.func.span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            param.variant_index = concrete.value();
                        } else {
                            auto concrete = instantiate_struct(&mod,
                                                               template_struct_index,
                                                               bindings,
                                                               param_ref.type_arg_names.len,
                                                               item.func.span);
                            if (!concrete) return core::make_unexpected(concrete.error());
                            param.struct_index = concrete.value();
                        }
                    } else {
                        auto param_type = resolve_func_type_ref(mod,
                                                                param_ref,
                                                                &fn.type_params,
                                                                &param.generic_index,
                                                                param.variant_index,
                                                                param.struct_index,
                                                                param.tuple_len,
                                                                param.tuple_types,
                                                                param.tuple_variant_indices,
                                                                param.tuple_struct_indices,
                                                                item.func.span);
                        if (!param_type) return core::make_unexpected(param_type.error());
                        param.type = param_type.value();
                    }
                } else {
                    auto param_type = resolve_func_type_ref(mod,
                                                            param_ref,
                                                            &fn.type_params,
                                                            &param.generic_index,
                                                            param.variant_index,
                                                            param.struct_index,
                                                            param.tuple_len,
                                                            param.tuple_types,
                                                            param.tuple_variant_indices,
                                                            param.tuple_struct_indices,
                                                            item.func.span);
                    if (!param_type) return core::make_unexpected(param_type.error());
                    param.type = param_type.value();
                }
            } else {
                auto param_type = resolve_func_type_ref(mod,
                                                        param_ref,
                                                        &fn.type_params,
                                                        &param.generic_index,
                                                        param.variant_index,
                                                        param.struct_index,
                                                        param.tuple_len,
                                                        param.tuple_types,
                                                        param.tuple_variant_indices,
                                                        param.tuple_struct_indices,
                                                        item.func.span);
                if (!param_type) return core::make_unexpected(param_type.error());
                param.type = param_type.value();
            }
            if (param.type == HirTypeKind::Generic && param.generic_index < fn.type_params.len)
                param.generic_has_error_constraint =
                    fn.type_params[param.generic_index].has_error_constraint;
            if (param.type == HirTypeKind::Generic && param.generic_index < fn.type_params.len) {
                param.generic_has_eq_constraint =
                    fn.type_params[param.generic_index].has_eq_constraint;
                param.generic_has_ord_constraint =
                    fn.type_params[param.generic_index].has_ord_constraint;
                param.generic_protocol_index =
                    fn.type_params[param.generic_index].custom_protocol_count != 0
                        ? fn.type_params[param.generic_index].custom_protocol_indices[0]
                        : 0xffffffffu;
                param.generic_protocol_count =
                    fn.type_params[param.generic_index].custom_protocol_count;
                for (u32 cpi = 0; cpi < param.generic_protocol_count; cpi++)
                    param.generic_protocol_indices[cpi] =
                        fn.type_params[param.generic_index].custom_protocol_indices[cpi];
            }
            auto param_shape = intern_hir_type_shape(&mod,
                                                     param.type,
                                                     param.generic_index,
                                                     param.variant_index,
                                                     param.struct_index,
                                                     param.tuple_len,
                                                     param.tuple_types,
                                                     param.tuple_variant_indices,
                                                     param.tuple_struct_indices,
                                                     item.func.span);
            if (!param_shape) return core::make_unexpected(param_shape.error());
            param.shape_index = param_shape.value();
            if (!fn.params.push(param))
                return frontend_error(FrontendError::TooManyItems, item.func.span);
        }
        if (fn.return_type != HirTypeKind::Unknown) {
            auto return_shape = intern_hir_type_shape(&mod,
                                                      fn.return_type,
                                                      fn.return_generic_index,
                                                      fn.return_variant_index,
                                                      fn.return_struct_index,
                                                      fn.return_tuple_len,
                                                      fn.return_tuple_types,
                                                      fn.return_tuple_variant_indices,
                                                      fn.return_tuple_struct_indices,
                                                      item.func.span);
            if (!return_shape) return core::make_unexpected(return_shape.error());
            fn.return_shape_index = return_shape.value();
        }
        if (!mod.functions.push(fn))
            return frontend_error(FrontendError::TooManyItems, item.func.span);
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Protocol) continue;
        const u32 protocol_index = find_protocol_index(mod, item.protocol.name);
        if (protocol_index >= mod.protocols.len) continue;
        for (u32 mi = 0; mi < item.protocol.methods.len; mi++) {
            const auto& method_ast = item.protocol.methods[mi];
            if (method_ast.default_body == nullptr) continue;
            auto* method = find_protocol_method_mut(mod.protocols[protocol_index], method_ast.name);
            if (method == nullptr)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.protocol.span, method_ast.name);
            auto mangled = make_protocol_default_function_name(item.protocol.name, method_ast.name);
            if (!mangled) return core::make_unexpected(mangled.error());
            if (find_function_index(mod, mangled.value()) != mod.functions.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.protocol.span, mangled.value());

            AstFunctionDecl synthetic{};
            synthetic.span = item.protocol.span;
            synthetic.name = mangled.value();
            synthetic.has_return_type = method_ast.has_return_type;
            synthetic.return_type = method_ast.return_type;
            AstFunctionDecl::ParamDecl self_param{};
            self_param.name = Str{"self", 4};
            self_param.type.name = Str{"Self", 4};
            if (!synthetic.params.push(self_param))
                return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            for (u32 pi = 0; pi < method_ast.params.len; pi++) {
                AstFunctionDecl::ParamDecl param{};
                param.name = method_ast.params[pi].name;
                param.type = method_ast.params[pi].type;
                if (!synthetic.params.push(param))
                    return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            }
            synthetic.body = method_ast.default_body;

            FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams> extra_type_params;
            HirFunction::TypeParamDecl self_tp{};
            self_tp.name = Str{"Self", 4};
            self_tp.has_constraint = true;
            self_tp.constraint = item.protocol.name;
            self_tp.constraint_kind = HirProtocolKind::Custom;
            if (!extra_type_params.push(self_tp))
                return frontend_error(FrontendError::TooManyItems, item.protocol.span);

            auto fn = declare_function_like(
                synthetic, synthetic.span, mangled.value(), &extra_type_params);
            if (!fn) return core::make_unexpected(fn.error());
            if (!mod.functions.push(fn.value()))
                return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            method->function_index = mod.functions.len - 1;
        }
    }

    auto resolve_impl_target =
        [&](const AstImplDecl& decl,
            HirTypeKind& out_type,
            u32& out_struct_index,
            bool& out_is_generic_template,
            FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams>& out_type_params)
        -> FrontendResult<void> {
        out_type = HirTypeKind::Unknown;
        out_struct_index = 0xffffffffu;
        out_is_generic_template = false;
        out_type_params.len = 0;
        if (!decl.target.is_tuple) {
            const u32 templ_struct_index = find_struct_index(mod, decl.target.name);
            if (templ_struct_index < mod.structs.len &&
                mod.structs[templ_struct_index].type_params.len != 0 &&
                decl.target.type_arg_names.len == mod.structs[templ_struct_index].type_params.len) {
                bool all_are_placeholders = true;
                for (u32 i = 0; i < decl.target.type_arg_names.len; i++) {
                    if (decl.target.type_arg_names[i].len == 0)
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, decl.span, decl.target.name);
                    u32 concrete_variant_index = 0xffffffffu;
                    u32 concrete_struct_index = 0xffffffffu;
                    if (resolve_named_type(mod,
                                           decl.target.type_arg_names[i],
                                           concrete_variant_index,
                                           concrete_struct_index) != HirTypeKind::Unknown) {
                        all_are_placeholders = false;
                        break;
                    }
                    for (u32 j = i + 1; j < decl.target.type_arg_names.len; j++) {
                        if (decl.target.type_arg_names[i].eq(decl.target.type_arg_names[j]))
                            return frontend_error(FrontendError::UnsupportedSyntax,
                                                  decl.span,
                                                  decl.target.type_arg_names[i]);
                    }
                }
                if (!all_are_placeholders) goto resolve_concrete_impl_target;
                out_type = HirTypeKind::Struct;
                out_struct_index = templ_struct_index;
                out_is_generic_template = true;
                for (u32 i = 0; i < decl.target.type_arg_names.len; i++) {
                    HirFunction::TypeParamDecl tp{};
                    tp.name = decl.target.type_arg_names[i];
                    if (!out_type_params.push(tp))
                        return frontend_error(FrontendError::TooManyItems, decl.span);
                }
                return {};
            }
        }
    resolve_concrete_impl_target:
        u32 target_variant_index = 0xffffffffu;
        u32 target_tuple_len = 0;
        HirTypeKind target_tuple_types[kMaxTupleSlots]{};
        u32 target_tuple_variant_indices[kMaxTupleSlots]{};
        u32 target_tuple_struct_indices[kMaxTupleSlots]{};
        auto target_type = resolve_func_type_ref(mod,
                                                 decl.target,
                                                 nullptr,
                                                 nullptr,
                                                 target_variant_index,
                                                 out_struct_index,
                                                 target_tuple_len,
                                                 target_tuple_types,
                                                 target_tuple_variant_indices,
                                                 target_tuple_struct_indices,
                                                 decl.span);
        if (!target_type) return core::make_unexpected(target_type.error());
        out_type = target_type.value();
        return {};
    };

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Impl) continue;
        if (item.impl_decl.protocols.len == 0)
            return frontend_error(FrontendError::UnsupportedSyntax, item.impl_decl.span);
        HirTypeKind target_type = HirTypeKind::Unknown;
        u32 target_struct_index = 0xffffffffu;
        bool target_is_generic_template = false;
        FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams> impl_target_type_params;
        auto resolved_target = resolve_impl_target(item.impl_decl,
                                                   target_type,
                                                   target_struct_index,
                                                   target_is_generic_template,
                                                   impl_target_type_params);
        if (!resolved_target) return core::make_unexpected(resolved_target.error());
        if (!(target_type == HirTypeKind::Bool || target_type == HirTypeKind::I32 ||
              target_type == HirTypeKind::Str || target_type == HirTypeKind::Struct))
            return frontend_error(FrontendError::UnsupportedSyntax, item.impl_decl.span);

        FixedVec<u32, AstImplDecl::kMaxProtocols> protocol_indices;
        FixedVec<HirImpl, AstImplDecl::kMaxProtocols> pending_impls;
        for (u32 pi = 0; pi < item.impl_decl.protocols.len; pi++) {
            const Str proto_name = item.impl_decl.protocols[pi];
            Str resolved_proto_name = proto_name;
            if (pi < item.impl_decl.protocol_namespaces.len &&
                item.impl_decl.protocol_namespaces[pi].len != 0) {
                if (!resolve_import_namespace_type_name(mod,
                                                        item.impl_decl.protocol_namespaces[pi],
                                                        proto_name,
                                                        resolved_proto_name))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, item.impl_decl.span, proto_name);
            }
            const u32 protocol_index = find_protocol_index(mod, resolved_proto_name);
            if (protocol_index >= mod.protocols.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.impl_decl.span, proto_name);
            if (mod.protocols[protocol_index].kind != HirProtocolKind::Custom)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.impl_decl.span, proto_name);
            for (u32 seen = 0; seen < protocol_indices.len; seen++) {
                if (protocol_indices[seen] == protocol_index)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, item.impl_decl.span, proto_name);
            }
            for (u32 ii = 0; ii < mod.impls.len; ii++) {
                if (mod.impls[ii].protocol_index == protocol_index &&
                    impl_targets_overlap(mod,
                                         mod.impls[ii],
                                         target_type,
                                         target_struct_index,
                                         target_is_generic_template))
                    return frontend_error(FrontendError::UnsupportedSyntax, item.impl_decl.span);
            }
            if (!protocol_indices.push(protocol_index))
                return frontend_error(FrontendError::TooManyItems, item.impl_decl.span);
            HirImpl impl{};
            impl.span = item.impl_decl.span;
            impl.protocol_index = protocol_index;
            impl.type = target_type;
            impl.struct_index = target_struct_index;
            impl.is_generic_template = target_is_generic_template;
            if (!pending_impls.push(impl))
                return frontend_error(FrontendError::TooManyItems, item.impl_decl.span);
            for (u32 ci = 0; ci < mod.conformances.len; ci++) {
                const auto& existing = mod.conformances[ci];
                if (existing.protocol_index == protocol_index && existing.type == target_type &&
                    existing.struct_index == target_struct_index &&
                    existing.is_generic_template == target_is_generic_template)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, item.impl_decl.span, proto_name);
            }
            HirConformance conf{};
            conf.span = item.impl_decl.span;
            conf.protocol_index = protocol_index;
            conf.type = target_type;
            conf.struct_index = target_struct_index;
            conf.is_generic_template = target_is_generic_template;
            if (!mod.conformances.push(conf))
                return frontend_error(FrontendError::TooManyItems, item.impl_decl.span);
        }

        for (u32 pi = 0; pi < protocol_indices.len; pi++) {
            const auto& lhs = mod.protocols[protocol_indices[pi]];
            for (u32 pj = pi + 1; pj < protocol_indices.len; pj++) {
                const auto& rhs = mod.protocols[protocol_indices[pj]];
                for (u32 li = 0; li < lhs.methods.len; li++) {
                    for (u32 ri = 0; ri < rhs.methods.len; ri++) {
                        if (lhs.methods[li].name.eq(rhs.methods[ri].name))
                            return frontend_error(FrontendError::UnsupportedSyntax,
                                                  item.impl_decl.span,
                                                  lhs.methods[li].name);
                    }
                }
            }
        }

        for (u32 mi = 0; mi < item.impl_decl.methods.len; mi++) {
            const auto& method_ast = item.impl_decl.methods[mi];
            if (method_ast.type_params.len != 0)
                return frontend_error(FrontendError::UnsupportedSyntax, method_ast.span);
            if (method_ast.params.len == 0)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);

            u32 matched_protocol_slot = 0xffffffffu;
            const HirProtocol::MethodDecl* req = nullptr;
            for (u32 pi = 0; pi < protocol_indices.len; pi++) {
                const auto* candidate =
                    find_protocol_method(mod.protocols[protocol_indices[pi]], method_ast.name);
                if (candidate == nullptr) continue;
                if (matched_protocol_slot != 0xffffffffu)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
                matched_protocol_slot = pi;
                req = candidate;
            }
            if (matched_protocol_slot == 0xffffffffu || req == nullptr)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);

            auto& impl = pending_impls[matched_protocol_slot];
            for (u32 seen = 0; seen < impl.methods.len; seen++) {
                if (impl.methods[seen].name.eq(method_ast.name))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            }

            if (method_ast.params.len != req->params.len + 1)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            for (u32 pi = 0; pi < req->params.len; pi++) {
                u32 method_variant_index = 0xffffffffu;
                u32 method_struct_index = 0xffffffffu;
                u32 method_tuple_len = 0;
                HirTypeKind method_tuple_types[kMaxTupleSlots]{};
                u32 method_tuple_variant_indices[kMaxTupleSlots]{};
                u32 method_tuple_struct_indices[kMaxTupleSlots]{};
                auto method_type = resolve_func_type_ref(mod,
                                                         method_ast.params[pi + 1].type,
                                                         nullptr,
                                                         nullptr,
                                                         method_variant_index,
                                                         method_struct_index,
                                                         method_tuple_len,
                                                         method_tuple_types,
                                                         method_tuple_variant_indices,
                                                         method_tuple_struct_indices,
                                                         method_ast.span);
                if (!method_type) return core::make_unexpected(method_type.error());
                auto method_shape = intern_hir_type_shape(&mod,
                                                          method_type.value(),
                                                          0xffffffffu,
                                                          method_variant_index,
                                                          method_struct_index,
                                                          method_tuple_len,
                                                          method_tuple_types,
                                                          method_tuple_variant_indices,
                                                          method_tuple_struct_indices,
                                                          method_ast.span);
                if (!method_shape) return core::make_unexpected(method_shape.error());
                HirExpr actual{};
                actual.type = method_type.value();
                actual.variant_index = method_variant_index;
                actual.struct_index = method_struct_index;
                actual.shape_index = method_shape.value();
                actual.tuple_len = method_tuple_len;
                for (u32 ti = 0; ti < method_tuple_len; ti++) {
                    actual.tuple_types[ti] = method_tuple_types[ti];
                    actual.tuple_variant_indices[ti] = method_tuple_variant_indices[ti];
                    actual.tuple_struct_indices[ti] = method_tuple_struct_indices[ti];
                }
                HirExpr expected{};
                expected.type = req->params[pi].type;
                expected.generic_index = req->params[pi].generic_index;
                expected.variant_index = req->params[pi].variant_index;
                expected.struct_index = req->params[pi].struct_index;
                expected.shape_index = req->params[pi].shape_index;
                expected.tuple_len = req->params[pi].tuple_len;
                for (u32 ti = 0; ti < req->params[pi].tuple_len; ti++) {
                    expected.tuple_types[ti] = req->params[pi].tuple_types[ti];
                    expected.tuple_variant_indices[ti] = req->params[pi].tuple_variant_indices[ti];
                    expected.tuple_struct_indices[ti] = req->params[pi].tuple_struct_indices[ti];
                }
                if (!same_hir_type_shape(mod, actual, expected))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            }
            if (req->has_return_type != method_ast.has_return_type)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            if (req->has_return_type) {
                u32 method_variant_index = 0xffffffffu;
                u32 method_struct_index = 0xffffffffu;
                u32 method_tuple_len = 0;
                HirTypeKind method_tuple_types[kMaxTupleSlots]{};
                u32 method_tuple_variant_indices[kMaxTupleSlots]{};
                u32 method_tuple_struct_indices[kMaxTupleSlots]{};
                auto method_type = resolve_func_type_ref(mod,
                                                         method_ast.return_type,
                                                         nullptr,
                                                         nullptr,
                                                         method_variant_index,
                                                         method_struct_index,
                                                         method_tuple_len,
                                                         method_tuple_types,
                                                         method_tuple_variant_indices,
                                                         method_tuple_struct_indices,
                                                         method_ast.span);
                if (!method_type) return core::make_unexpected(method_type.error());
                auto method_shape = intern_hir_type_shape(&mod,
                                                          method_type.value(),
                                                          0xffffffffu,
                                                          method_variant_index,
                                                          method_struct_index,
                                                          method_tuple_len,
                                                          method_tuple_types,
                                                          method_tuple_variant_indices,
                                                          method_tuple_struct_indices,
                                                          method_ast.span);
                if (!method_shape) return core::make_unexpected(method_shape.error());
                HirExpr actual{};
                actual.type = method_type.value();
                actual.variant_index = method_variant_index;
                actual.struct_index = method_struct_index;
                actual.shape_index = method_shape.value();
                actual.tuple_len = method_tuple_len;
                for (u32 ti = 0; ti < method_tuple_len; ti++) {
                    actual.tuple_types[ti] = method_tuple_types[ti];
                    actual.tuple_variant_indices[ti] = method_tuple_variant_indices[ti];
                    actual.tuple_struct_indices[ti] = method_tuple_struct_indices[ti];
                }
                HirExpr expected{};
                expected.type = req->return_type;
                expected.generic_index = req->return_generic_index;
                expected.variant_index = req->return_variant_index;
                expected.struct_index = req->return_struct_index;
                expected.shape_index = req->return_shape_index;
                expected.tuple_len = req->return_tuple_len;
                for (u32 ti = 0; ti < req->return_tuple_len; ti++) {
                    expected.tuple_types[ti] = req->return_tuple_types[ti];
                    expected.tuple_variant_indices[ti] = req->return_tuple_variant_indices[ti];
                    expected.tuple_struct_indices[ti] = req->return_tuple_struct_indices[ti];
                }
                if (!same_hir_type_shape(mod, actual, expected))
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            }
            auto type_name = make_impl_target_name(item.impl_decl.target);
            if (!type_name) return core::make_unexpected(type_name.error());
            auto mangled = make_impl_function_name(
                mod.protocols[impl.protocol_index].name, type_name.value(), method_ast.name);
            if (!mangled) return core::make_unexpected(mangled.error());
            if (find_function_index(mod, mangled.value()) != mod.functions.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, mangled.value());
            auto fn = declare_function_like(
                method_ast, method_ast.span, mangled.value(), &impl_target_type_params);
            if (!fn) return core::make_unexpected(fn.error());
            if (fn->params.len == 0 || fn->params[0].type != impl.type)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            if (fn->params[0].type == HirTypeKind::Struct) {
                const auto& recv_param = fn->params[0];
                if (!impl.is_generic_template && recv_param.struct_index != impl.struct_index)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
                if (impl.is_generic_template) {
                    const bool matches_template =
                        recv_param.template_struct_index == impl.struct_index ||
                        (recv_param.struct_index < mod.structs.len &&
                         mod.structs[recv_param.struct_index].template_struct_index ==
                             impl.struct_index);
                    if (!matches_template)
                        return frontend_error(
                            FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
                }
            }
            if (!mod.functions.push(fn.value()))
                return frontend_error(FrontendError::TooManyItems, method_ast.span);
            HirImplMethod impl_method{};
            impl_method.name = method_ast.name;
            impl_method.function_index = mod.functions.len - 1;
            if (!impl.methods.push(impl_method))
                return frontend_error(FrontendError::TooManyItems, method_ast.span);
        }
        for (u32 pi = 0; pi < pending_impls.len; pi++) {
            const auto& proto = mod.protocols[pending_impls[pi].protocol_index];
            for (u32 mi = 0; mi < proto.methods.len; mi++) {
                const auto& req = proto.methods[mi];
                if (req.function_index != 0xffffffffu) continue;
                if (find_impl_method(pending_impls[pi], req.name) != nullptr) continue;
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.impl_decl.span, req.name);
            }
            if (!mod.impls.push(pending_impls[pi]))
                return frontend_error(FrontendError::TooManyItems, item.impl_decl.span);
        }
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Func) continue;
        const u32 fn_index = find_function_index(mod, item.func.name);
        if (fn_index == mod.functions.len)
            return frontend_error(FrontendError::UnsupportedSyntax, item.func.span, item.func.name);
        HirFunction& fn = mod.functions[fn_index];
        HirRoute scratch{};
        FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases> ast_named_error_cases;
        auto ast_collected = collect_named_error_cases_ast(*item.func.body, ast_named_error_cases);
        if (!ast_collected) return core::make_unexpected(ast_collected.error());
        if (ast_named_error_cases.len != 0) {
            HirVariant error_variant{};
            error_variant.span = item.func.span;
            error_variant.name = {"__error_func", 12};
            for (u32 ci = 0; ci < ast_named_error_cases.len; ci++) {
                HirVariant::CaseDecl case_decl{};
                case_decl.name = ast_named_error_cases[ci].name;
                if (!error_variant.cases.push(case_decl))
                    return frontend_error(FrontendError::TooManyItems, item.func.span);
            }
            if (!mod.variants.push(error_variant))
                return frontend_error(FrontendError::TooManyItems, item.func.span);
            scratch.error_variant_index = mod.variants.len - 1;
        }
        HirLocal param_locals[AstFunctionDecl::kMaxParams]{};
        for (u32 pi = 0; pi < fn.params.len; pi++) {
            param_locals[pi].span = item.func.span;
            param_locals[pi].name = fn.params[pi].name;
            param_locals[pi].ref_index = pi;
            param_locals[pi].type = fn.params[pi].type;
            param_locals[pi].generic_index = fn.params[pi].generic_index;
            param_locals[pi].generic_has_error_constraint =
                fn.params[pi].generic_has_error_constraint;
            param_locals[pi].generic_has_eq_constraint = fn.params[pi].generic_has_eq_constraint;
            param_locals[pi].generic_has_ord_constraint = fn.params[pi].generic_has_ord_constraint;
            param_locals[pi].generic_protocol_index = fn.params[pi].generic_protocol_index;
            param_locals[pi].generic_protocol_count = fn.params[pi].generic_protocol_count;
            for (u32 cpi = 0; cpi < param_locals[pi].generic_protocol_count; cpi++)
                param_locals[pi].generic_protocol_indices[cpi] =
                    fn.params[pi].generic_protocol_indices[cpi];
            param_locals[pi].variant_index = fn.params[pi].variant_index;
            param_locals[pi].struct_index = fn.params[pi].struct_index;
            param_locals[pi].shape_index = fn.params[pi].shape_index;
            param_locals[pi].tuple_len = fn.params[pi].tuple_len;
            for (u32 ti = 0; ti < fn.params[pi].tuple_len; ti++) {
                param_locals[pi].tuple_types[ti] = fn.params[pi].tuple_types[ti];
                param_locals[pi].tuple_variant_indices[ti] =
                    fn.params[pi].tuple_variant_indices[ti];
                param_locals[pi].tuple_struct_indices[ti] = fn.params[pi].tuple_struct_indices[ti];
            }
            param_locals[pi].init.kind = HirExprKind::LocalRef;
            param_locals[pi].init.type = fn.params[pi].type;
            param_locals[pi].init.generic_index = fn.params[pi].generic_index;
            param_locals[pi].init.generic_has_error_constraint =
                fn.params[pi].generic_has_error_constraint;
            param_locals[pi].init.generic_has_eq_constraint =
                fn.params[pi].generic_has_eq_constraint;
            param_locals[pi].init.generic_has_ord_constraint =
                fn.params[pi].generic_has_ord_constraint;
            param_locals[pi].init.generic_protocol_index = fn.params[pi].generic_protocol_index;
            param_locals[pi].init.generic_protocol_count = fn.params[pi].generic_protocol_count;
            for (u32 cpi = 0; cpi < param_locals[pi].init.generic_protocol_count; cpi++)
                param_locals[pi].init.generic_protocol_indices[cpi] =
                    fn.params[pi].generic_protocol_indices[cpi];
            param_locals[pi].init.local_index = pi;
            param_locals[pi].init.variant_index = fn.params[pi].variant_index;
            param_locals[pi].init.struct_index = fn.params[pi].struct_index;
            param_locals[pi].init.shape_index = fn.params[pi].shape_index;
            param_locals[pi].init.tuple_len = fn.params[pi].tuple_len;
            for (u32 ti = 0; ti < fn.params[pi].tuple_len; ti++) {
                param_locals[pi].init.tuple_types[ti] = fn.params[pi].tuple_types[ti];
                param_locals[pi].init.tuple_variant_indices[ti] =
                    fn.params[pi].tuple_variant_indices[ti];
                param_locals[pi].init.tuple_struct_indices[ti] =
                    fn.params[pi].tuple_struct_indices[ti];
            }
        }
        auto body = analyze_function_body_stmt(
            *item.func.body, &scratch, mod, param_locals, fn.params.len, nullptr);
        if (!body) return core::make_unexpected(body.error());
        FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases> named_error_cases;
        for (u32 li = 0; li < scratch.locals.len; li++) {
            auto collected = collect_named_error_cases(scratch.locals[li].init, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
        }
        auto collected = collect_named_error_cases(body.value(), named_error_cases);
        if (!collected) return core::make_unexpected(collected.error());
        if (named_error_cases.len != 0 && scratch.error_variant_index == 0xffffffffu) {
            HirVariant error_variant{};
            error_variant.span = item.func.span;
            error_variant.name = {"__error_func", 12};
            for (u32 ci = 0; ci < named_error_cases.len; ci++) {
                HirVariant::CaseDecl case_decl{};
                case_decl.name = named_error_cases[ci].name;
                if (!error_variant.cases.push(case_decl))
                    return frontend_error(FrontendError::TooManyItems, item.func.span);
            }
            if (!mod.variants.push(error_variant))
                return frontend_error(FrontendError::TooManyItems, item.func.span);
            scratch.error_variant_index = mod.variants.len - 1;
        }
        if (named_error_cases.len != 0 && scratch.error_variant_index != 0xffffffffu) {
            const u32 error_variant_index = scratch.error_variant_index;
            for (u32 li = 0; li < scratch.locals.len; li++) {
                patch_named_error_variant(
                    &scratch.locals[li].init, error_variant_index, named_error_cases);
                patch_error_variant_refs(&scratch.locals[li].init, error_variant_index);
                if (scratch.locals[li].may_error &&
                    scratch.locals[li].error_variant_index == 0xffffffffu)
                    scratch.locals[li].error_variant_index = error_variant_index;
            }
            patch_named_error_variant(&body.value(), error_variant_index, named_error_cases);
            patch_error_variant_refs(&body.value(), error_variant_index);
        }
        if (fn.return_type == HirTypeKind::Unknown) {
            if (body->type == HirTypeKind::Unknown)
                return frontend_error(FrontendError::UnsupportedSyntax, item.func.body->span);
            fn.return_type = body->type;
            fn.return_generic_index = body->generic_index;
            if (body->type == HirTypeKind::Variant) fn.return_variant_index = body->variant_index;
            if (body->type == HirTypeKind::Struct) fn.return_struct_index = body->struct_index;
            if (body->type == HirTypeKind::Tuple) {
                fn.return_tuple_len = body->tuple_len;
                for (u32 ti = 0; ti < body->tuple_len; ti++) {
                    fn.return_tuple_types[ti] = body->tuple_types[ti];
                    fn.return_tuple_variant_indices[ti] = body->tuple_variant_indices[ti];
                    fn.return_tuple_struct_indices[ti] = body->tuple_struct_indices[ti];
                }
            }
        } else {
            if (body->type == HirTypeKind::Unknown && (body->may_nil || body->may_error)) {
                body->type = fn.return_type;
                body->generic_index = fn.return_generic_index;
                body->shape_index = fn.return_shape_index;
                if (fn.return_generic_index < fn.type_params.len) {
                    body->generic_has_error_constraint =
                        fn.type_params[fn.return_generic_index].has_error_constraint;
                    body->generic_has_eq_constraint =
                        fn.type_params[fn.return_generic_index].has_eq_constraint;
                    body->generic_has_ord_constraint =
                        fn.type_params[fn.return_generic_index].has_ord_constraint;
                    body->generic_protocol_index =
                        fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                            ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                            : 0xffffffffu;
                    body->generic_protocol_count =
                        fn.type_params[fn.return_generic_index].custom_protocol_count;
                    for (u32 cpi = 0; cpi < body->generic_protocol_count; cpi++)
                        body->generic_protocol_indices[cpi] =
                            fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
                }
                if (fn.return_type == HirTypeKind::Variant)
                    body->variant_index = fn.return_variant_index;
                if (fn.return_type == HirTypeKind::Struct)
                    body->struct_index = fn.return_struct_index;
                if (fn.return_type == HirTypeKind::Tuple) {
                    body->tuple_len = fn.return_tuple_len;
                    for (u32 ti = 0; ti < fn.return_tuple_len; ti++) {
                        body->tuple_types[ti] = fn.return_tuple_types[ti];
                        body->tuple_variant_indices[ti] = fn.return_tuple_variant_indices[ti];
                        body->tuple_struct_indices[ti] = fn.return_tuple_struct_indices[ti];
                    }
                }
            }
            auto expected = make_expected_type_expr(fn.return_type,
                                                    fn.return_variant_index,
                                                    fn.return_struct_index,
                                                    fn.return_tuple_len,
                                                    fn.return_tuple_types,
                                                    fn.return_tuple_variant_indices,
                                                    fn.return_tuple_struct_indices,
                                                    fn.return_shape_index);
            expected.generic_index = fn.return_generic_index;
            if (fn.return_generic_index < fn.type_params.len) {
                expected.generic_has_error_constraint =
                    fn.type_params[fn.return_generic_index].has_error_constraint;
                expected.generic_has_eq_constraint =
                    fn.type_params[fn.return_generic_index].has_eq_constraint;
                expected.generic_has_ord_constraint =
                    fn.type_params[fn.return_generic_index].has_ord_constraint;
                expected.generic_protocol_index =
                    fn.type_params[fn.return_generic_index].custom_protocol_count != 0
                        ? fn.type_params[fn.return_generic_index].custom_protocol_indices[0]
                        : 0xffffffffu;
                expected.generic_protocol_count =
                    fn.type_params[fn.return_generic_index].custom_protocol_count;
                for (u32 cpi = 0; cpi < expected.generic_protocol_count; cpi++)
                    expected.generic_protocol_indices[cpi] =
                        fn.type_params[fn.return_generic_index].custom_protocol_indices[cpi];
            }
            if (!same_hir_type_shape(mod, body.value(), expected))
                return frontend_error(FrontendError::UnsupportedSyntax, item.func.body->span);
            if (body->type == HirTypeKind::Variant &&
                body->variant_index != fn.return_variant_index)
                return frontend_error(FrontendError::UnsupportedSyntax, item.func.body->span);
        }
        HirLocal all_locals[AstFunctionDecl::kMaxParams + HirRoute::kMaxLocals]{};
        u32 all_local_count = 0;
        for (u32 pi = 0; pi < fn.params.len; pi++) all_locals[all_local_count++] = param_locals[pi];
        for (u32 li = 0; li < scratch.locals.len; li++)
            all_locals[all_local_count++] = scratch.locals[li];
        fn.exprs.len = 0;
        auto normalized =
            normalize_function_expr(body.value(), &fn, all_locals, all_local_count, fn.params.len);
        if (!normalized) return core::make_unexpected(normalized.error());
        fn.body = normalized.value();
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Protocol) continue;
        const u32 protocol_index = find_protocol_index(mod, item.protocol.name);
        if (protocol_index >= mod.protocols.len) continue;
        for (u32 mi = 0; mi < item.protocol.methods.len; mi++) {
            const auto& method_ast = item.protocol.methods[mi];
            if (method_ast.default_body == nullptr) continue;
            auto* method = find_protocol_method_mut(mod.protocols[protocol_index], method_ast.name);
            if (method == nullptr || method->function_index >= mod.functions.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, item.protocol.span, method_ast.name);

            AstFunctionDecl synthetic{};
            synthetic.span = item.protocol.span;
            synthetic.name = mod.functions[method->function_index].name;
            synthetic.has_return_type = method_ast.has_return_type;
            synthetic.return_type = method_ast.return_type;
            AstFunctionDecl::ParamDecl self_param{};
            self_param.name = Str{"self", 4};
            self_param.type.name = Str{"Self", 4};
            if (!synthetic.params.push(self_param))
                return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            for (u32 pi = 0; pi < method_ast.params.len; pi++) {
                AstFunctionDecl::ParamDecl param{};
                param.name = method_ast.params[pi].name;
                param.type = method_ast.params[pi].type;
                if (!synthetic.params.push(param))
                    return frontend_error(FrontendError::TooManyItems, item.protocol.span);
            }
            synthetic.body = method_ast.default_body;
            auto body_ok = analyze_function_body_like(
                mod.functions[method->function_index], synthetic, synthetic.span);
            if (!body_ok) return core::make_unexpected(body_ok.error());
            method->return_may_nil = mod.functions[method->function_index].body.may_nil;
            method->return_may_error = mod.functions[method->function_index].body.may_error;
            method->return_error_struct_index =
                mod.functions[method->function_index].body.error_struct_index;
            method->return_error_variant_index =
                mod.functions[method->function_index].body.error_variant_index;
        }
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Impl) continue;
        HirTypeKind target_type = HirTypeKind::Unknown;
        u32 target_struct_index = 0xffffffffu;
        bool target_is_generic_template = false;
        FixedVec<HirFunction::TypeParamDecl, HirFunction::kMaxTypeParams> impl_target_type_params;
        auto resolved_target = resolve_impl_target(item.impl_decl,
                                                   target_type,
                                                   target_struct_index,
                                                   target_is_generic_template,
                                                   impl_target_type_params);
        if (!resolved_target) return core::make_unexpected(resolved_target.error());
        for (u32 mi = 0; mi < item.impl_decl.methods.len; mi++) {
            const auto& method_ast = item.impl_decl.methods[mi];
            u32 matched_protocol_index = 0xffffffffu;
            for (u32 pi = 0; pi < item.impl_decl.protocols.len; pi++) {
                Str resolved_proto_name = item.impl_decl.protocols[pi];
                if (pi < item.impl_decl.protocol_namespaces.len &&
                    item.impl_decl.protocol_namespaces[pi].len != 0) {
                    if (!resolve_import_namespace_type_name(mod,
                                                            item.impl_decl.protocol_namespaces[pi],
                                                            item.impl_decl.protocols[pi],
                                                            resolved_proto_name))
                        return frontend_error(FrontendError::UnsupportedSyntax,
                                              method_ast.span,
                                              item.impl_decl.protocols[pi]);
                }
                const u32 protocol_index = find_protocol_index(mod, resolved_proto_name);
                if (protocol_index >= mod.protocols.len) continue;
                if (find_protocol_method(mod.protocols[protocol_index], method_ast.name) == nullptr)
                    continue;
                if (matched_protocol_index != 0xffffffffu)
                    return frontend_error(
                        FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
                matched_protocol_index = protocol_index;
            }
            if (matched_protocol_index == 0xffffffffu)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            const HirImpl* impl_ptr = nullptr;
            for (u32 ii = 0; ii < mod.impls.len; ii++) {
                if (mod.impls[ii].protocol_index == matched_protocol_index &&
                    impl_matches_exact_target(mod.impls[ii],
                                              target_type,
                                              target_struct_index,
                                              target_is_generic_template)) {
                    impl_ptr = &mod.impls[ii];
                    break;
                }
            }
            if (impl_ptr == nullptr)
                return frontend_error(FrontendError::UnsupportedSyntax, item.impl_decl.span);
            const auto* method = find_impl_method(*impl_ptr, method_ast.name);
            if (method == nullptr || method->function_index >= mod.functions.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, method_ast.span, method_ast.name);
            auto body_ok = analyze_function_body_like(
                mod.functions[method->function_index], method_ast, method_ast.span);
            if (!body_ok) return core::make_unexpected(body_ok.error());
        }
    }

    for (u32 i = 0; i < file.items.len; i++) {
        const auto& item = file.items[i];
        if (item.kind != AstItemKind::Route) continue;

        HirRoute route{};
        route.span = item.route.span;
        route.path = item.route.path;
        route.method = method_char(item.route.method);
        if (route.method == 0)
            return frontend_error(FrontendError::UnsupportedSyntax, item.route.span);

        for (u32 si = 0; si < item.route.statements.len; si++) {
            const auto& stmt = item.route.statements[si];
            if (stmt.kind == AstStmtKind::Let) {
                HirLocal local{};
                local.span = stmt.span;
                local.name = stmt.name;
                local.ref_index = next_local_ref_index(&route, route.locals.data, route.locals.len);
                auto init = analyze_expr(
                    stmt.expr, &route, mod, route.locals.data, route.locals.len, nullptr);
                if (!init) return core::make_unexpected(init.error());
                auto typed = apply_declared_type_to_expr(&init.value(), mod, stmt);
                if (!typed) return core::make_unexpected(typed.error());
                local.type = init->type;
                local.generic_index = init->generic_index;
                local.generic_has_error_constraint = init->generic_has_error_constraint;
                local.generic_has_eq_constraint = init->generic_has_eq_constraint;
                local.generic_has_ord_constraint = init->generic_has_ord_constraint;
                local.generic_protocol_index = init->generic_protocol_index;
                local.generic_protocol_count = init->generic_protocol_count;
                for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                    local.generic_protocol_indices[cpi] = init->generic_protocol_indices[cpi];
                local.may_nil = init->may_nil;
                local.may_error = init->may_error;
                local.variant_index = init->variant_index;
                local.struct_index = init->struct_index;
                local.tuple_len = init->tuple_len;
                for (u32 ti = 0; ti < init->tuple_len; ti++) {
                    local.tuple_types[ti] = init->tuple_types[ti];
                    local.tuple_variant_indices[ti] = init->tuple_variant_indices[ti];
                    local.tuple_struct_indices[ti] = init->tuple_struct_indices[ti];
                }
                local.error_struct_index = init->error_struct_index;
                local.error_variant_index = init->error_variant_index;
                local.shape_index = init->shape_index;
                local.init = init.value();
                if (!route.locals.push(local))
                    return frontend_error(FrontendError::TooManyItems, stmt.span);
                continue;
            }
            if (stmt.kind == AstStmtKind::Guard) {
                if (si + 1 >= item.route.statements.len)
                    return frontend_error(FrontendError::UnsupportedSyntax, stmt.span);
                HirGuard guard{};
                guard.span = stmt.span;
                auto bound = analyze_expr(
                    stmt.expr, &route, mod, route.locals.data, route.locals.len, nullptr);
                if (!bound) return core::make_unexpected(bound.error());
                auto cond =
                    analyze_guard_cond(stmt.expr, &route, mod, route.locals.data, route.locals.len);
                if (!cond) return core::make_unexpected(cond.error());
                guard.cond = cond.value();
                if (stmt.match_arms.len != 0) {
                    if (!bound->may_error) {
                        u32 fallback_arm = 0;
                        for (u32 ai = 0; ai < stmt.match_arms.len; ai++) {
                            if (stmt.match_arms[ai].is_wildcard) {
                                fallback_arm = ai;
                                break;
                            }
                        }
                        auto fail_term = analyze_term(*stmt.match_arms[fallback_arm].stmt, mod);
                        if (!fail_term) return core::make_unexpected(fail_term.error());
                        guard.fail_term = fail_term.value();
                    } else {
                        guard.fail_kind = HirGuard::FailKind::Match;
                        guard.fail_match_expr = bound.value();
                        auto fail_match = analyze_guard_match_arms(stmt.match_arms,
                                                                   bound.value(),
                                                                   &route,
                                                                   mod,
                                                                   route.locals.data,
                                                                   route.locals.len,
                                                                   &mod.guard_match_arms,
                                                                   &guard);
                        if (!fail_match) return core::make_unexpected(fail_match.error());
                    }
                } else {
                    if (stmt.else_stmt->kind == AstStmtKind::ReturnStatus ||
                        stmt.else_stmt->kind == AstStmtKind::ForwardUpstream) {
                        auto fail_term = analyze_term(*stmt.else_stmt, mod);
                        if (!fail_term) return core::make_unexpected(fail_term.error());
                        guard.fail_term = fail_term.value();
                    } else {
                        guard.fail_kind = HirGuard::FailKind::Body;
                        auto fail_body = analyze_guard_fail_body(*stmt.else_stmt,
                                                                 &guard.fail_body,
                                                                 &route,
                                                                 mod,
                                                                 route.locals.data,
                                                                 route.locals.len,
                                                                 nullptr);
                        if (!fail_body) return core::make_unexpected(fail_body.error());
                    }
                }
                if (stmt.bind_value) {
                    if (known_value_state(bound.value(), route.locals.data, route.locals.len, 0) ==
                        KnownValueState::Error)
                        return frontend_error(FrontendError::UnsupportedSyntax, stmt.expr.span);
                    HirLocal local{};
                    local.span = stmt.span;
                    local.name = stmt.name;
                    local.ref_index =
                        next_local_ref_index(&route, route.locals.data, route.locals.len);
                    local.type = bound->type;
                    local.generic_index = bound->generic_index;
                    local.generic_has_error_constraint = bound->generic_has_error_constraint;
                    local.generic_has_eq_constraint = bound->generic_has_eq_constraint;
                    local.generic_has_ord_constraint = bound->generic_has_ord_constraint;
                    local.generic_protocol_index = bound->generic_protocol_index;
                    local.generic_protocol_count = bound->generic_protocol_count;
                    for (u32 cpi = 0; cpi < local.generic_protocol_count; cpi++)
                        local.generic_protocol_indices[cpi] = bound->generic_protocol_indices[cpi];
                    local.may_nil = bound->may_nil;
                    local.may_error = false;
                    local.variant_index = bound->variant_index;
                    local.struct_index = bound->struct_index;
                    local.tuple_len = bound->tuple_len;
                    for (u32 ti = 0; ti < bound->tuple_len; ti++) {
                        local.tuple_types[ti] = bound->tuple_types[ti];
                        local.tuple_variant_indices[ti] = bound->tuple_variant_indices[ti];
                        local.tuple_struct_indices[ti] = bound->tuple_struct_indices[ti];
                    }
                    local.error_struct_index = bound->error_struct_index;
                    local.error_variant_index = 0xffffffffu;
                    local.shape_index = bound->shape_index;
                    auto init = make_guard_bound_init(&route, bound.value(), stmt.span);
                    if (!init) return core::make_unexpected(init.error());
                    local.init = init.value();
                    if (!route.locals.push(local))
                        return frontend_error(FrontendError::TooManyItems, stmt.span);
                }
                if (!route.guards.push(guard))
                    return frontend_error(FrontendError::TooManyItems, stmt.span);
                continue;
            }
            auto control = analyze_control_stmt(stmt, &route, mod, nullptr);
            if (!control) return core::make_unexpected(control.error());
            if (si + 1 != item.route.statements.len)
                return frontend_error(FrontendError::UnsupportedSyntax,
                                      item.route.statements[si + 1].span);
            break;
        }

        if (route.control.kind == HirControlKind::Direct &&
            route.control.direct_term.span.end == 0 && route.control.direct_term.span.start == 0)
            return frontend_error(FrontendError::UnsupportedSyntax, item.route.span);

        FixedVec<RouteNamedErrorCase, HirVariant::kMaxCases> named_error_cases;
        for (u32 li = 0; li < route.locals.len; li++) {
            auto collected = collect_named_error_cases(route.locals[li].init, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
        }
        for (u32 gi = 0; gi < route.guards.len; gi++) {
            auto collected = collect_named_error_cases(route.guards[gi].cond, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
            collected =
                collect_named_error_cases(route.guards[gi].fail_match_expr, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
            for (u32 ai = 0; ai < route.guards[gi].fail_match_count; ai++) {
                collected = collect_named_error_cases(
                    mod.guard_match_arms[route.guards[gi].fail_match_start + ai].pattern,
                    named_error_cases);
                if (!collected) return core::make_unexpected(collected.error());
            }
        }
        if (route.control.kind == HirControlKind::If) {
            auto collected = collect_named_error_cases(route.control.cond, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
        } else if (route.control.kind == HirControlKind::Match) {
            auto collected = collect_named_error_cases(route.control.match_expr, named_error_cases);
            if (!collected) return core::make_unexpected(collected.error());
            for (u32 ai = 0; ai < route.control.match_arms.len; ai++) {
                collected = collect_named_error_cases(route.control.match_arms[ai].pattern,
                                                      named_error_cases);
                if (!collected) return core::make_unexpected(collected.error());
                for (u32 gi = 0; gi < route.control.match_arms[ai].guards.len; gi++) {
                    collected = collect_named_error_cases(
                        route.control.match_arms[ai].guards[gi].cond, named_error_cases);
                    if (!collected) return core::make_unexpected(collected.error());
                    collected = collect_named_error_cases(
                        route.control.match_arms[ai].guards[gi].fail_match_expr, named_error_cases);
                    if (!collected) return core::make_unexpected(collected.error());
                    for (u32 fai = 0;
                         fai < route.control.match_arms[ai].guards[gi].fail_match_count;
                         fai++) {
                        collected = collect_named_error_cases(
                            mod.guard_match_arms
                                [route.control.match_arms[ai].guards[gi].fail_match_start + fai]
                                    .pattern,
                            named_error_cases);
                        if (!collected) return core::make_unexpected(collected.error());
                    }
                }
                collected =
                    collect_named_error_cases(route.control.match_arms[ai].cond, named_error_cases);
                if (!collected) return core::make_unexpected(collected.error());
            }
        }
        if (named_error_cases.len != 0) {
            HirVariant error_variant{};
            error_variant.span = item.route.span;
            error_variant.name = {"__error_route", 13};
            for (u32 ci = 0; ci < named_error_cases.len; ci++) {
                HirVariant::CaseDecl case_decl{};
                case_decl.name = named_error_cases[ci].name;
                if (!error_variant.cases.push(case_decl))
                    return frontend_error(FrontendError::TooManyItems, item.route.span);
            }
            if (!mod.variants.push(error_variant))
                return frontend_error(FrontendError::TooManyItems, item.route.span);
            route.error_variant_index = mod.variants.len - 1;
            for (u32 li = 0; li < route.locals.len; li++) {
                patch_named_error_variant(
                    &route.locals[li].init, route.error_variant_index, named_error_cases);
                patch_error_variant_refs(&route.locals[li].init, route.error_variant_index);
                if (route.locals[li].may_error &&
                    route.locals[li].error_variant_index == 0xffffffffu)
                    route.locals[li].error_variant_index = route.error_variant_index;
            }
            for (u32 gi = 0; gi < route.guards.len; gi++) {
                patch_named_error_variant(
                    &route.guards[gi].cond, route.error_variant_index, named_error_cases);
                patch_error_variant_refs(&route.guards[gi].cond, route.error_variant_index);
                patch_named_error_variant(&route.guards[gi].fail_match_expr,
                                          route.error_variant_index,
                                          named_error_cases);
                patch_error_variant_refs(&route.guards[gi].fail_match_expr,
                                         route.error_variant_index);
                for (u32 ai = 0; ai < route.guards[gi].fail_match_count; ai++) {
                    patch_named_error_variant(
                        &mod.guard_match_arms[route.guards[gi].fail_match_start + ai].pattern,
                        route.error_variant_index,
                        named_error_cases);
                    patch_error_variant_refs(
                        &mod.guard_match_arms[route.guards[gi].fail_match_start + ai].pattern,
                        route.error_variant_index);
                }
            }
            if (route.control.kind == HirControlKind::If) {
                patch_named_error_variant(
                    &route.control.cond, route.error_variant_index, named_error_cases);
                patch_error_variant_refs(&route.control.cond, route.error_variant_index);
            } else if (route.control.kind == HirControlKind::Match) {
                patch_named_error_variant(
                    &route.control.match_expr, route.error_variant_index, named_error_cases);
                patch_error_variant_refs(&route.control.match_expr, route.error_variant_index);
                for (u32 ai = 0; ai < route.control.match_arms.len; ai++) {
                    patch_named_error_variant(&route.control.match_arms[ai].pattern,
                                              route.error_variant_index,
                                              named_error_cases);
                    patch_error_variant_refs(&route.control.match_arms[ai].pattern,
                                             route.error_variant_index);
                    for (u32 gi = 0; gi < route.control.match_arms[ai].guards.len; gi++) {
                        patch_named_error_variant(&route.control.match_arms[ai].guards[gi].cond,
                                                  route.error_variant_index,
                                                  named_error_cases);
                        patch_error_variant_refs(&route.control.match_arms[ai].guards[gi].cond,
                                                 route.error_variant_index);
                        patch_named_error_variant(
                            &route.control.match_arms[ai].guards[gi].fail_match_expr,
                            route.error_variant_index,
                            named_error_cases);
                        patch_error_variant_refs(
                            &route.control.match_arms[ai].guards[gi].fail_match_expr,
                            route.error_variant_index);
                        for (u32 fai = 0;
                             fai < route.control.match_arms[ai].guards[gi].fail_match_count;
                             fai++) {
                            patch_named_error_variant(
                                &mod.guard_match_arms
                                     [route.control.match_arms[ai].guards[gi].fail_match_start +
                                      fai]
                                         .pattern,
                                route.error_variant_index,
                                named_error_cases);
                            patch_error_variant_refs(
                                &mod.guard_match_arms
                                     [route.control.match_arms[ai].guards[gi].fail_match_start +
                                      fai]
                                         .pattern,
                                route.error_variant_index);
                        }
                    }
                    patch_named_error_variant(&route.control.match_arms[ai].cond,
                                              route.error_variant_index,
                                              named_error_cases);
                    patch_error_variant_refs(&route.control.match_arms[ai].cond,
                                             route.error_variant_index);
                }
            }
        }

        // Resolve route decorators (@auth, @requestId, ...) to function indices,
        // validate signatures, then inline each decorator's body into a synthetic
        // local + guard pair. Pre-middleware short-circuit is achieved by guards:
        // each guard checks `local == 0`; on non-zero, fail_term returns the
        // local's runtime value (HirTerminatorSourceKind::LocalRef).
        const u32 first_decorator_guard_index = route.guards.len;
        for (u32 di = 0; di < item.route.decorators.len; di++) {
            const auto& ast_deco = item.route.decorators[di];
            const u32 fn_index = find_function_index(mod, ast_deco.name);
            if (fn_index == mod.functions.len)
                return frontend_error(
                    FrontendError::UnsupportedSyntax, ast_deco.span, ast_deco.name);
            auto sig_check = validate_decorator_signature(mod.functions[fn_index], ast_deco.span);
            if (!sig_check) return core::make_unexpected(sig_check.error());

            // Inline the decorator's body and bind the result to a synthetic local.
            // The implicit `req` parameter (slot 0) is filled with IntConst(0) — a
            // placeholder until the runtime Request type lands. Decorators that need
            // request data should use `req.header(...)` sugar (parser-level construct
            // independent of the param value).
            HirExpr placeholder_req{};
            placeholder_req.kind = HirExprKind::IntLit;
            placeholder_req.type = HirTypeKind::I32;
            placeholder_req.int_value = 0;
            placeholder_req.span = ast_deco.span;
            HirExpr deco_args[1] = {placeholder_req};
            const auto& deco_fn = mod.functions[fn_index];
            auto inlined =
                instantiate_function_expr(deco_fn.body, &route, mod, deco_args, 1u, nullptr, 0u);
            if (!inlined) return core::make_unexpected(inlined.error());

            HirLocal deco_local{};
            deco_local.span = ast_deco.span;
            deco_local.name =
                intern_generated_name(std::string("_deco_") + std::to_string(di) + "_" +
                                      std::string(ast_deco.name.ptr, ast_deco.name.len));
            deco_local.ref_index =
                next_local_ref_index(&route, route.locals.data, route.locals.len);
            deco_local.type = HirTypeKind::I32;
            deco_local.init = inlined.value();
            const u32 deco_ref = deco_local.ref_index;
            if (!route.locals.push(deco_local))
                return frontend_error(FrontendError::TooManyItems, ast_deco.span);

            // guard cond: Eq(LocalRef(deco_ref), IntConst(0)) — true means "pass through"
            HirExpr lhs_local{};
            lhs_local.kind = HirExprKind::LocalRef;
            lhs_local.type = HirTypeKind::I32;
            lhs_local.local_index = deco_ref;
            lhs_local.span = ast_deco.span;
            HirExpr rhs_zero{};
            rhs_zero.kind = HirExprKind::IntLit;
            rhs_zero.type = HirTypeKind::I32;
            rhs_zero.int_value = 0;
            rhs_zero.span = ast_deco.span;
            HirGuard deco_guard{};
            deco_guard.span = ast_deco.span;
            deco_guard.cond.kind = HirExprKind::Eq;
            deco_guard.cond.type = HirTypeKind::Bool;
            deco_guard.cond.span = ast_deco.span;
            // Allocate Eq operands from route's expression pool so they survive HirRoute
            // copy/move (which rebases pointers via rebase_expr).
            if (route.exprs.len + 2 > HirRoute::kMaxExprs)
                return frontend_error(FrontendError::TooManyItems, ast_deco.span);
            route.exprs.push(lhs_local);
            deco_guard.cond.lhs = &route.exprs[route.exprs.len - 1];
            route.exprs.push(rhs_zero);
            deco_guard.cond.rhs = &route.exprs[route.exprs.len - 1];
            // fail_term: ReturnStatus reading from the synthetic local at runtime.
            deco_guard.fail_kind = HirGuard::FailKind::Term;
            deco_guard.fail_term.kind = HirTerminatorKind::ReturnStatus;
            deco_guard.fail_term.source_kind = HirTerminatorSourceKind::LocalRef;
            deco_guard.fail_term.local_ref_index = deco_ref;
            deco_guard.fail_term.span = ast_deco.span;
            if (!route.guards.push(deco_guard))
                return frontend_error(FrontendError::TooManyItems, ast_deco.span);

            HirRoute::DecoratorRef ref{};
            ref.span = ast_deco.span;
            ref.name = ast_deco.name;
            ref.function_index = fn_index;
            if (!route.decorators.push(ref))
                return frontend_error(FrontendError::TooManyItems, ast_deco.span);
        }

        // Decorator guards must run BEFORE any user-written top-level guard so a
        // rejected decorator short-circuits before user logic. We appended them
        // at the end above; rotate so they sit in front while preserving the
        // user guards' relative order. HirGuards are value-copied; their cond
        // expressions point into route.exprs (stable storage), so rotating in
        // place doesn't invalidate any pointers.
        const u32 num_user_guards = first_decorator_guard_index;
        const u32 num_deco_guards = route.guards.len - first_decorator_guard_index;
        if (num_deco_guards > 0 && num_user_guards > 0) {
            HirGuard tmp[HirRoute::kMaxGuards];
            for (u32 i = 0; i < route.guards.len; i++) tmp[i] = route.guards[i];
            for (u32 i = 0; i < num_deco_guards; i++) route.guards[i] = tmp[num_user_guards + i];
            for (u32 i = 0; i < num_user_guards; i++) route.guards[num_deco_guards + i] = tmp[i];
        }

        if (!mod.routes.push(route))
            return frontend_error(FrontendError::TooManyItems, item.route.span);
    }

    if (source_path.len != 0 && !import_stack.empty()) import_stack.pop_back();
    return mod_ptr.release();
}

FrontendResult<HirModule*> analyze_file(const AstFile& file, Str source_path) {
    std::vector<std::string> import_stack;
    return analyze_file_internal(file, source_path, import_stack, nullptr);
}

FrontendResult<HirModule*> analyze_file(const AstFile& file) {
    return analyze_file(file, {});
}

void reset_import_analysis_counter() {
    g_import_analysis_counter = 0;
}

u32 get_import_analysis_counter() {
    return g_import_analysis_counter;
}

}  // namespace rut
