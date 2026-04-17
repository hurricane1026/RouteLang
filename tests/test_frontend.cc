#include "rut/compiler/analyze.h"
#include "rut/compiler/lexer.h"
#include "rut/compiler/lower_rir.h"
#include "rut/compiler/mir_build.h"
#include "rut/compiler/parser.h"
#include "test.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace rut;
static Str lit(const char* s) {
    u32 n = 0;
    while (s[n]) n++;
    return {s, n};
}
static bool block_has_op(const rir::Block& block, rir::Opcode op) {
    for (u32 i = 0; i < block.inst_count; i++) {
        if (block.insts[i].op == op) return true;
    }
    return false;
}
static u32 block_op_count(const rir::Block& block, rir::Opcode op) {
    u32 count = 0;
    for (u32 i = 0; i < block.inst_count; i++) {
        if (block.insts[i].op == op) count++;
    }
    return count;
}
// RAII wrapper for FrontendResult<T*>. The frontend APIs (parse_file,
// analyze_file, build_mir) all .release() a unique_ptr and hand back a raw
// pointer. Tests allocated hundreds of these without ever deleting; before
// this guard the test binaries leaked ~75 MB per test case, pushing the full
// test_frontend suite to ~44 GB resident and tripping CI OOM kills.
template <typename T>
struct HeapFrontendResult {
    FrontendResult<T*> inner;

    HeapFrontendResult() = default;
    HeapFrontendResult(FrontendResult<T*> v) : inner(std::move(v)) {}
    HeapFrontendResult(const HeapFrontendResult&) = delete;
    HeapFrontendResult& operator=(const HeapFrontendResult&) = delete;
    HeapFrontendResult(HeapFrontendResult&& other) noexcept : inner(std::move(other.inner)) {
        other.inner = core::make_unexpected(Diagnostic{});
    }
    HeapFrontendResult& operator=(HeapFrontendResult&& other) noexcept {
        if (this != &other) {
            reset();
            inner = std::move(other.inner);
            other.inner = core::make_unexpected(Diagnostic{});
        }
        return *this;
    }
    ~HeapFrontendResult() { reset(); }

    void reset() {
        if (inner.has_value()) {
            delete inner.value();
            inner = core::make_unexpected(Diagnostic{});
        }
    }

    bool has_value() const { return inner.has_value(); }
    explicit operator bool() const { return static_cast<bool>(inner); }
    T* operator->() { return inner.value(); }
    const T* operator->() const { return inner.value(); }
    T& value() { return *inner.value(); }
    const T& value() const { return *inner.value(); }
    Diagnostic& error() { return inner.error(); }
    const Diagnostic& error() const { return inner.error(); }
};
static HeapFrontendResult<AstFile> parse_file_heap(const LexedTokens& tokens) {
    auto ast = parse_file(tokens);
    if (!ast) return {core::make_unexpected(ast.error())};
    return {ast.value()};
}
static HeapFrontendResult<HirModule> analyze_file_heap(const AstFile& file) {
    auto hir = analyze_file(file);
    if (!hir) return {core::make_unexpected(hir.error())};
    return {hir.value()};
}
static HeapFrontendResult<HirModule> analyze_file_heap_with_path(const AstFile& file,
                                                                 const std::string& source_path) {
    Str path{source_path.c_str(), static_cast<u32>(source_path.size())};
    auto hir = analyze_file(file, path);
    if (!hir) return {core::make_unexpected(hir.error())};
    return {hir.value()};
}
static HeapFrontendResult<MirModule> build_mir_heap(const HirModule& module) {
    auto mir = build_mir(module);
    if (!mir) return {core::make_unexpected(mir.error())};
    return {mir.value()};
}
TEST(frontend, lex_parse_return_route) {
    const char* src = "upstream api\nroute GET \"/users\" { return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    CHECK(lexed->tokens.len > 0);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Upstream));
    CHECK_EQ(static_cast<u8>(ast->items[1].kind), static_cast<u8>(AstItemKind::Route));
    CHECK(ast->items[1].route.path.eq(lit("/users")));
    REQUIRE_EQ(ast->items[1].route.statements.len, 1u);
    CHECK_EQ(ast->items[1].route.statements[0].status_code, 200);
}

TEST(frontend, lex_emits_at_token_for_decorator_prefix) {
    const char* src = "@auth";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    REQUIRE_EQ(lexed->tokens.len, 3u);  // @, auth, EOF
    CHECK_EQ(static_cast<u8>(lexed->tokens[0].type), static_cast<u8>(TokenType::At));
    CHECK_EQ(static_cast<u8>(lexed->tokens[1].type), static_cast<u8>(TokenType::Ident));
    CHECK(lexed->tokens[1].text.eq(lit("auth")));
}

TEST(frontend, lex_recognizes_lowercase_http_methods) {
    const char* src = "get post put delete patch head options";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    REQUIRE_EQ(lexed->tokens.len, 8u);  // 7 methods + EOF
    CHECK_EQ(static_cast<u8>(lexed->tokens[0].type), static_cast<u8>(TokenType::KwGet));
    CHECK_EQ(static_cast<u8>(lexed->tokens[1].type), static_cast<u8>(TokenType::KwPost));
    CHECK_EQ(static_cast<u8>(lexed->tokens[2].type), static_cast<u8>(TokenType::KwPut));
    CHECK_EQ(static_cast<u8>(lexed->tokens[3].type), static_cast<u8>(TokenType::KwDelete));
    CHECK_EQ(static_cast<u8>(lexed->tokens[4].type), static_cast<u8>(TokenType::KwPatch));
    CHECK_EQ(static_cast<u8>(lexed->tokens[5].type), static_cast<u8>(TokenType::KwHead));
    CHECK_EQ(static_cast<u8>(lexed->tokens[6].type), static_cast<u8>(TokenType::KwOptions));
}

TEST(frontend, lex_recognizes_wait_keyword) {
    const char* src = "wait";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    REQUIRE_EQ(lexed->tokens.len, 2u);  // wait, EOF
    CHECK_EQ(static_cast<u8>(lexed->tokens[0].type), static_cast<u8>(TokenType::KwWait));
}

TEST(frontend, parse_route_accepts_wait_statement) {
    const char* src = "route GET \"/sleep\" { wait(1000) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    const auto& route = ast->items[0].route;
    REQUIRE_EQ(route.statements.len, 2u);
    CHECK_EQ(static_cast<u8>(route.statements[0].kind), static_cast<u8>(AstStmtKind::Wait));
    CHECK_EQ(route.statements[0].status_code, 1000);  // ms stored in status_code field
    CHECK_EQ(static_cast<u8>(route.statements[1].kind), static_cast<u8>(AstStmtKind::ReturnStatus));
    CHECK_EQ(route.statements[1].status_code, 200);
}

TEST(frontend, analyze_records_wait_in_hir_route) {
    const char* src = "route GET \"/sleep\" { wait(1000) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes.len, 1u);
    REQUIRE_EQ(hir->routes[0].waits.len, 1u);
    CHECK_EQ(hir->routes[0].waits[0].ms, 1000);
}

TEST(frontend, analyze_records_multiple_waits_in_order) {
    const char* src = "route GET \"/x\" { wait(100) wait(200) wait(300) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].waits.len, 3u);
    CHECK_EQ(hir->routes[0].waits[0].ms, 100);
    CHECK_EQ(hir->routes[0].waits[1].ms, 200);
    CHECK_EQ(hir->routes[0].waits[2].ms, 300);
}

TEST(frontend, analyze_accepts_wait_larger_than_u16_max) {
    // After the u32 payload widening, any non-negative ms value is legal.
    // Carry 100000ms (~100s) through HIR — well past the old 65535 cap.
    const char* src = "route GET \"/x\" { wait(100000) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].waits.len, 1u);
    CHECK_EQ(hir->routes[0].waits[0].ms, 100000);
}

TEST(frontend, rir_function_carries_yield_payload_for_waits) {
    const char* src = "route GET \"/x\" { wait(500) wait(1000) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions.len, 1u);
    REQUIRE_EQ(mir->functions[0].waits.len, 2u);
    CHECK_EQ(mir->functions[0].waits[0].ms, 500);
    CHECK_EQ(mir->functions[0].waits[1].ms, 1000);

    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE(rir.module.func_count >= 1u);
    CHECK_EQ(rir.module.functions[0].yield_count, 2u);
    REQUIRE(rir.module.functions[0].yield_payload != nullptr);
    CHECK_EQ(rir.module.functions[0].yield_payload[0], 500u);
    CHECK_EQ(rir.module.functions[0].yield_payload[1], 1000u);
    rir.destroy();
}

TEST(frontend, parse_route_rejects_wait_without_parens) {
    const char* src = "route GET \"/sleep\" { wait 1000 return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(!ast);
}

TEST(frontend, parse_route_rejects_wait_non_integer_arg) {
    const char* src = "route GET \"/sleep\" { wait(\"1s\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(!ast);
}

TEST(frontend, parse_route_block_single_entry_no_decorators) {
    const char* src = "route { GET \"/users\" { return 200 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Route));
    CHECK(ast->items[0].route.path.eq(lit("/users")));
    CHECK_EQ(ast->items[0].route.decorators.len, 0u);
}

TEST(frontend, parse_route_block_multiple_entries) {
    const char* src =
        "route {\n  GET \"/users\" { return 200 }\n  POST \"/orders\" { return 201 }\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK(ast->items[0].route.path.eq(lit("/users")));
    CHECK(ast->items[1].route.path.eq(lit("/orders")));
}

TEST(frontend, parse_route_block_wildcard_binding_applies_to_all_entries) {
    const char* src =
        "route {\n  @auth \"*\"\n  GET \"/users\" { return 200 }\n  POST \"/orders\" { return 201 "
        "}\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    REQUIRE_EQ(ast->items[0].route.decorators.len, 1u);
    CHECK(ast->items[0].route.decorators[0].name.eq(lit("auth")));
    REQUIRE_EQ(ast->items[1].route.decorators.len, 1u);
    CHECK(ast->items[1].route.decorators[0].name.eq(lit("auth")));
}

TEST(frontend, parse_route_block_prefix_binding_applies_only_to_matching_entries) {
    const char* src =
        "route {\n  @auth \"/admin\"\n  GET \"/admin/users\" { return 200 }\n  GET "
        "\"/public/health\" { return 200 }\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    REQUIRE_EQ(ast->items[0].route.decorators.len, 1u);
    CHECK(ast->items[0].route.decorators[0].name.eq(lit("auth")));
    CHECK_EQ(ast->items[1].route.decorators.len, 0u);
}

TEST(frontend, parse_route_block_entry_decorator_only_attached_to_its_entry) {
    const char* src =
        "route {\n  @logResp\n  GET \"/users\" { return 200 }\n  GET \"/health\" { return 200 "
        "}\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    REQUIRE_EQ(ast->items[0].route.decorators.len, 1u);
    CHECK(ast->items[0].route.decorators[0].name.eq(lit("logResp")));
    CHECK_EQ(ast->items[1].route.decorators.len, 0u);
}

TEST(frontend, parse_route_block_binding_and_entry_decorators_are_merged) {
    const char* src =
        "route {\n  @requestId \"*\"\n  @auth \"/admin\"\n  @maxBody\n  POST \"/admin/upload\" { "
        "return 200 }\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    REQUIRE_EQ(ast->items[0].route.decorators.len, 3u);
    // Order: matching bindings first (in declaration order), then entry-prefix decorators
    CHECK(ast->items[0].route.decorators[0].name.eq(lit("requestId")));
    CHECK(ast->items[0].route.decorators[1].name.eq(lit("auth")));
    CHECK(ast->items[0].route.decorators[2].name.eq(lit("maxBody")));
}

TEST(frontend, parse_route_block_lowercase_methods_are_accepted) {
    const char* src =
        "route {\n  get \"/users\" { return 200 }\n  post \"/orders\" { return 201 }\n}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
}

TEST(frontend, parse_route_block_legacy_form_still_works) {
    const char* src = "route GET \"/users\" { return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    CHECK(ast->items[0].route.path.eq(lit("/users")));
    CHECK_EQ(ast->items[0].route.decorators.len, 0u);
}

TEST(frontend, parse_func_param_accepts_underscore_label) {
    const char* src = "func auth(_ req: i32) -> i32 => 0\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    REQUIRE_EQ(ast->items[0].func.params.len, 1u);
    CHECK(ast->items[0].func.params[0].has_underscore_label);
    CHECK(ast->items[0].func.params[0].name.eq(lit("req")));
}

TEST(frontend, parse_func_param_without_underscore_label) {
    const char* src = "func plain(req: i32) -> i32 => 0\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 1u);
    REQUIRE_EQ(ast->items[0].func.params.len, 1u);
    CHECK(!ast->items[0].func.params[0].has_underscore_label);
}

TEST(frontend, analyze_resolves_route_decorator_to_function_index) {
    const char* src = R"rut(
func auth(_ req: i32) -> i32 => 0
route {
    @auth "*"
    GET "/users" { return 200 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes.len, 1u);
    REQUIRE_EQ(hir->routes[0].decorators.len, 1u);
    CHECK(hir->routes[0].decorators[0].name.eq(lit("auth")));
    CHECK_EQ(hir->routes[0].decorators[0].function_index, 0u);  // auth is the only func
}

TEST(frontend, analyze_rejects_unknown_route_decorator) {
    const char* src = R"rut(
route {
    @doesNotExist "*"
    GET "/users" { return 200 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
    CHECK_EQ(hir.error().code, FrontendError::UnsupportedSyntax);
}

TEST(frontend, analyze_rejects_decorator_function_with_zero_params) {
    const char* src = R"rut(
func auth() -> i32 => 0
route {
    @auth "*"
    GET "/users" { return 200 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
    CHECK_EQ(hir.error().code, FrontendError::UnsupportedSyntax);
}

TEST(frontend, analyze_rejects_decorator_function_missing_underscore_first_param) {
    const char* src = R"rut(
func auth(req: i32) -> i32 => 0
route {
    @auth "*"
    GET "/users" { return 200 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
    CHECK_EQ(hir.error().code, FrontendError::UnsupportedSyntax);
}

TEST(frontend, analyze_rejects_decorator_function_with_non_i32_return_type) {
    const char* src = R"rut(
func auth(_ req: i32) -> bool => true
route {
    @auth "*"
    GET "/users" { return 200 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
    CHECK_EQ(hir.error().code, FrontendError::UnsupportedSyntax);
}

TEST(frontend, parse_file_header_package_decl_is_recorded) {
    const char* src = "package auth\nfunc jwtAuth() -> i32 => 200\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    CHECK(ast->has_package_decl);
    CHECK(ast->package_name.eq(lit("auth")));
    REQUIRE_EQ(ast->items.len, 1u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Func));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->has_package_decl);
    CHECK(hir->package_name.eq(lit("auth")));
}

TEST(frontend, parse_rejects_package_decl_after_top_level_item) {
    const char* src = "func jwtAuth() -> i32 => 200\npackage auth\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(!ast);
    CHECK_EQ(ast.error().code, FrontendError::UnexpectedToken);
}
TEST(frontend, import_relative_file_merges_imported_function_symbols) {
    const std::string dir = "/tmp/rut_import_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        std::fprintf(stderr,
                     "hir error code=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(hir.error().code),
                     static_cast<int>(hir.error().detail.len),
                     hir.error().detail.ptr,
                     hir.error().span.line,
                     hir.error().span.col);
    }
    REQUIRE(hir);
    CHECK(hir->functions.len >= 1u);
}

TEST(frontend, import_relative_file_with_package_decl_merges_imported_function_symbols) {
    const std::string dir = "/tmp/rut_import_packaged_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        std::fprintf(stderr,
                     "hir error code=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(hir.error().code),
                     static_cast<int>(hir.error().detail.len),
                     hir.error().detail.ptr,
                     hir.error().span.line,
                     hir.error().span.col);
    }
    REQUIRE(hir);
    CHECK(hir->functions.len >= 1u);
}

TEST(frontend, import_relative_file_records_imported_package_metadata_in_hir) {
    const std::string dir = "/tmp/rut_import_package_metadata_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].has_package_decl);
    CHECK(hir->imports[0].package_name.eq(lit("auth")));
    CHECK(!hir->imports[0].same_package);
}

TEST(frontend, import_relative_file_in_same_package_marks_hir_import_as_same_package) {
    const std::string dir = "/tmp/rut_import_same_package_metadata_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
package auth
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].has_package_decl);
    CHECK(hir->imports[0].package_name.eq(lit("auth")));
    CHECK(hir->imports[0].same_package);
}

TEST(frontend, import_relative_file_in_different_package_keeps_hir_import_outside_same_package) {
    const std::string dir = "/tmp/rut_import_different_package_metadata_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
package gateway
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].has_package_decl);
    CHECK(hir->imports[0].package_name.eq(lit("auth")));
    CHECK(!hir->imports[0].same_package);
}

TEST(frontend, selective_import_in_same_package_marks_hir_import_as_same_package) {
    const std::string dir = "/tmp/rut_selective_import_same_package_metadata_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
        out << "func basicAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
package auth
import { jwtAuth } from "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].selective);
    CHECK(hir->imports[0].has_package_decl);
    CHECK(hir->imports[0].package_name.eq(lit("auth")));
    CHECK(hir->imports[0].same_package);
}

TEST(frontend, namespace_alias_import_in_same_package_marks_hir_import_as_same_package) {
    const std::string dir = "/tmp/rut_namespace_import_same_package_metadata_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
package auth
import * as authV1 from "auth.rut"
route GET "/users" {
    if authV1.jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].has_namespace_alias);
    CHECK(hir->imports[0].namespace_alias.eq(lit("authV1")));
    CHECK(hir->imports[0].has_package_decl);
    CHECK(hir->imports[0].package_name.eq(lit("auth")));
    CHECK(hir->imports[0].same_package);
}

TEST(frontend, import_namespace_function_call_is_supported_for_file_with_package_decl) {
    const std::string dir = "/tmp/rut_import_namespace_packaged_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" {
    if auth.jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        rut::test::out("import_namespace_default_method_struct_return err=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
}

TEST(frontend, same_package_multiple_files_do_not_create_package_namespace) {
    const std::string dir = "/tmp/rut_import_same_package_no_package_namespace_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/jwt.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/basic.rut", std::ios::binary);
        out << "package auth\nfunc basicAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "jwt.rut"
import "basic.rut"
route GET "/users" {
    if auth.jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
    CHECK_EQ(hir.error().code, FrontendError::UnsupportedSyntax);
}

TEST(frontend, same_package_multiple_files_still_use_file_namespaces) {
    const std::string dir = "/tmp/rut_import_same_package_file_namespaces_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/jwt.rut", std::ios::binary);
        out << "package auth\nfunc jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/basic.rut", std::ios::binary);
        out << "package auth\nfunc basicAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "jwt.rut"
import "basic.rut"
route GET "/users" {
    if jwt.jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        rut::test::out("import_namespace_default_method_struct_return err=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_struct_symbol) {
    const std::string dir = "/tmp/rut_import_struct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import "types.rut"
route GET "/users" {
    let b = Box(value: 200)
    if b.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir)
        std::fprintf(stderr,
                     "import_namespace_default_method_struct_return err=%d line=%u col=%u\n",
                     static_cast<int>(hir.error().code),
                     hir.error().span.line,
                     hir.error().span.col);
    REQUIRE(hir);
}

TEST(frontend, import_relative_file_merges_imported_struct_tuple_of_struct_field_symbol) {
    const std::string dir = "/tmp/rut_import_struct_tuple_of_struct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "struct Item { value: i32 }\n";
        out << "struct Wrap { pair: (Item, i32) }\n";
    }
    const auto src = R"rut(
import "types.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    const auto& wrap = hir->structs[1];
    REQUIRE_EQ(wrap.fields.len, 1u);
    const auto& pair = wrap.fields[0];
    CHECK(pair.type == HirTypeKind::Tuple);
    REQUIRE_EQ(pair.tuple_len, 2u);
    CHECK(pair.tuple_types[0] == HirTypeKind::Struct);
    CHECK(pair.tuple_struct_indices[0] == 0u);
}

TEST(frontend, import_relative_file_merges_imported_struct_tuple_of_variant_field_symbol) {
    const std::string dir = "/tmp/rut_import_struct_tuple_of_variant_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "variant State { ok, err }\n";
        out << "struct Wrap { pair: (State, i32) }\n";
    }
    const auto src = R"rut(
import "types.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 1u);
    const auto& wrap = hir->structs[0];
    REQUIRE_EQ(wrap.fields.len, 1u);
    const auto& pair = wrap.fields[0];
    CHECK(pair.type == HirTypeKind::Tuple);
    REQUIRE_EQ(pair.tuple_len, 2u);
    CHECK(pair.tuple_types[0] == HirTypeKind::Variant);
    CHECK(pair.tuple_variant_indices[0] == 0u);
}

TEST(frontend, import_relative_file_merges_imported_variant_symbol) {
    const std::string dir = "/tmp/rut_import_variant_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "variant AuthState { ok, err }\n";
    }
    const auto src = R"rut(
import "types.rut"
route GET "/users" {
    let state = AuthState.ok
    match state { case .ok: return 200 case _: return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        std::fprintf(stderr,
                     "hir error code=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(hir.error().code),
                     static_cast<int>(hir.error().detail.len),
                     hir.error().detail.ptr,
                     hir.error().span.line,
                     hir.error().span.col);
    }
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_variant_tuple_of_struct_payload_symbol) {
    const std::string dir = "/tmp/rut_import_variant_tuple_of_struct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "variant Result { ok((Box, i32)), err }\n";
    }
    const auto src = R"rut(
import "types.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 1u);
    const auto& ok = hir->variants[0].cases[0];
    CHECK(ok.has_payload);
    CHECK(ok.payload_type == HirTypeKind::Tuple);
    REQUIRE_EQ(ok.payload_tuple_len, 2u);
    CHECK(ok.payload_tuple_types[0] == HirTypeKind::Struct);
    CHECK(ok.payload_tuple_struct_indices[0] == 0u);
}
TEST(frontend, import_relative_file_merges_imported_protocol_symbol) {
    const std::string dir = "/tmp/rut_import_protocol_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    if (!hir) {
        std::fprintf(stderr,
                     "hir error code=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(hir.error().code),
                     static_cast<int>(hir.error().detail.len),
                     hir.error().detail.ptr,
                     hir.error().span.line,
                     hir.error().span.col);
    }
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_impl_symbol) {
    const std::string dir = "/tmp/rut_import_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, import_relative_file_merges_imported_generic_impl_symbol) {
    const std::string dir = "/tmp/rut_import_generic_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {\n";
        out << "    func hash(self: Box<T>) -> i32 => 200\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 123)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, import_relative_file_remaps_imported_concrete_generic_impl_target) {
    const std::string dir = "/tmp/rut_import_concrete_generic_impl_target_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const u32 protocol_index = [&]() {
        for (u32 i = 0; i < hir->protocols.len; i++) {
            if (hir->protocols[i].name.eq(lit("Hashable"))) return i;
        }
        return hir->protocols.len;
    }();
    REQUIRE(protocol_index < hir->protocols.len);
    const HirImpl* imported_impl = nullptr;
    for (u32 i = 0; i < hir->impls.len; i++) {
        const auto& impl = hir->impls[i];
        if (impl.protocol_index != protocol_index || impl.is_generic_template ||
            impl.type != HirTypeKind::Struct)
            continue;
        if (impl.struct_index >= hir->structs.len) continue;
        const auto& st = hir->structs[impl.struct_index];
        if (!st.name.eq(lit("Box")) || st.template_struct_index == 0xffffffffu) continue;
        imported_impl = &impl;
        break;
    }
    REQUIRE(imported_impl != nullptr);
    REQUIRE(imported_impl->struct_index < hir->structs.len);
    const auto& concrete = hir->structs[imported_impl->struct_index];
    REQUIRE(concrete.template_struct_index < hir->structs.len);
    REQUIRE(concrete.instance_type_arg_count == 1);
    CHECK(concrete.instance_type_args[0] == HirTypeKind::I32);
    CHECK(concrete.instance_shape_indices[0] != 0xffffffffu);
    CHECK(hir->type_shapes[concrete.instance_shape_indices[0]].type == HirTypeKind::I32);
}
TEST(frontend, analyze_rejects_local_impl_overlapping_imported_impl_for_same_protocol_and_type) {
    const std::string dir = "/tmp/rut_import_impl_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box impl Hashable {
    func hash(self: Box) -> i32 => 0
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_local_impl_overlapping_imported_concrete_generic_impl) {
    const std::string dir = "/tmp/rut_import_concrete_generic_impl_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<i32> impl Hashable {
    func hash(self: Box<i32>) -> i32 => 0
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}

TEST(frontend, import_relative_file_allows_distinct_local_concrete_generic_impl) {
    const std::string dir = "/tmp/rut_import_concrete_generic_impl_distinct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<str> impl Hashable {
    func hash(self: Box<str>) -> i32 => 200
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: "ok")) == 200 { return 200 } else { return 500 }
}
    )rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, import_relative_file_dispatches_distinct_concrete_generic_impls) {
    const std::string dir = "/tmp/rut_import_concrete_generic_impl_dual_dispatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<str> impl Hashable {
    func hash(self: Box<str>) -> i32 => 200
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 7)) == 7 {
        if run(Box(value: "ok")) == 200 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, analyze_rejects_imported_concrete_generic_impl_for_distinct_local_instance) {
    const std::string dir = "/tmp/rut_import_concrete_generic_impl_distinct_instance_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: "ok")) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}

TEST(frontend, analyze_rejects_local_concrete_impl_overlapping_imported_generic_impl) {
    const std::string dir = "/tmp/rut_import_impl_overlap_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {\n";
        out << "    func hash(self: Box<T>) -> i32 => 1\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<i32> impl Hashable {
    func hash(self: Box<i32>) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}

TEST(frontend, analyze_rejects_local_generic_impl_overlapping_imported_generic_impl) {
    const std::string dir = "/tmp/rut_import_generic_impl_overlap_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {\n";
        out << "    func hash(self: Box<T>) -> i32 => 1\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<U> impl Hashable {
    func hash(self: Box<U>) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}

TEST(frontend, analyze_rejects_local_concrete_impl_overlapping_imported_generic_empty_impl) {
    const std::string dir = "/tmp/rut_import_generic_empty_impl_overlap_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
Box<i32> impl Hashable {
    func hash(self: Box<i32>) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}

TEST(frontend, analyze_rejects_imported_impl_conflict_across_files_for_same_protocol_and_type) {
    const std::string dir = "/tmp/rut_import_impl_imported_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => 1\n";
        out << "}\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => 2\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "a.rut"
import "b.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, import_relative_file_merges_imported_empty_impl_for_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_default_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend,
     import_relative_file_merges_imported_generic_empty_impl_for_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_default_method_dispatch_with_parameter) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_param_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder { func add(x: i32) -> i32 => x }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" { if run(Box(value: 1)) == 201 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend,
     import_relative_file_merges_imported_generic_empty_impl_for_optional_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_optional_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend,
     import_relative_file_merges_imported_generic_empty_impl_for_error_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_error_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_merges_imported_generic_empty_impl_for_tuple_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_tuple_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func second(a: i32, b: i32) -> i32 => b
func run<T: Pairable>(x: T) -> i32 => x.pair() | second(_2, _1)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_tuple_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_tuple_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func second(a: i32, b: i32) -> i32 => b
func run<T: Pairable>(x: T) -> i32 => x.pair() | second(_2, _1)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_tuple_default_method_equality) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_tuple_eq_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Pairable>(x: T) -> (i32, i32) => x.pair()
route GET "/users" { if run(Box(value: 1)) == (200, 500) { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_tuple_default_method_ordering) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_tuple_ord_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Pairable>(x: T) -> (i32, i32) => x.pair()
route GET "/users" { if run(Box(value: 1)) < (200, 600) { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_block_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code()
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_block_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code()
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_block_body_default_method_dispatch_with_parameter) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_block_param_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" { if run(Box(value: 1)) == 201 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_block_body_default_method_dispatch_with_parameter) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_block_param_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" { if run(Box(value: 1)) == 201 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend,
     import_relative_file_merges_imported_generic_empty_impl_for_if_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_if_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code(ok: bool) -> i32 { if ok { 200 } else { 500 } } }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code(true)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_if_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_if_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code(ok: bool) -> i32 { if ok { 200 } else { 500 } } }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code(true)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend,
     lower_to_rir_supports_imported_generic_empty_impl_for_error_default_method_guard_match) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_error_guard_match_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code(ok: bool) -> i32 { if ok { 200 } else { "
               "error(.timeout) } } }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 {
    let failed = x.code(false)
    guard match failed else { case .timeout => 401 case _ => 500 }
    200
}
route GET "/users" { if run(Box(value: 1)) == 401 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, selective_import_relative_file_merges_selected_function_symbol) {
    const std::string dir = "/tmp/rut_selective_import_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
        out << "func basicAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
import { jwtAuth } from "auth.rut"
route GET "/users" { if jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, selective_import_relative_file_merges_selected_protocol_and_struct_with_impl) {
    const std::string dir = "/tmp/rut_selective_import_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import { Hashable, Box } from "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(Box(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, selective_import_relative_file_aliases_selected_function_symbol) {
    const std::string dir = "/tmp/rut_selective_import_alias_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
        out << "func basicAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
import { jwtAuth as authV1 } from "auth.rut"
route GET "/users" { if authV1() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_namespace_function_call_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_function_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" { if auth.jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_namespace_type_ref_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_type_ref_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func read(x: proto.Box) -> i32 => x.value
route GET "/users" { if read(proto.Box(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_import_namespace_type_ref_binding_to_local_same_name_type) {
    const std::string dir = "/tmp/rut_import_namespace_type_ref_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
struct Box { value: str }
func read(x: proto.Box) -> i32 => x.value
route GET "/users" { if read(Box(value: "x")) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}

TEST(frontend, import_namespace_protocol_constraint_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_protocol_constraint_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
proto.Box impl proto.Hashable {
    func hash(self: proto.Box) -> i32 => self.value
}
func run<T: proto.Hashable>(x: T) -> i32 => x.hash()
route GET "/users" { if run(proto.Box(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     analyze_rejects_import_namespace_protocol_constraint_for_local_same_name_type_without_impl) {
    const std::string dir = "/tmp/rut_import_namespace_protocol_constraint_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {}\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box { value: str }
func run<T: proto.Hashable>(x: T) -> i32 => 200
route GET "/users" {
    if run(Box(value: "x")) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_import_namespace_generic_receiver_dispatch_with_local_same_name_protocol) {
    const std::string dir = "/tmp/rut_import_namespace_protocol_dispatch_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 1 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {}\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
protocol Hashable {}
struct Box { value: str }
func run<T: proto.Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: "x")) == 1 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_import_namespace_concrete_generic_receiver_dispatch_with_local_same_name_type) {
    const std::string dir =
        "/tmp/rut_import_namespace_concrete_generic_receiver_dispatch_same_name_type_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 1 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box<T> { value: T }
func run<T: proto.Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: "x")) == 1 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}

TEST(frontend, import_namespace_generic_type_ref_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_generic_type_ref_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result<T> { ok(T), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func pick(x: proto.Result<i32>) -> i32 {
    match x {
    case .ok(v) => v
    case .err => 0
    }
}
route GET "/users" { if pick(proto.Result<i32>.ok(1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_namespace_nested_generic_payload_lowering_path_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_nested_generic_type_arg_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "variant Result<T> { ok(T), err }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
func wrap(x: proto.Result<proto.Box<proto.Result<i32>>>) -> proto.Result<proto.Box<proto.Result<i32>>> => x
route GET "/users" { let state = wrap(proto.Result<proto.Box<proto.Result<i32>>>.ok(proto.Box<proto.Result<i32>>(value: proto.Result<i32>.ok(1)))) match state { case .ok(v): return 200 case .err: return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, import_namespace_nested_generic_payload_shape_is_carrier_ready_in_mir) {
    const std::string dir = "/tmp/rut_import_namespace_nested_generic_payload_shape_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "variant Result<T> { ok(T), err }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
func wrap(x: proto.Result<proto.Box<proto.Result<i32>>>) -> proto.Result<proto.Box<proto.Result<i32>>> => x
route GET "/users" { let state = wrap(proto.Result<proto.Box<proto.Result<i32>>>.ok(proto.Box<proto.Result<i32>>(value: proto.Result<i32>.ok(1)))) match state { case .ok(v): return 200 case .err: return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);

    bool found = false;
    for (u32 vi = 0; vi < mir->variants.len; vi++) {
        const auto& variant = mir->variants[vi];
        if (variant.template_variant_index == 0xffffffffu) continue;
        bool variant_fully_instantiated = variant.instance_type_arg_count != 0;
        for (u32 ai = 0; variant_fully_instantiated && ai < variant.instance_type_arg_count; ai++) {
            if (variant.instance_shape_indices[ai] == 0xffffffffu)
                variant_fully_instantiated = false;
        }
        if (!variant_fully_instantiated) continue;
        if (variant.cases.len == 0) continue;
        const auto& c = variant.cases[0];
        if (!c.has_payload) continue;
        if (c.payload_type != MirTypeKind::Struct) continue;
        if (c.payload_struct_index >= mir->structs.len) continue;
        const auto& payload_struct = mir->structs[c.payload_struct_index];
        if (payload_struct.template_struct_index == 0xffffffffu) continue;
        bool payload_fully_instantiated = payload_struct.instance_type_arg_count != 0;
        for (u32 ai = 0; payload_fully_instantiated && ai < payload_struct.instance_type_arg_count;
             ai++) {
            if (payload_struct.instance_shape_indices[ai] == 0xffffffffu)
                payload_fully_instantiated = false;
        }
        if (!payload_fully_instantiated) continue;
        if (c.payload_shape_index == 0xffffffffu) continue;
        REQUIRE(c.payload_shape_index < mir->type_shapes.len);
        const auto& shape = mir->type_shapes[c.payload_shape_index];
        CHECK(shape.is_concrete);
        CHECK(shape.carrier_ready);
        found = true;
        break;
    }
    CHECK(found);
}

TEST(frontend, generic_variant_instance_payload_shape_survives_refresh_in_mir) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Result<T> { ok(T), err }
func wrap(x: Result<Box<Result<i32>>>) -> Result<Box<Result<i32>>> => x
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);

    bool found = false;
    for (u32 vi = 0; vi < mir->variants.len; vi++) {
        const auto& variant = mir->variants[vi];
        if (variant.template_variant_index == 0xffffffffu) continue;
        if (variant.cases.len == 0) continue;
        const auto& c = variant.cases[0];
        if (!c.has_payload) continue;
        if (c.payload_shape_index == 0xffffffffu) continue;
        REQUIRE(c.payload_shape_index < mir->type_shapes.len);
        const auto& shape = mir->type_shapes[c.payload_shape_index];
        CHECK(shape.is_concrete);
        CHECK(shape.carrier_ready);
        found = true;
        break;
    }
    CHECK(found);
}

TEST(frontend, generic_struct_instance_field_shape_survives_refresh_in_mir) {
    const auto src = R"rut(
struct Box<T> { value: T }
struct Holder<T> { inner: Box<T> }
struct Wrap<U> { holder: Holder<U> }
func wrap(x: Wrap<i32>) -> Wrap<i32> => x
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);

    bool found = false;
    for (u32 si = 0; si < mir->structs.len; si++) {
        const auto& st = mir->structs[si];
        if (st.template_struct_index == 0xffffffffu) continue;
        if (st.fields.len == 0) continue;
        const auto& field = st.fields[0];
        if (field.shape_index == 0xffffffffu) continue;
        REQUIRE(field.shape_index < mir->type_shapes.len);
        const auto& shape = mir->type_shapes[field.shape_index];
        if (!shape.is_concrete) continue;
        if (!shape.carrier_ready) continue;
        found = true;
    }
    CHECK(found);
}

TEST(frontend, generic_function_instantiated_return_shape_is_carrier_ready_in_mir) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: Result<T> }
func unwrap<T>(x: Holder<T>) -> Result<T> => x.state
route GET "/users" {
    let state = unwrap(Holder(state: Result.ok(200)))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions.len, 1u);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    const auto& local = mir->functions[0].locals[0];
    REQUIRE(local.shape_index < mir->type_shapes.len);
    const auto& shape = mir->type_shapes[local.shape_index];
    CHECK(shape.is_concrete);
    CHECK(shape.carrier_ready);
}

TEST(frontend, import_namespace_struct_init_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_struct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { if proto.Box(value: 1).value == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_namespace_variant_constructor_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_variant_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result { ok(i32), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    match proto.Result.ok(200) {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_namespace_payloadless_variant_case_is_supported) {
    const std::string dir = "/tmp/rut_import_namespace_payloadless_variant_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Token { ready, pending }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    match proto.Token.ready {
    case .ready: return 200
    case .pending: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, analyze_rejects_import_namespace_reference_after_selective_import_only) {
    const std::string dir = "/tmp/rut_import_namespace_selective_only_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import { jwtAuth } from "auth.rut"
route GET "/users" { if auth.jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_reference_to_missing_function_member) {
    const std::string dir = "/tmp/rut_import_namespace_missing_member_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
route GET "/users" { if auth.basicAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_conflict_across_files_with_same_stem) {
    const std::string dir = "/tmp/rut_import_namespace_conflict_frontend";
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    {
        std::ofstream out(dir + "/a/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/b/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
import "a/auth.rut"
import "b/auth.rut"
route GET "/users" { if auth.jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, import_namespace_alias_resolves_same_stem_conflict) {
    const std::string dir = "/tmp/rut_import_namespace_alias_conflict_frontend";
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    {
        std::ofstream out(dir + "/a/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/b/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
import * as authA from "a/auth.rut"
import * as authB from "b/auth.rut"
route GET "/users" { if authA.jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_local_function_name) {
    const std::string dir = "/tmp/rut_import_namespace_alias_local_fn_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "auth.rut"
func auth() -> i32 => 200
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_local_protocol_name) {
    const std::string dir = "/tmp/rut_import_namespace_alias_local_protocol_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "auth.rut"
protocol auth {}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_local_using_alias) {
    const std::string dir = "/tmp/rut_import_namespace_alias_local_using_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "auth.rut"
using auth = v1.jwtAuth
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_local_struct_name) {
    const std::string dir = "/tmp/rut_import_namespace_alias_local_struct_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "auth.rut"
struct auth { value: i32 }
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_local_variant_name) {
    const std::string dir = "/tmp/rut_import_namespace_alias_local_variant_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "auth.rut"
variant auth { ok, err }
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_selective_import_alias) {
    const std::string dir = "/tmp/rut_import_namespace_alias_selective_alias_conflict_frontend";
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    {
        std::ofstream out(dir + "/a/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/b/api.rut", std::ios::binary);
        out << "func login() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "a/auth.rut"
import { login as auth } from "b/api.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}

TEST(frontend, analyze_rejects_import_namespace_alias_conflicting_with_imported_function_name) {
    const std::string dir = "/tmp/rut_import_namespace_alias_imported_function_conflict_frontend";
    std::filesystem::create_directories(dir + "/a");
    std::filesystem::create_directories(dir + "/b");
    {
        std::ofstream out(dir + "/a/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    {
        std::ofstream out(dir + "/b/core.rut", std::ios::binary);
        out << "func auth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import * as auth from "a/auth.rut"
import "b/core.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(!hir);
}
TEST(frontend, selective_import_relative_file_aliases_selected_struct_symbol) {
    const std::string dir = "/tmp/rut_selective_import_alias_struct_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import { Box as AuthBox } from "proto.rut"
route GET "/users" { if AuthBox(value: 1).value == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_selective_import_struct_alias_binding_to_local_same_name_type_ref) {
    const std::string dir = "/tmp/rut_selective_import_alias_struct_same_name_type_ref_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import { Box as AuthBox } from "proto.rut"
struct AuthBox { value: str }
func read(x: AuthBox) -> i32 => x.value
route GET "/users" { if read(AuthBox(value: "x")) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, selective_import_relative_file_aliases_selected_protocol_and_struct_with_impl) {
    const std::string dir = "/tmp/rut_selective_import_alias_impl_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as AuthBox } from "proto.rut"
func run<T: Digestible>(x: T) -> i32 => x.hash()
route GET "/users" { if run(AuthBox(value: 1)) == 1 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_selective_import_alias_combined_same_name_type_and_protocol_drift) {
    const std::string dir = "/tmp/rut_selective_import_alias_combined_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as AuthBox } from "proto.rut"
protocol Digestible { func hash() -> i32 => 200 }
struct AuthBox { value: str }
AuthBox impl Digestible {}
func run<T: Digestible>(x: T) -> i32 => x.hash()
route GET "/users" { if run(AuthBox(value: "x")) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_selective_import_struct_alias_impl_target_with_local_same_name_receiver_type) {
    const std::string dir = "/tmp/rut_selective_import_alias_struct_same_name_impl_target_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as AuthBox } from "proto.rut"
struct AuthBox { value: str }
AuthBox impl Digestible {
    func hash(self: AuthBox) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_selective_import_protocol_alias_constraint_for_local_same_name_type_without_impl) {
    const std::string dir = "/tmp/rut_selective_import_alias_protocol_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {}\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as ImportedBox } from "proto.rut"
protocol Digestible {}
struct Box { value: str }
func run<T: Digestible>(x: T) -> i32 => 200
route GET "/users" {
    if run(Box(value: "x")) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_selective_import_concrete_generic_receiver_dispatch_with_local_same_name_type) {
    const std::string dir =
        "/tmp/"
        "rut_selective_import_alias_concrete_generic_receiver_dispatch_same_name_type_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 1 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<i32> impl Hashable {\n";
        out << "    func hash(self: Box<i32>) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as AuthBox } from "proto.rut"
struct AuthBox<T> { value: T }
func run<T: Digestible>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(AuthBox(value: "x")) == 1 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_selective_import_protocol_alias_impl_binding_to_local_same_name_protocol) {
    const std::string dir = "/tmp/rut_selective_import_alias_protocol_impl_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as AuthBox } from "proto.rut"
protocol Digestible {}
AuthBox impl Digestible {}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_selective_import_protocol_alias_empty_impl_binding_to_local_same_name_protocol) {
    const std::string dir =
        "/tmp/rut_selective_import_alias_protocol_empty_impl_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Producer { func make() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import { Producer as Buildable, Box as ImportedBox } from "proto.rut"
protocol Buildable {}
struct Box { value: str }
ImportedBox impl Buildable {}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_selective_import_impl_method_with_mismatched_concrete_generic_return_type) {
    const std::string dir =
        "/tmp/rut_selective_import_alias_protocol_concrete_generic_return_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "protocol Producer { func make() -> Box<i32> }\n";
    }
    const auto src = R"rut(
import { Producer as Buildable, Box as AuthBox } from "proto.rut"
struct Holder { value: i32 }
struct AuthBox<T> { value: T }
Holder impl Buildable {
    func make(self: Holder) -> AuthBox<str> => AuthBox(value: "x")
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_selective_import_impl_method_with_mismatched_concrete_generic_parameter_type) {
    const std::string dir =
        "/tmp/rut_selective_import_alias_protocol_concrete_generic_parameter_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "protocol Consumer { func take(x: Box<i32>) -> i32 }\n";
    }
    const auto src = R"rut(
import { Consumer as Takeable, Box as AuthBox } from "proto.rut"
struct Holder { value: i32 }
struct AuthBox<T> { value: T }
Holder impl Takeable {
    func take(self: Holder, x: AuthBox<str>) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_selective_import_alias_leaking_original_function_name) {
    const std::string dir = "/tmp/rut_selective_import_alias_hidden_original_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import { jwtAuth as authV1 } from "auth.rut"
route GET "/users" { if jwtAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_selective_import_reference_to_non_selected_function) {
    const std::string dir = "/tmp/rut_selective_import_hidden_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
        out << "func basicAuth() -> i32 => 500\n";
    }
    const auto src = R"rut(
import { jwtAuth } from "auth.rut"
route GET "/users" { if basicAuth() == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_selective_import_of_missing_symbol) {
    const std::string dir = "/tmp/rut_selective_import_missing_symbol_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import { basicAuth } from "auth.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_cyclic_relative_imports) {
    const std::string dir = "/tmp/rut_import_cycle_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "import \"b.rut\"\nfunc fa() -> i32 => 1\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "import \"a.rut\"\nfunc fb() -> i32 => 2\n";
    }
    const auto src = R"rut(
import "a.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_imported_function_name_conflict_across_files) {
    const std::string dir = "/tmp/rut_import_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 1\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 2\n";
    }
    const auto src = R"rut(
import "a.rut"
import "b.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_imported_struct_name_conflict_across_files) {
    const std::string dir = "/tmp/rut_import_struct_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "struct Box { code: i32 }\n";
    }
    const auto src = R"rut(
import "a.rut"
import "b.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_imported_variant_name_conflict_across_files) {
    const std::string dir = "/tmp/rut_import_variant_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "variant AuthState { ok }\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "variant AuthState { err }\n";
    }
    const auto src = R"rut(
import "a.rut"
import "b.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_imported_protocol_name_conflict_across_files) {
    const std::string dir = "/tmp/rut_import_protocol_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/a.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
    }
    {
        std::ofstream out(dir + "/b.rut", std::ios::binary);
        out << "protocol Hashable { func code() -> i32 }\n";
    }
    const auto src = R"rut(
import "a.rut"
import "b.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_imported_struct_name_conflict_with_local_declaration) {
    const std::string dir = "/tmp/rut_import_local_struct_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/types.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import "types.rut"
struct Box { code: i32 }
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    CHECK(!hir);
}
TEST(frontend, import_declaration_is_recorded_in_hir) {
    const auto src = R"rut(
import "middleware/auth.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Import));
    CHECK(ast->items[0].import_decl.path.eq(lit("middleware/auth.rut")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
    CHECK(hir->imports[0].path.eq(lit("middleware/auth.rut")));
}
TEST(frontend, duplicate_import_is_deduplicated_in_hir) {
    const auto src = R"rut(
import "middleware/auth.rut"
import "middleware/auth.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->imports.len, 1u);
}
TEST(frontend, duplicate_relative_import_does_not_reanalyze_same_file) {
    const std::string dir = "/tmp/rut_import_dedup_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/auth.rut", std::ios::binary);
        out << "func jwtAuth() -> i32 => 200\n";
    }
    const auto src = R"rut(
import "auth.rut"
import "auth.rut"
route GET "/users" {
    if jwtAuth() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    reset_import_analysis_counter();
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    CHECK_EQ(get_import_analysis_counter(), 1u);
}
TEST(frontend, using_alias_declaration_is_recorded_in_hir) {
    const auto src = R"rut(
using authV1 = v1.jwtAuth
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Using));
    CHECK(ast->items[0].using_decl.name.eq(lit("authV1")));
    REQUIRE_EQ(ast->items[0].using_decl.target_parts.len, 2u);
    CHECK(ast->items[0].using_decl.target_parts[0].eq(lit("v1")));
    CHECK(ast->items[0].using_decl.target_parts[1].eq(lit("jwtAuth")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->aliases.len, 1u);
    CHECK(hir->aliases[0].name.eq(lit("authV1")));
    REQUIRE_EQ(hir->aliases[0].target_parts.len, 2u);
    CHECK(hir->aliases[0].target_parts[0].eq(lit("v1")));
    CHECK(hir->aliases[0].target_parts[1].eq(lit("jwtAuth")));
}
TEST(frontend, analyze_resolves_using_alias_in_function_call) {
    const auto src = R"rut(
using authV1 = v1.jwtAuth
func jwtAuth() -> i32 => 200
route GET "/users" {
    if authV1() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->aliases.len, 1u);
    REQUIRE_EQ(hir->functions.len, 1u);
}
TEST(frontend, analyze_rejects_using_alias_to_unknown_function_symbol_on_call) {
    const auto src = R"rut(
using authV1 = v1.jwtAuth
route GET "/users" {
    if authV1() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_duplicate_using_alias_name) {
    const auto src = R"rut(
using authV1 = v1.jwtAuth
using authV1 = v2.jwtAuth
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, parse_rejects_old_using_as_protocol_syntax) {
    const auto src = R"rut(
using Box as Hashable
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE_FALSE(ast);
    CHECK(ast.error().code == FrontendError::UnexpectedToken ||
          ast.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, variant_match_lowers_to_tag_compare) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "route GET \"/users\" { let state = AuthState.timeout match state { case .timeout: return "
        "200 case _: return 403 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Variant));
    REQUIRE_EQ(ast->items[0].variant.cases.len, 2u);
    CHECK(ast->items[0].variant.name.eq(lit("AuthState")));
    CHECK(ast->items[0].variant.cases[0].name.eq(lit("timeout")));
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
        REQUIRE(hir);
    }
    REQUIRE_EQ(hir->variants.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::Variant));
    CHECK_EQ(hir->routes[0].locals[0].variant_index, 0u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::VariantCase));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].pattern.kind),
             static_cast<u8>(HirExprKind::VariantCase));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[0].type),
             static_cast<u8>(MirTypeKind::Variant));
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[0].init.kind),
             static_cast<u8>(MirValueKind::VariantCase));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructField));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::CmpEq));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::Br));
    rir.destroy();
}
TEST(frontend, variant_match_all_cases_without_wildcard_is_exhaustive) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "route GET \"/users\" { let state = AuthState.timeout match state { case .timeout: return "
        "200 case .forbidden: return 403 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("plain struct analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    if (!lowered) {
        std::fprintf(stderr,
                     "imported_optional_default lowered err=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(lowered.error().code),
                     static_cast<int>(lowered.error().detail.len),
                     lowered.error().detail.ptr,
                     lowered.error().span.line,
                     lowered.error().span.col);
    }
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_single_payload_case_parses_and_lowers) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): return 200 "
        "case .err: return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    REQUIRE_EQ(ast->items[0].variant.cases.len, 2u);
    CHECK(ast->items[0].variant.cases[0].name.eq(lit("ok")));
    CHECK(ast->items[0].variant.cases[0].has_payload);
    CHECK(!ast->items[0].variant.cases[0].payload_type.is_tuple);
    CHECK(ast->items[0].variant.cases[0].payload_type.name.eq(lit("i32")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->variants.len, 1u);
    REQUIRE_EQ(hir->variants[0].cases.len, 2u);
    CHECK(hir->variants[0].cases[0].has_payload);
    CHECK_EQ(static_cast<u8>(hir->variants[0].cases[0].payload_type),
             static_cast<u8>(HirTypeKind::I32));
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::VariantCase));
    CHECK_EQ(hir->routes[0].locals[0].init.case_index, 0u);
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].pattern.kind),
             static_cast<u8>(HirExprKind::VariantCase));
    CHECK_EQ(hir->routes[0].control.match_arms[0].pattern.case_index, 0u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[0].init.kind),
             static_cast<u8>(MirValueKind::VariantCase));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_payload_binding_flows_into_match_arm_if) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): if x == 200 "
        "{ return 200 } else { return 500 } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    CHECK(hir->routes[0].control.match_arms[0].bind_payload);
    CHECK(hir->routes[0].control.match_arms[0].bind_name.eq(lit("x")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].body_kind),
             static_cast<u8>(HirMatchArm::BodyKind::If));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].cond.kind),
             static_cast<u8>(HirExprKind::Eq));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    if (!lowered) {
        std::fprintf(stderr,
                     "imported_error_default lowered err=%d detail=%.*s span=(%u,%u)\n",
                     static_cast<int>(lowered.error().code),
                     static_cast<int>(lowered.error().detail.len),
                     lowered.error().detail.ptr,
                     lowered.error().span.line,
                     lowered.error().span.col);
    }
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_payload_binding_flows_into_match_arm_block) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): { let y = x "
        "if y == 200 { return 200 } else { return 500 } } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
        REQUIRE(hir);
    }
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[1].name.eq(lit("y")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].body_kind),
             static_cast<u8>(HirMatchArm::BodyKind::If));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_payload_binding_flows_into_match_arm_block_with_guard) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): { let failed "
        "= error(7) guard failed else { return 401 } if x == 200 { return 200 } else { return 500 "
        "} } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
        REQUIRE(hir);
    }
    REQUIRE_EQ(hir->routes[0].control.match_arms[0].guards.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].guards[0].cond.kind),
             static_cast<u8>(HirExprKind::BoolLit));
    CHECK_EQ(hir->routes[0].control.match_arms[0].guards[0].cond.bool_value, false);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_payload_binding_flows_into_match_arm_block_with_guard_match) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): { let failed "
        "= error(.timeout) guard match failed else { case .timeout: return 401 case _: return 402 "
        "} if x == 200 { return 200 } else { return 500 } } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].control.match_arms[0].guards.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].guards[0].fail_kind),
             static_cast<u8>(HirGuard::FailKind::Match));
    REQUIRE_EQ(hir->routes[0].control.match_arms[0].guards[0].fail_match_count, 2u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_mixed_payload_cases_lower) {
    const char* src =
        "variant Mixed { count(i32), ready(bool), label(str), none }\n"
        "route GET \"/users\" { let state = Mixed.ready(true) match state { case .count(x): return "
        "200 case .ready(flag): if flag == true { return 201 } else { return 202 } case "
        ".label(name): return 203 case .none: return 204 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->variants.len, 1u);
    REQUIRE_EQ(hir->variants[0].cases.len, 4u);
    CHECK_EQ(static_cast<u8>(hir->variants[0].cases[0].payload_type),
             static_cast<u8>(HirTypeKind::I32));
    CHECK_EQ(static_cast<u8>(hir->variants[0].cases[1].payload_type),
             static_cast<u8>(HirTypeKind::Bool));
    CHECK_EQ(static_cast<u8>(hir->variants[0].cases[2].payload_type),
             static_cast<u8>(HirTypeKind::Str));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, variant_tuple_payload_binding_flows_into_pipe_multi_slot) {
    const char* src =
        "variant Result { ok((i32, i32)), err }\n"
        "func second(a: i32, b: i32) -> i32 => b\n"
        "route GET \"/users\" { let state = Result.ok((200, 500)) match state { case .ok(pair): { "
        "let code = pair | second(_2, _1) if code == 200 { return 200 } else { return 500 } } case "
        ".err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].variant.cases.len, 2u);
    CHECK(ast->items[0].variant.cases[0].has_payload);
    CHECK(ast->items[0].variant.cases[0].payload_type.is_tuple);
    REQUIRE_EQ(ast->items[0].variant.cases[0].payload_type.tuple_elem_names.len, 2u);
    CHECK(ast->items[0].variant.cases[0].payload_type.tuple_elem_names[0].eq(lit("i32")));
    CHECK(ast->items[0].variant.cases[0].payload_type.tuple_elem_names[1].eq(lit("i32")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(hir->variants[0].cases[0].payload_type, HirTypeKind::Tuple);
    CHECK_EQ(hir->variants[0].cases[0].payload_tuple_len, 2u);
    CHECK_EQ(hir->routes[0].control.match_arms[0].bind_type, HirTypeKind::Tuple);
    CHECK_EQ(hir->routes[0].control.match_arms[0].bind_tuple_len, 2u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, route_match_struct_payload_binding_preserves_shape) {
    const char* src =
        "struct Box { value: i32 }\n"
        "variant Result { ok(Box), err }\n"
        "func boxCode(x: Box) -> i32 => x.value\n"
        "route GET \"/users\" { let state = Result.ok(Box(value: 200)) match state { case "
        ".ok(box): { let code = box | boxCode(_) if code == 200 { return 200 } else { return 500 } "
        "} case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    CHECK(hir->routes[0].control.match_arms[0].bind_payload);
    CHECK_EQ(hir->routes[0].control.match_arms[0].bind_type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].control.match_arms[0].bind_struct_index, 0u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].body_kind),
             static_cast<u8>(HirMatchArm::BodyKind::If));
}

TEST(frontend, route_match_nested_struct_payload_projection) {
    const char* src =
        "struct Box { value: i32 }\n"
        "struct Outer { inner: Box }\n"
        "variant Result { ok(Outer), err }\n"
        "route GET \"/users\" { let state = Result.ok(Outer(inner: Box(value: 200))) match state { "
        "case .ok(v): { let code = v.inner.value if code == 200 { return 200 } else { return 500 } "
        "} case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::I32);
}

TEST(frontend, import_namespace_route_match_nested_struct_payload_projection) {
    const std::string dir = "/tmp/rut_import_namespace_match_nested_struct_payload_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "struct Outer { inner: Box }\n";
        out << "variant Result { ok(Outer), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let state = proto.Result.ok(proto.Outer(inner: proto.Box(value: 200)))
    match state {
    case .ok(v): {
        let code = v.inner.value
        if code == 200 { return 200 } else { return 500 }
    }
    case .err: return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::I32);
}

TEST(frontend, import_namespace_match_payload_is_struct_and_carrier_ready_in_mir) {
    const std::string dir = "/tmp/rut_import_namespace_match_payload_mir_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "struct Outer { inner: Box }\n";
        out << "variant Result { ok(Outer), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let state = proto.Result.ok(proto.Outer(inner: proto.Box(value: 200)))
    match state {
    case .ok(v): {
        let code = v.inner.value
        if code == 200 { return 200 } else { return 500 }
    }
    case .err: return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);

    bool found = false;
    for (u32 fi = 0; fi < mir->functions.len; fi++) {
        const auto& fn = mir->functions[fi];
        for (u32 vi = 0; vi < fn.values.len; vi++) {
            const auto& value = fn.values[vi];
            if (value.kind != MirValueKind::MatchPayload) continue;
            CHECK_EQ(value.type, MirTypeKind::Struct);
            CHECK(value.struct_index < mir->structs.len);
            CHECK(value.shape_index != 0xffffffffu);
            REQUIRE(value.shape_index < mir->type_shapes.len);
            const auto& shape = mir->type_shapes[value.shape_index];
            CHECK_EQ(shape.type, MirTypeKind::Struct);
            CHECK(shape.struct_index < mir->structs.len);
            CHECK(shape.is_concrete);
            CHECK(shape.carrier_ready);
            found = true;
        }
    }
    CHECK(found);
}

TEST(frontend, import_relative_file_preserves_imported_function_signature_shape_indices) {
    const std::string dir = "/tmp/rut_import_function_signature_shape_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result<T> { ok(T), err }\n";
        out << "struct Holder<T> { state: Result<T> }\n";
        out << "func unwrap(x: Holder<i32>) -> Result<i32> => x.state\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const HirFunction* imported_fn = nullptr;
    for (u32 i = 0; i < hir->functions.len; i++) {
        if (hir->functions[i].name.eq(lit("unwrap"))) {
            imported_fn = &hir->functions[i];
            break;
        }
    }
    REQUIRE(imported_fn != nullptr);
    REQUIRE_EQ(imported_fn->params.len, 1u);
    CHECK_EQ(imported_fn->params[0].type, HirTypeKind::Struct);
    CHECK(imported_fn->params[0].shape_index != 0xffffffffu);
    CHECK(imported_fn->params[0].type_args[0].shape_index != 0xffffffffu);
    CHECK_EQ(imported_fn->return_type, HirTypeKind::Variant);
    CHECK(imported_fn->return_shape_index != 0xffffffffu);
    CHECK(imported_fn->return_type_args[0].shape_index != 0xffffffffu);
}

TEST(frontend, import_relative_file_preserves_imported_function_body_shape_indices_in_hir) {
    const std::string dir = "/tmp/rut_import_function_body_shape_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "variant Result { ok, err }\n";
        out << "func makeBox(ok: bool) -> Box { if ok { Box(value: 200) } else { Box(value: 500) } "
               "}\n";
        out << "func maybe(ok: bool) { if ok { 200 } else { nil } }\n";
        out << "func pickOr(ok: bool) -> i32 => or(maybe(ok), 500)\n";
        out << "func pickMatch(x: Result) -> i32 {\n";
        out << "    match x {\n";
        out << "        case .ok => 200\n";
        out << "        case .err => 500\n";
        out << "    }\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const HirFunction* make_box = nullptr;
    const HirFunction* pick_or = nullptr;
    const HirFunction* pick_match = nullptr;
    for (u32 i = 0; i < hir->functions.len; i++) {
        if (hir->functions[i].name.eq(lit("makeBox"))) make_box = &hir->functions[i];
        if (hir->functions[i].name.eq(lit("pickOr"))) pick_or = &hir->functions[i];
        if (hir->functions[i].name.eq(lit("pickMatch"))) pick_match = &hir->functions[i];
    }
    REQUIRE(make_box != nullptr);
    REQUIRE(pick_or != nullptr);
    REQUIRE(pick_match != nullptr);

    CHECK_EQ(static_cast<u8>(make_box->body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK(make_box->body.shape_index != 0xffffffffu);
    REQUIRE(make_box->body.shape_index < hir->type_shapes.len);
    CHECK_EQ(hir->type_shapes[make_box->body.shape_index].type, HirTypeKind::Struct);
    CHECK(hir->type_shapes[make_box->body.shape_index].struct_index < hir->structs.len);

    CHECK_EQ(static_cast<u8>(pick_or->body.kind), static_cast<u8>(HirExprKind::Or));
    CHECK(pick_or->body.shape_index != 0xffffffffu);
    REQUIRE(pick_or->body.shape_index < hir->type_shapes.len);
    CHECK_EQ(hir->type_shapes[pick_or->body.shape_index].type, HirTypeKind::I32);
    CHECK(hir->type_shapes[pick_or->body.shape_index].is_concrete);

    CHECK_EQ(static_cast<u8>(pick_match->body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK(pick_match->body.shape_index != 0xffffffffu);
    REQUIRE(pick_match->body.shape_index < hir->type_shapes.len);
    CHECK_EQ(hir->type_shapes[pick_match->body.shape_index].type, HirTypeKind::I32);
    CHECK(hir->type_shapes[pick_match->body.shape_index].is_concrete);
}

TEST(frontend,
     import_relative_file_preserves_imported_protocol_default_method_wrapper_metadata_in_hir) {
    const std::string dir = "/tmp/rut_import_protocol_default_wrapper_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "protocol MaybeFail { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
        out << "Box<T> impl MaybeFail {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const auto* maybe_code = [&]() -> const HirProtocol* {
        for (u32 i = 0; i < hir->protocols.len; i++) {
            if (hir->protocols[i].name.eq(lit("MaybeCode"))) return &hir->protocols[i];
        }
        return nullptr;
    }();
    REQUIRE(maybe_code != nullptr);
    const auto* maybe_code_req = [&]() -> const HirProtocol::MethodDecl* {
        for (u32 i = 0; i < maybe_code->methods.len; i++) {
            if (maybe_code->methods[i].name.eq(lit("code"))) return &maybe_code->methods[i];
        }
        return nullptr;
    }();
    REQUIRE(maybe_code_req != nullptr);
    CHECK(maybe_code_req->function_index != 0xffffffffu);
    CHECK(maybe_code_req->return_may_nil);
    CHECK_FALSE(maybe_code_req->return_may_error);

    const auto* maybe_fail = [&]() -> const HirProtocol* {
        for (u32 i = 0; i < hir->protocols.len; i++) {
            if (hir->protocols[i].name.eq(lit("MaybeFail"))) return &hir->protocols[i];
        }
        return nullptr;
    }();
    REQUIRE(maybe_fail != nullptr);
    const auto* maybe_fail_req = [&]() -> const HirProtocol::MethodDecl* {
        for (u32 i = 0; i < maybe_fail->methods.len; i++) {
            if (maybe_fail->methods[i].name.eq(lit("code"))) return &maybe_fail->methods[i];
        }
        return nullptr;
    }();
    REQUIRE(maybe_fail_req != nullptr);
    CHECK(maybe_fail_req->function_index != 0xffffffffu);
    CHECK_FALSE(maybe_fail_req->return_may_nil);
    CHECK(maybe_fail_req->return_may_error);
    CHECK(maybe_fail_req->return_error_variant_index != 0xffffffffu);
}

TEST(frontend,
     imported_generic_receiver_protocol_call_preserves_default_method_wrapper_metadata_in_hir) {
    const std::string dir = "/tmp/rut_import_protocol_call_wrapper_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "protocol MaybeFail { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
        out << "Box<T> impl MaybeFail {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func runOpt<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
func runErr<T: MaybeFail>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    if runOpt(Box(value: 1)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const auto* run_opt = [&]() -> const HirFunction* {
        for (u32 i = 0; i < hir->functions.len; i++) {
            if (hir->functions[i].name.eq(lit("runOpt"))) return &hir->functions[i];
        }
        return nullptr;
    }();
    REQUIRE(run_opt != nullptr);
    REQUIRE(run_opt->body.lhs != nullptr);
    CHECK_EQ(static_cast<u8>(run_opt->body.kind), static_cast<u8>(HirExprKind::Or));
    CHECK_EQ(static_cast<u8>(run_opt->body.lhs->kind), static_cast<u8>(HirExprKind::ProtocolCall));
    CHECK(run_opt->body.lhs->may_nil);
    CHECK_FALSE(run_opt->body.lhs->may_error);

    const auto* run_err = [&]() -> const HirFunction* {
        for (u32 i = 0; i < hir->functions.len; i++) {
            if (hir->functions[i].name.eq(lit("runErr"))) return &hir->functions[i];
        }
        return nullptr;
    }();
    REQUIRE(run_err != nullptr);
    REQUIRE(run_err->body.lhs != nullptr);
    CHECK_EQ(static_cast<u8>(run_err->body.kind), static_cast<u8>(HirExprKind::Or));
    CHECK_EQ(static_cast<u8>(run_err->body.lhs->kind), static_cast<u8>(HirExprKind::ProtocolCall));
    CHECK_FALSE(run_err->body.lhs->may_nil);
    CHECK(run_err->body.lhs->may_error);
    CHECK(run_err->body.lhs->error_variant_index != 0xffffffffu);
}

TEST(
    frontend,
    import_relative_file_inlines_imported_generic_empty_impl_default_method_wrappers_in_route_hir) {
    const std::string dir = "/tmp/rut_import_protocol_call_wrapper_route_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "protocol MaybeFail { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
        out << "Box<T> impl MaybeFail {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func runOpt<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
func runErr<T: MaybeFail>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    let a = runOpt(Box(value: 1))
    let b = runErr(Box(value: 1))
    if a == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);

    const auto& opt = hir->routes[0].locals[0].init;
    CHECK_EQ(static_cast<u8>(opt.kind), static_cast<u8>(HirExprKind::Or));
    REQUIRE(opt.lhs != nullptr);
    CHECK_EQ(static_cast<u8>(opt.lhs->kind), static_cast<u8>(HirExprKind::Nil));
    CHECK(opt.lhs->may_nil);
    CHECK_FALSE(opt.lhs->may_error);

    const auto& err = hir->routes[0].locals[1].init;
    CHECK_EQ(static_cast<u8>(err.kind), static_cast<u8>(HirExprKind::Or));
    REQUIRE(err.lhs != nullptr);
    CHECK_EQ(static_cast<u8>(err.lhs->kind), static_cast<u8>(HirExprKind::Error));
    CHECK_FALSE(err.lhs->may_nil);
    CHECK(err.lhs->may_error);
    CHECK(err.lhs->error_variant_index != 0xffffffffu);
}

TEST(frontend, import_relative_file_preserves_imported_decl_type_arg_shape_indices_in_hir) {
    const std::string dir = "/tmp/rut_import_decl_type_arg_shapes_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Item { value: i32 }\n";
        out << "struct Wrap<T> { value: T }\n";
        out << "struct Box<T> { value: T }\n";
        out << "struct Holder { value: Box<Wrap<Item>> }\n";
        out << "variant Result { ok(Box<Wrap<Item>>), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);

    const auto* holder = [&]() -> const HirStruct* {
        for (u32 i = 0; i < hir->structs.len; i++) {
            if (hir->structs[i].name.eq(lit("Holder"))) return &hir->structs[i];
        }
        return nullptr;
    }();
    REQUIRE(holder != nullptr);
    const auto& field = holder->fields[0];
    REQUIRE(field.type_arg_count == 1);
    CHECK(field.type_args[0].shape_index != 0xffffffffu);
    CHECK(hir->type_shapes[field.type_args[0].shape_index].type == HirTypeKind::Struct);

    const auto* result = [&]() -> const HirVariant* {
        for (u32 i = 0; i < hir->variants.len; i++) {
            if (hir->variants[i].name.eq(lit("Result"))) return &hir->variants[i];
        }
        return nullptr;
    }();
    REQUIRE(result != nullptr);
    const auto& payload = result->cases[0];
    REQUIRE(payload.payload_type_arg_count == 1);
    CHECK(payload.payload_type_args[0].shape_index != 0xffffffffu);
    CHECK(hir->type_shapes[payload.payload_type_args[0].shape_index].type == HirTypeKind::Struct);
}

TEST(frontend, lower_to_rir_supports_imported_function_body_struct_init_projection) {
    const std::string dir = "/tmp/rut_import_function_body_struct_init_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "func make() -> Box => Box(value: 200)\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let box = make()
    if box.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, lower_to_rir_supports_imported_function_body_variant_case_projection) {
    const std::string dir = "/tmp/rut_import_function_body_variant_case_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result { ok(i32), err }\n";
        out << "struct Holder { state: Result }\n";
        out << "func make() -> Result => Result.ok(200)\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let holder = proto.Holder(state: make())
    if holder.state == proto.Result.ok(200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, lower_to_rir_supports_imported_function_body_ifelse_struct_projection) {
    const std::string dir = "/tmp/rut_import_function_body_ifelse_struct_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "func make(ok: bool) -> Box {\n";
        out << "    if ok { Box(value: 200) } else { Box(value: 500) }\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let box = make(true)
    if box.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, lower_to_rir_supports_imported_function_body_or) {
    const std::string dir = "/tmp/rut_import_function_body_or_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "func maybe(ok: bool) { if ok { 200 } else { nil } }\n";
        out << "func pick(ok: bool) -> i32 => or(maybe(ok), 500)\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let code = pick(true)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend,
     lower_to_rir_supports_imported_generic_empty_impl_for_optional_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_optional_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend,
     lower_to_rir_supports_imported_generic_empty_impl_for_error_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_error_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" { if run(Box(value: 1)) == 200 { return 200 } else { return 500 } }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, lower_to_rir_supports_imported_function_body_match) {
    const std::string dir = "/tmp/rut_import_function_body_match_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result { ok, err }\n";
        out << "func pick(x: Result) -> i32 {\n";
        out << "    match x {\n";
        out << "        case .ok => 200\n";
        out << "        case .err => 500\n";
        out << "    }\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let code = pick(proto.Result.ok)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend,
     lower_to_rir_supports_import_namespace_route_match_nested_struct_payload_projection) {
    const std::string dir = "/tmp/rut_import_namespace_match_nested_struct_payload_lower_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "struct Outer { inner: Box }\n";
        out << "variant Result { ok(Outer), err }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let state = proto.Result.ok(proto.Outer(inner: proto.Box(value: 200)))
    match state {
    case .ok(v): {
        let code = v.inner.value
        if code == 200 { return 200 } else { return 500 }
    }
    case .err: return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, route_guard_bound_struct_preserves_shape_index) {
    const char* src =
        "struct Box { value: i32 }\n"
        "func maybeBox(ok: bool) -> Box { if ok { Box(value: 200) } else { nil } }\n"
        "route GET \"/users\" { guard let picked = maybeBox(true) else { return 401 } if "
        "picked.value == 200 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[0].struct_index, 0u);
    CHECK_NE(hir->routes[0].locals[0].shape_index, 0xffffffffu);
}

TEST(frontend, route_guard_bound_runtime_error_struct_preserves_init_shape) {
    const char* src =
        "struct Box { value: i32 }\n"
        "func maybeBox(ok: bool) -> Box { if ok { Box(value: 200) } else { error(.timeout) } }\n"
        "route GET \"/users\" { guard let picked = maybeBox(true) else { return 401 } if "
        "picked.value == 200 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    const auto& picked = hir->routes[0].locals[0];
    CHECK_EQ(picked.type, HirTypeKind::Struct);
    CHECK_EQ(picked.struct_index, 0u);
    CHECK_NE(picked.shape_index, 0xffffffffu);
    CHECK_EQ(static_cast<u8>(picked.init.kind), static_cast<u8>(HirExprKind::ValueOf));
    CHECK_EQ(picked.init.type, HirTypeKind::Struct);
    CHECK_EQ(picked.init.struct_index, 0u);
    CHECK_NE(picked.init.shape_index, 0xffffffffu);
}

TEST(frontend, match_arm_guard_bound_struct_preserves_shape_index) {
    const char* src =
        "struct Box { value: i32 }\n"
        "variant Result { ok, err }\n"
        "func maybeBox(ok: bool) -> Box { if ok { Box(value: 200) } else { nil } }\n"
        "route GET \"/users\" { let state = Result.ok match state { case .ok: { guard let picked = "
        "maybeBox(true) else { return 401 } if picked.value == 200 { return 200 } else { return "
        "500 } } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[1].struct_index, 0u);
    CHECK_NE(hir->routes[0].locals[1].shape_index, 0xffffffffu);
}

TEST(frontend, generic_variant_constructor_and_match_are_supported) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
route GET "/users" {
    let state = Result<i32>.ok(200)
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].variant.type_params.len, 1u);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->variants[0].type_params.len, 1u);
    CHECK_EQ(hir->variants[1].template_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].variant_index, 1u);
}
TEST(frontend, explicit_generic_variant_constructor_records_instance_shape_index) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Wrap<T> { some(T), none }
route GET "/users" {
    let state = Wrap<Box<i32>>.some(Box<i32>(value: 200))
    match state {
    case .some(v): return 200
    case .none: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    const auto& inst = hir->variants[1];
    CHECK_EQ(inst.template_variant_index, 0u);
    CHECK_EQ(inst.instance_type_arg_count, 1u);
    CHECK_EQ(inst.instance_type_args[0], HirTypeKind::Struct);
    CHECK_NE(inst.instance_shape_indices[0], 0xffffffffu);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_NE(hir->routes[0].locals[0].shape_index, 0xffffffffu);
    CHECK_NE(hir->routes[0].locals[0].init.shape_index, 0xffffffffu);
}
TEST(frontend, generic_variant_constructor_infers_type_argument_from_single_payload_case) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
route GET "/users" {
    let state = Result.ok(200)
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->variants[0].type_params.len, 1u);
    CHECK_EQ(hir->variants[1].template_variant_index, 0u);
    CHECK_EQ(hir->variants[1].instance_type_arg_count, 1u);
    CHECK_EQ(hir->variants[1].instance_type_args[0], HirTypeKind::I32);
    CHECK_EQ(hir->routes[0].locals[0].variant_index, 1u);
}
TEST(frontend, generic_variant_instance_records_shape_for_tuple_of_struct_type_arg) {
    const auto src = R"rut(
struct Item { value: i32 }
variant Result<T> { ok(T), err }
route GET "/users" {
    let state = Result.ok((Item(value: 7), 9))
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->variants[1].template_variant_index, 0u);
    CHECK_EQ(hir->variants[1].instance_type_arg_count, 1u);
    CHECK_EQ(hir->variants[1].instance_type_args[0], HirTypeKind::Tuple);
    CHECK_EQ(hir->variants[1].instance_tuple_lens[0], 2u);
    CHECK_EQ(hir->variants[1].instance_tuple_types[0][0], HirTypeKind::Struct);
    CHECK_EQ(hir->variants[1].instance_tuple_struct_indices[0][0], 0u);
    CHECK_NE(hir->variants[1].instance_shape_indices[0], 0xffffffffu);
    CHECK_EQ(hir->routes[0].locals[0].variant_index, 1u);
}
TEST(frontend, generic_variant_tuple_of_struct_payload_preserves_struct_slots_after_instantiation) {
    const auto src = R"rut(
struct Item { value: i32 }
variant Result<T> { ok(T), err }
route GET "/users" {
    let state = Result.ok((Item(value: 7), 9))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    const auto& inst = hir->variants[1];
    CHECK_EQ(inst.template_variant_index, 0u);
    REQUIRE(inst.cases.len >= 1u);
    CHECK_EQ(inst.cases[0].payload_type, HirTypeKind::Tuple);
    CHECK_EQ(inst.cases[0].payload_tuple_len, 2u);
    CHECK_EQ(inst.cases[0].payload_tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(inst.cases[0].payload_tuple_struct_indices[0], 0u);
}
TEST(frontend, mir_build_preserves_variant_instance_shape_for_tuple_of_struct_type_arg) {
    const auto src = R"rut(
struct Item { value: i32 }
variant Result<T> { ok(T), err }
route GET "/users" {
    let state = Result.ok((Item(value: 7), 9))
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE(mir->variants.len >= 2u);
    CHECK_EQ(mir->variants[1].template_variant_index, 0u);
    CHECK_EQ(mir->variants[1].instance_type_arg_count, 1u);
    CHECK_EQ(mir->variants[1].instance_type_args[0], MirTypeKind::Tuple);
    CHECK_NE(mir->variants[1].instance_shape_indices[0], 0xffffffffu);
    REQUIRE(mir->variants[1].instance_shape_indices[0] < mir->type_shapes.len);
    CHECK(mir->type_shapes[mir->variants[1].instance_shape_indices[0]].is_concrete);
    CHECK(mir->type_shapes[mir->variants[1].instance_shape_indices[0]].carrier_ready);
}
TEST(frontend, named_errors_aggregate_into_hidden_error_variant) {
    const char* src =
        "route GET \"/users\" { let timeout = error(.timeout) let denied = error(.forbidden) "
        "return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->variants.len, 1u);
    CHECK(hir->variants[0].name.eq(lit("__error_route")));
    REQUIRE_EQ(hir->variants[0].cases.len, 2u);
    CHECK(hir->variants[0].cases[0].name.eq(lit("timeout")));
    CHECK(hir->variants[0].cases[1].name.eq(lit("forbidden")));
    CHECK_EQ(hir->routes[0].error_variant_index, 0u);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[0].error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].init.error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].init.error_case_index, 0u);
    CHECK_EQ(hir->routes[0].locals[1].error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[1].init.error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[1].init.error_case_index, 1u);
}
TEST(frontend, error_message_is_preserved_for_named_error) {
    const char* src =
        "route GET \"/users\" { let timeout = error(.timeout, \"request timed out\") return 200 "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 2u);
    CHECK(ast->items[0].route.statements[0].expr.msg.eq(lit("request timed out")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].init.msg.eq(lit("request timed out")));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    CHECK(mir->functions[0].locals[0].init.msg.eq(lit("request timed out")));
}
TEST(frontend, custom_error_struct_can_be_used_in_error_constructor) {
    const char* src =
        "struct AuthError { err: Error }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\") return 200 "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].kind), static_cast<u8>(AstItemKind::Struct));
    REQUIRE_EQ(ast->items[0].struct_decl.fields.len, 1u);
    CHECK(ast->items[0].struct_decl.fields[0].name.eq(lit("err")));
    CHECK(!ast->items[0].struct_decl.fields[0].type.is_tuple);
    CHECK(ast->items[0].struct_decl.fields[0].type.name.eq(lit("Error")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->structs.len, 1u);
    CHECK(hir->structs[0].name.eq(lit("AuthError")));
    CHECK(hir->structs[0].conforms_error);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::Error));
    CHECK_EQ(hir->routes[0].locals[0].init.error_struct_index, 0u);
    CHECK(hir->routes[0].locals[0].init.msg.eq(lit("timed out")));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    CHECK_EQ(mir->functions[0].locals[0].init.error_struct_index, 0u);
}
TEST(frontend, custom_error_struct_with_extra_fields_lowers_to_rir_struct) {
    const char* src =
        "struct AuthError { err: Error, token: str, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", token: "
        "\"abc\", retry: 3) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir.has_value()) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->structs.len, 1u);
    REQUIRE_EQ(hir->structs[0].fields.len, 3u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->structs.len, 1u);
    REQUIRE_EQ(mir->structs[0].fields.len, 3u);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool found_auth_error = false;
    for (u32 i = 0; i < rir.module.struct_count; i++) {
        auto* sd = rir.module.struct_defs[i];
        if (!sd->name.eq(lit("AuthError"))) continue;
        found_auth_error = true;
        REQUIRE_EQ(sd->field_count, 3u);
        CHECK(sd->fields()[0].name.eq(lit("err")));
        CHECK(sd->fields()[1].name.eq(lit("token")));
        CHECK(sd->fields()[2].name.eq(lit("retry")));
        break;
    }
    CHECK(found_auth_error);
    rir.destroy();
}
TEST(frontend, custom_error_struct_constructor_preserves_extra_field_inits) {
    const char* src =
        "struct AuthError { err: Error, token: str, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", token: "
        "\"abc\", retry: 3) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[1].route.statements[0].expr.field_inits.len, 2u);
    CHECK(ast->items[1].route.statements[0].expr.field_inits[0].name.eq(lit("token")));
    CHECK(ast->items[1].route.statements[0].expr.field_inits[1].name.eq(lit("retry")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals[0].init.field_inits.len, 2u);
    CHECK(hir->routes[0].locals[0].init.field_inits[0].name.eq(lit("token")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.field_inits[0].value->type),
             static_cast<u8>(HirTypeKind::Str));
    CHECK(hir->routes[0].locals[0].init.field_inits[1].name.eq(lit("retry")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.field_inits[1].value->type),
             static_cast<u8>(HirTypeKind::I32));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals[0].init.field_inits.len, 2u);
    CHECK(mir->functions[0].locals[0].init.field_inits[0].name.eq(lit("token")));
    CHECK(mir->functions[0].locals[0].init.field_inits[1].name.eq(lit("retry")));
}
TEST(frontend, custom_error_struct_with_tuple_field_lowers_to_rir_struct) {
    const char* src =
        "struct AuthError { err: Error, pair: (i32, i32) }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", pair: (200, "
        "500)) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE(ast->items[0].struct_decl.fields[1].type.is_tuple);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->structs.len, 1u);
    CHECK_EQ(hir->structs[0].fields[1].type, HirTypeKind::Tuple);
    CHECK_EQ(hir->structs[0].fields[1].tuple_len, 2u);
    REQUIRE_EQ(hir->routes[0].locals[0].init.field_inits.len, 1u);
    CHECK_EQ(hir->routes[0].locals[0].init.field_inits[0].value->type, HirTypeKind::Tuple);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    CHECK_EQ(mir->structs[0].fields[1].type, MirTypeKind::Tuple);
    CHECK_EQ(mir->structs[0].fields[1].tuple_len, 2u);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool found_auth_error = false;
    for (u32 i = 0; i < rir.module.struct_count; i++) {
        auto* sd = rir.module.struct_defs[i];
        if (!sd->name.eq(lit("AuthError"))) continue;
        found_auth_error = true;
        REQUIRE_EQ(sd->field_count, 2u);
        CHECK(sd->fields()[0].name.eq(lit("err")));
        CHECK(sd->fields()[1].name.eq(lit("pair")));
        break;
    }
    CHECK(found_auth_error);
    rir.destroy();
}
TEST(frontend, analyze_rejects_non_error_struct_in_error_constructor) {
    const char* src =
        "struct Foo { code: i32 }\n"
        "route GET \"/users\" { let failed = error(Foo, .timeout, \"timed out\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_custom_error_constructor_missing_extra_fields) {
    const char* src =
        "struct AuthError { err: Error, token: str, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", token: "
        "\"abc\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_custom_error_constructor_unknown_extra_field) {
    const char* src =
        "struct AuthError { err: Error, token: str }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", retry: 3) "
        "return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_custom_error_constructor_wrong_extra_field_type) {
    const char* src =
        "struct AuthError { err: Error, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", retry: "
        "\"soon\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_user_declared_struct_error) {
    const char* src =
        "struct Error { code: i32 }\n"
        "route GET \"/users\" { return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_duplicate_struct_field_names) {
    const char* src =
        "struct AuthError { err: Error, err: Error }\n"
        "route GET \"/users\" { return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, explicit_error_variant_is_not_wrapped_again) {
    const char* src =
        "variant AuthError { timeout, forbidden }\n"
        "route GET \"/users\" { let failed = error(AuthError.timeout) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->variants.len, 1u);
    CHECK_EQ(hir->routes[0].error_variant_index, 0xffffffffu);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(hir->routes[0].locals[0].error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].init.error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].init.error_case_index, 0u);
}
TEST(frontend, error_message_is_preserved_for_explicit_error_variant) {
    const char* src =
        "variant AuthError { timeout, forbidden }\n"
        "route GET \"/users\" { let failed = error(AuthError.timeout, \"timed out\") return 200 "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items.len, 2u);
    CHECK(ast->items[1].route.statements[0].expr.msg.eq(lit("timed out")));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].init.msg.eq(lit("timed out")));
    CHECK_EQ(hir->routes[0].locals[0].init.error_variant_index, 0u);
    CHECK_EQ(hir->routes[0].locals[0].init.error_case_index, 0u);
}
TEST(frontend, lower_to_rir_emits_standard_error_struct) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout, \"timed out\") let code = or(failed, "
        "200) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool found_error = false;
    for (u32 i = 0; i < rir.module.struct_count; i++) {
        auto* sd = rir.module.struct_defs[i];
        if (!sd->name.eq(lit("Error"))) continue;
        found_error = true;
        REQUIRE_EQ(sd->field_count, 5u);
        CHECK(sd->fields()[0].name.eq(lit("code")));
        CHECK(sd->fields()[1].name.eq(lit("msg")));
        CHECK(sd->fields()[2].name.eq(lit("file")));
        CHECK(sd->fields()[3].name.eq(lit("func")));
        CHECK(sd->fields()[4].name.eq(lit("line")));
        break;
    }
    CHECK(found_error);
    rir.destroy();
}
TEST(frontend, lower_to_rir_uses_module_name_for_error_file_field) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout, \"timed out\") let code = or(failed, "
        "200) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    rir.source_name = lit("custom_frontend.rut");
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool saw_custom_file = false;
    for (u32 bi = 0; bi < rir.module.functions[0].block_count; bi++) {
        const auto& block = rir.module.functions[0].blocks[bi];
        for (u32 ii = 0; ii < block.inst_count; ii++) {
            const auto& inst = block.insts[ii];
            if (inst.op != rir::Opcode::ConstStr) continue;
            if (inst.imm.str_val.eq(lit("custom_frontend.rut"))) {
                saw_custom_file = true;
                break;
            }
        }
        if (saw_custom_file) break;
    }
    CHECK(saw_custom_file);
    rir.destroy();
}
TEST(frontend, custom_error_struct_lowers_with_standard_error_metadata) {
    const char* src =
        "struct AuthError { err: Error }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\") let code = "
        "or(failed, 200) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    rir.source_name = lit("custom_auth_error.rut");
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool saw_error_struct = false;
    bool saw_msg = false;
    bool saw_file = false;
    bool saw_func = false;
    for (u32 i = 0; i < rir.module.struct_count; i++) {
        auto* sd = rir.module.struct_defs[i];
        if (!sd->name.eq(lit("Error"))) continue;
        saw_error_struct = true;
        break;
    }
    for (u32 bi = 0; bi < rir.module.functions[0].block_count; bi++) {
        const auto& block = rir.module.functions[0].blocks[bi];
        for (u32 ii = 0; ii < block.inst_count; ii++) {
            const auto& inst = block.insts[ii];
            if (inst.op != rir::Opcode::ConstStr) continue;
            saw_msg = saw_msg || inst.imm.str_val.eq(lit("timed out"));
            saw_file = saw_file || inst.imm.str_val.eq(lit("custom_auth_error.rut"));
            saw_func = saw_func || inst.imm.str_val.eq(lit("route"));
        }
    }
    CHECK(saw_error_struct);
    CHECK(saw_msg);
    CHECK(saw_file);
    CHECK(saw_func);
    rir.destroy();
}
TEST(frontend, custom_error_struct_extra_fields_enter_runtime_lowering) {
    const char* src =
        "struct AuthError { err: Error, token: str, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", token: "
        "\"abc\", retry: 3) let code = or(failed, 200) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    bool saw_auth_error_create = false;
    bool saw_retry = false;
    bool saw_token = false;
    for (u32 bi = 0; bi < rir.module.functions[0].block_count; bi++) {
        const auto& block = rir.module.functions[0].blocks[bi];
        for (u32 ii = 0; ii < block.inst_count; ii++) {
            const auto& inst = block.insts[ii];
            if (inst.op == rir::Opcode::StructCreate &&
                inst.imm.struct_ref.name.eq(lit("AuthError")))
                saw_auth_error_create = true;
            if (inst.op == rir::Opcode::ConstI32 && inst.imm.i32_val == 3) saw_retry = true;
            if (inst.op == rir::Opcode::ConstStr && inst.imm.str_val.eq(lit("abc")))
                saw_token = true;
        }
    }
    CHECK(saw_auth_error_create);
    CHECK(saw_retry);
    CHECK(saw_token);
    rir.destroy();
}
TEST(frontend, custom_error_struct_fields_can_be_projected_from_known_error_local) {
    const char* src =
        "struct AuthError { err: Error, token: str, retry: i32 }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", token: "
        "\"abc\", retry: 3) let token = failed.token let retry = failed.retry if retry == 3 { "
        "return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 3u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::Str);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::StrLit));
    CHECK(hir.value().routes[0].locals[1].init.str_value.eq(lit("abc")));
    CHECK_EQ(hir.value().routes[0].locals[2].type, HirTypeKind::I32);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[2].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir.value().routes[0].locals[2].init.int_value, 3);
}
TEST(frontend, custom_error_struct_tuple_field_can_flow_into_pipe_slots) {
    const char* src =
        "struct AuthError { err: Error, pair: (i32, i32) }\n"
        "func second(a: i32, b: i32) -> i32 => b\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", pair: (200, "
        "500)) let code = failed.pair | second(_2, _1) if code == 200 { return 200 } else { return "
        "500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 2u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::I32);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, plain_struct_constructor_and_field_projection_are_supported) {
    constexpr auto src =
        "struct Foo { code: i32, msg: str }\n"
        "route GET \"/users\" { let foo = Foo(code: 200, msg: \"ok\") let code = foo.code let msg "
        "= foo.msg if code == 200 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 3u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[1].init.kind, HirExprKind::Field);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::I32);
    CHECK_EQ(hir->routes[0].locals[2].init.kind, HirExprKind::Field);
    CHECK_EQ(hir->routes[0].locals[2].type, HirTypeKind::Str);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, generic_struct_constructor_and_field_projection_are_supported) {
    const char* src =
        "struct Box<T> { value: T }\n"
        "route GET \"/users\" { let box = Box<i32>(value: 200) if box.value == 200 { return 200 } "
        "else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].struct_decl.type_params.len, 1u);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    CHECK_EQ(hir->structs[0].type_params.len, 1u);
    CHECK_EQ(hir->structs[1].template_struct_index, 0u);
    CHECK_EQ(hir->structs[1].instance_type_arg_count, 1u);
    CHECK_EQ(hir->structs[1].instance_type_args[0], HirTypeKind::I32);
    CHECK_EQ(hir->routes[0].locals[0].struct_index, 1u);
}
TEST(frontend, generic_struct_constructor_infers_type_argument_from_field_shape) {
    const auto src = R"rut(
struct Box<T> { value: T }
route GET "/users" {
    let box = Box(value: 200)
    if box.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    CHECK_EQ(hir->structs[1].template_struct_index, 0u);
    CHECK_EQ(hir->structs[1].instance_type_arg_count, 1u);
    CHECK_EQ(hir->structs[1].instance_type_args[0], HirTypeKind::I32);
    CHECK_EQ(hir->routes[0].locals[0].struct_index, 1u);
}
TEST(frontend, generic_struct_instance_records_shape_for_tuple_of_struct_type_arg) {
    const auto src = R"rut(
struct Item { value: i32 }
struct Box<T> { value: T }
route GET "/users" {
    let box = Box(value: (Item(value: 7), 9))
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 3u);
    CHECK_EQ(hir->structs[2].template_struct_index, 1u);
    CHECK_EQ(hir->structs[2].instance_type_arg_count, 1u);
    CHECK_EQ(hir->structs[2].instance_type_args[0], HirTypeKind::Tuple);
    CHECK_EQ(hir->structs[2].instance_tuple_lens[0], 2u);
    CHECK_EQ(hir->structs[2].instance_tuple_types[0][0], HirTypeKind::Struct);
    CHECK_EQ(hir->structs[2].instance_tuple_struct_indices[0][0], 0u);
    CHECK_NE(hir->structs[2].instance_shape_indices[0], 0xffffffffu);
    CHECK_EQ(hir->routes[0].locals[0].struct_index, 2u);
}
TEST(frontend, generic_struct_field_tuple_of_struct_preserves_struct_slots_after_instantiation) {
    const auto src = R"rut(
struct Item { value: i32 }
struct Wrap<T> { payload: T }
func boxCode(x: Item) -> i32 => x.value
func read(x: Wrap<(Item, i32)>) -> i32 => x.payload | boxCode(_1)
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 3u);
    const auto& wrap_inst = hir->structs[2];
    CHECK_EQ(wrap_inst.template_struct_index, 1u);
    REQUIRE_EQ(wrap_inst.fields.len, 1u);
    const auto& payload = wrap_inst.fields[0];
    CHECK_EQ(payload.type, HirTypeKind::Tuple);
    REQUIRE_EQ(payload.tuple_len, 2u);
    CHECK_EQ(payload.tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(payload.tuple_struct_indices[0], 0u);
}
TEST(frontend, mir_build_preserves_struct_instance_shape_for_tuple_of_struct_type_arg) {
    const auto src = R"rut(
struct Item { value: i32 }
struct Box<T> { value: T }
route GET "/users" {
    let box = Box(value: (Item(value: 7), 9))
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE(mir->structs.len >= 3u);
    CHECK_EQ(mir->structs[2].template_struct_index, 1u);
    CHECK_EQ(mir->structs[2].instance_type_arg_count, 1u);
    CHECK_EQ(mir->structs[2].instance_type_args[0], MirTypeKind::Tuple);
    CHECK_NE(mir->structs[2].instance_shape_indices[0], 0xffffffffu);
    REQUIRE(mir->structs[2].instance_shape_indices[0] < mir->type_shapes.len);
    CHECK(mir->type_shapes[mir->structs[2].instance_shape_indices[0]].is_concrete);
    CHECK(mir->type_shapes[mir->structs[2].instance_shape_indices[0]].carrier_ready);
}
TEST(frontend, concrete_generic_type_refs_are_supported_in_let_types) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
route GET "/users" {
    let state: Result<i32> = Result.ok(200)
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Variant);
    CHECK_EQ(hir->routes[0].locals[0].variant_index, 1u);
}
TEST(frontend, concrete_generic_type_refs_are_supported_in_function_signatures) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
func wrap(x: Result<i32>) -> Result<i32> => x
route GET "/users" {
    let state = wrap(Result.ok(200))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Variant);
    CHECK_EQ(hir->functions[0].params[0].variant_index, 1u);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::Variant);
    CHECK_EQ(hir->functions[0].return_variant_index, 1u);
}
TEST(frontend, concrete_generic_struct_type_refs_are_supported_in_function_signatures) {
    const auto src = R"rut(
struct Box<T> { value: T }
func wrap(x: Box<i32>) -> Box<i32> => x
route GET "/users" {
    let box = wrap(Box(value: 200))
    if box.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->functions[0].params[0].struct_index, 1u);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::Struct);
    CHECK_EQ(hir->functions[0].return_struct_index, 1u);
}
TEST(frontend, concrete_generic_type_refs_are_supported_in_struct_fields) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder { state: Result<i32> }
route GET "/users" {
    let holder = Holder(state: Result.ok(200))
    match holder.state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 1u);
    CHECK_EQ(hir->structs[0].fields[0].type, HirTypeKind::Variant);
    CHECK_EQ(hir->structs[0].fields[0].variant_index, 1u);
}

TEST(frontend, variant_struct_field_projection_supports_equality) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder { state: Result<i32> }
route GET "/users" {
    let holder = Holder(state: Result.ok(200))
    if holder.state == Result.ok(200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, variant_struct_field_projection_supports_ordering) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder { state: Result<i32> }
route GET "/users" {
    let holder = Holder(state: Result.ok(200))
    if holder.state < Result.ok(500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, import_namespace_variant_struct_field_projection_supports_equality) {
    const std::string dir = "/tmp/rut_import_namespace_variant_field_eq_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result { ok(i32), err }\n";
        out << "struct Holder { state: Result }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let holder = proto.Holder(state: proto.Result.ok(200))
    if holder.state == proto.Result.ok(200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, import_namespace_variant_struct_field_projection_supports_ordering) {
    const std::string dir = "/tmp/rut_import_namespace_variant_field_ord_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "variant Result { ok(i32), err }\n";
        out << "struct Holder { state: Result }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let holder = proto.Holder(state: proto.Result.ok(200))
    if holder.state < proto.Result.ok(500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}

TEST(frontend, tuple_of_struct_field_projection_supports_ordering) {
    const auto src = R"rut(
struct Item { value: i32 }
struct Holder { pair: (Item, i32) }
route GET "/users" {
    let holder = Holder(pair: (Item(value: 200), 500))
    if holder.pair < (Item(value: 200), 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, import_namespace_tuple_of_struct_field_projection_supports_ordering) {
    const std::string dir = "/tmp/rut_import_namespace_tuple_struct_field_ord_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Item { value: i32 }\n";
        out << "struct Holder { pair: (Item, i32) }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let holder = proto.Holder(pair: (proto.Item(value: 200), 500))
    if holder.pair < (proto.Item(value: 200), 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, concrete_generic_type_refs_are_supported_in_variant_payloads) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
variant Outer { wrap(Result<i32>), bad }
func isOk(x: Result<i32>) -> bool {
    match x {
    case .ok => true
    case .err => false
    }
}
route GET "/users" {
    let state = Outer.wrap(Result.ok(200))
    match state {
    case .wrap(inner): {
        let ok = isOk(inner)
        if ok { return 200 } else { return 500 }
    }
    case .bad:
        return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 3u);
    CHECK_EQ(hir->variants[1].cases[0].payload_type, HirTypeKind::Variant);
    CHECK_EQ(hir->variants[1].cases[0].payload_variant_index, 2u);
}
TEST(frontend, concrete_generic_struct_type_refs_are_supported_in_variant_payloads) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Outer { wrap(Box<i32>), bad }
func is200(x: Box<i32>) -> bool => x.value == 200
route GET "/users" {
    let state = Outer.wrap(Box(value: 200))
    match state {
    case .wrap(inner): {
        let ok = is200(inner)
        if ok { return 200 } else { return 500 }
    }
    case .bad:
        return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    CHECK_EQ(hir->variants[0].cases[0].payload_type, HirTypeKind::Struct);
    CHECK_EQ(hir->variants[0].cases[0].payload_struct_index, 1u);
}
TEST(frontend, generic_struct_can_reference_generic_variant_with_same_type_arg) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: Result<T> }
route GET "/users" {
    let holder = Holder<i32>(state: Result.ok(200))
    match holder.state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->structs[1].template_struct_index, 0u);
    CHECK_EQ(hir->structs[1].fields[0].type, HirTypeKind::Variant);
    REQUIRE(hir->structs[1].fields[0].variant_index < hir->variants.len);
    CHECK_EQ(hir->variants[hir->structs[1].fields[0].variant_index].template_variant_index, 0u);
}
TEST(frontend, generic_variant_can_reference_generic_struct_with_same_type_arg) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Wrap<T> { some(Box<T>), none }
route GET "/users" {
    let state = Wrap<i32>.some(Box(value: 200))
    match state {
    case .some(box): {
        if box.value == 200 { return 200 } else { return 500 }
    }
    case .none:
        return 404
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE(hir->variants.len >= 2u);
    CHECK_EQ(hir->variants[1].template_variant_index, 0u);
    CHECK_EQ(hir->variants[1].cases[0].payload_type, HirTypeKind::Struct);
    REQUIRE(hir->variants[1].cases[0].payload_struct_index < hir->structs.len);
    CHECK_EQ(hir->structs[hir->variants[1].cases[0].payload_struct_index].template_struct_index,
             0u);
}
TEST(frontend, concrete_nested_generic_struct_type_refs_are_supported_in_function_signatures) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: Result<T> }
func unwrap(x: Holder<i32>) -> Result<i32> => x.state
route GET "/users" {
    let state = unwrap(Holder<i32>(state: Result.ok(200)))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE(hir->variants.len >= 2u);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->functions[0].params[0].struct_index, 1u);
    CHECK_NE(hir->functions[0].params[0].type_args[0].shape_index, 0xffffffffu);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::Variant);
    REQUIRE(hir->functions[0].return_variant_index < hir->variants.len);
    CHECK_EQ(hir->variants[hir->functions[0].return_variant_index].template_variant_index, 0u);
    CHECK_NE(hir->functions[0].return_type_args[0].shape_index, 0xffffffffu);
}
TEST(frontend, concrete_nested_generic_struct_type_ref_records_instance_shape_index) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: T }
func unwrap(x: Holder<Result<i32>>) -> Result<i32> => x.state
route GET "/users" {
    let state = unwrap(Holder<Result<i32>>(state: Result.ok(200)))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Struct);
    REQUIRE(hir->functions[0].params[0].struct_index < hir->structs.len);
    const auto& holder_inst = hir->structs[hir->functions[0].params[0].struct_index];
    CHECK_EQ(holder_inst.template_struct_index, 0u);
    CHECK_EQ(holder_inst.instance_type_arg_count, 1u);
    CHECK_EQ(holder_inst.instance_type_args[0], HirTypeKind::Variant);
    CHECK_NE(holder_inst.instance_shape_indices[0], 0xffffffffu);
    CHECK_NE(hir->functions[0].params[0].type_args[0].shape_index, 0xffffffffu);
}
TEST(frontend, explicit_generic_struct_init_records_instance_shape_index) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: T }
route GET "/users" {
    let holder = Holder<Result<i32>>(state: Result<i32>.ok(200))
    match holder.state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    const auto& inst = hir->structs[1];
    CHECK_EQ(inst.template_struct_index, 0u);
    CHECK_EQ(inst.instance_type_arg_count, 1u);
    CHECK_EQ(inst.instance_type_args[0], HirTypeKind::Variant);
    CHECK_NE(inst.instance_shape_indices[0], 0xffffffffu);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_NE(hir->routes[0].locals[0].shape_index, 0xffffffffu);
    CHECK_NE(hir->routes[0].locals[0].init.shape_index, 0xffffffffu);
}
TEST(frontend, concrete_nested_generic_variant_type_ref_records_instance_shape_index) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Wrap<T> { some(T), none }
func unwrap(x: Wrap<Box<i32>>) -> Box<i32> {
    match x {
    case .some(v) => v
    case .none => Box<i32>(value: 0)
    }
}
route GET "/users" {
    let box = unwrap(Wrap<Box<i32>>.some(Box<i32>(value: 200)))
    if box.value == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->variants.len >= 2u);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Variant);
    REQUIRE(hir->functions[0].params[0].variant_index < hir->variants.len);
    const auto& wrap_inst = hir->variants[hir->functions[0].params[0].variant_index];
    CHECK_EQ(wrap_inst.template_variant_index, 0u);
    CHECK_EQ(wrap_inst.instance_type_arg_count, 1u);
    CHECK_EQ(wrap_inst.instance_type_args[0], HirTypeKind::Struct);
    CHECK_NE(wrap_inst.instance_shape_indices[0], 0xffffffffu);
    CHECK_NE(hir->functions[0].params[0].type_args[0].shape_index, 0xffffffffu);
}
TEST(frontend, concrete_nested_generic_variant_type_refs_are_supported_in_function_signatures) {
    const auto src = R"rut(
struct Box<T> { value: T }
variant Wrap<T> { some(Box<T>), none }
func is200(x: Wrap<i32>) -> bool {
    match x {
    case .some(box) => box.value == 200
    case .none => false
    }
}
route GET "/users" {
    let ok = is200(Wrap<i32>.some(Box(value: 200)))
    if ok { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->structs.len >= 2u);
    REQUIRE(hir->variants.len >= 2u);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Variant);
    CHECK_EQ(hir->functions[0].params[0].variant_index, 1u);
    CHECK_NE(hir->functions[0].params[0].type_args[0].shape_index, 0xffffffffu);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::Bool);
}
TEST(frontend, tuple_literal_with_struct_element_can_flow_into_pipe) {
    const auto src = R"rut(
struct Box { value: i32 }
func boxCode(b: Box) -> i32 => b.value
route GET "/users" {
    let pair = (Box(value: 200), 1)
    let code = pair | boxCode(_1)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, variant_tuple_of_struct_payload_records_struct_slots) {
    const auto src = R"rut(
struct Box { value: i32 }
variant Result { ok((Box, i32)), err }
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->variants.len, 1u);
    REQUIRE_EQ(hir->variants[0].cases.len, 2u);
    const auto& ok = hir->variants[0].cases[0];
    CHECK(ok.has_payload);
    CHECK(ok.payload_type == HirTypeKind::Tuple);
    REQUIRE_EQ(ok.payload_tuple_len, 2u);
    CHECK(ok.payload_tuple_types[0] == HirTypeKind::Struct);
    CHECK(ok.payload_tuple_struct_indices[0] == 0u);
}

TEST(frontend, generic_function_tuple_of_struct_binding_preserves_struct_slots) {
    const auto src = R"rut(
struct Box { value: i32 }
func id<T>(x: T) -> T => x
func boxCode(b: Box) -> i32 => b.value
route GET "/users" {
    let pair = id((Box(value: 200), 1))
    let code = pair | boxCode(_1)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, generic_function_explicit_tuple_of_struct_type_arg_preserves_struct_slots) {
    const auto src = R"rut(
struct Box { value: i32 }
func id<T>(x: T) -> T => x
func boxCode(b: Box) -> i32 => b.value
route GET "/users" {
    let pair = id<(Box, i32)>((Box(value: 200), 1))
    let code = pair | boxCode(_1)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, plain_struct_tuple_field_can_flow_into_pipe_slots) {
    const char* src =
        "struct Foo { pair: (i32, i32) }\n"
        "func second(a: i32, b: i32) -> i32 => b\n"
        "route GET \"/users\" { let foo = Foo(pair: (200, 500)) let code = foo.pair | second(_2, "
        "_1) if code == 200 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::I32);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    CHECK(block_has_op(rir.module.functions[0].blocks[0], rir::Opcode::StructField));
    rir.destroy();
}

TEST(frontend, custom_error_nested_struct_field_projection) {
    const char* src =
        "struct Box { value: i32 }\n"
        "struct AuthError { err: Error, inner: Box }\n"
        "route GET \"/users\" { let failed = error(AuthError, .timeout, \"timed out\", inner: "
        "Box(value: 200)) let code = failed.inner.value if code == 200 { return 200 } else { "
        "return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 2u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::I32);
}

TEST(frontend, nested_struct_field_projection_preserves_projected_struct_type) {
    const char* src =
        "struct Box { value: i32 }\n"
        "struct Outer { inner: Box }\n"
        "route GET \"/users\" { let outer = Outer(inner: Box(value: 200)) let code = "
        "outer.inner.value if code == 200 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::I32);
}
TEST(frontend, plain_struct_tuple_of_struct_field_projection_preserves_struct_slots) {
    const char* src =
        "struct Box { value: i32 }\n"
        "struct Foo { pair: (Box, i32) }\n"
        "route GET \"/users\" { let foo = Foo(pair: (Box(value: 200), 500)) let pair = foo.pair "
        "return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(hir->routes[0].locals[1].type, HirTypeKind::Tuple);
    REQUIRE_EQ(hir->routes[0].locals[1].tuple_len, 2u);
    CHECK_EQ(hir->routes[0].locals[1].tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(hir->routes[0].locals[1].tuple_struct_indices[0], 0u);
}
TEST(frontend, plain_struct_variant_field_can_flow_into_match) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "struct Foo { state: AuthState }\n"
        "route GET \"/users\" { let foo = Foo(state: AuthState.timeout) match foo.state { case "
        ".timeout: return 200 case _: return 403 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Struct);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    CHECK(block_has_op(rir.module.functions[0].blocks[0], rir::Opcode::StructField));
    rir.destroy();
}
TEST(frontend, source_error_standard_fields_are_accessible) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout, \"timed out\") let code = failed.code "
        "let msg = failed.msg if code == 0 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 3u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::I32);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir.value().routes[0].locals[2].type, HirTypeKind::Str);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[2].init.kind),
             static_cast<u8>(HirExprKind::StrLit));
    CHECK(hir.value().routes[0].locals[2].init.str_value.eq(lit("timed out")));
}
TEST(frontend, source_error_line_field_is_accessible) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout, \"timed out\") let line = failed.line "
        "if line == 1 { return 200 } else { return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 2u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::I32);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir.value().routes[0].locals[1].init.int_value, 1);
}
TEST(frontend, source_error_file_and_func_fields_are_accessible) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout, \"timed out\") let file_name = "
        "failed.file let fn_name = failed.func return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir.value().routes[0].locals.len, 3u);
    CHECK_EQ(hir.value().routes[0].locals[1].type, HirTypeKind::Str);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::Field));
    CHECK(hir.value().routes[0].locals[1].init.str_value.eq(lit("file")));
    CHECK_EQ(hir.value().routes[0].locals[2].type, HirTypeKind::Str);
    CHECK_EQ(static_cast<u8>(hir.value().routes[0].locals[2].init.kind),
             static_cast<u8>(HirExprKind::Field));
    CHECK(hir.value().routes[0].locals[2].init.str_value.eq(lit("func")));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    CHECK(block_has_op(rir.module.functions[0].blocks[0], rir::Opcode::StructField));
    rir.destroy();
}
TEST(frontend, known_named_error_match_selects_error_case) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout) match failed { case .timeout: return "
        "503 case _: return 200 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Direct));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.direct_term.kind),
             static_cast<u8>(HirTerminatorKind::ReturnStatus));
    CHECK_EQ(hir->routes[0].control.direct_term.status_code, 503);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, if_const_selects_then_without_checking_else) {
    const char* src =
        "route GET \"/users\" { if const true { return 200 } else { forward missing } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Direct));
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.direct_term.kind),
             static_cast<u8>(HirTerminatorKind::ReturnStatus));
    CHECK_EQ(hir->routes[0].control.direct_term.status_code, 200);
}
TEST(frontend, if_const_rejects_runtime_condition) {
    const char* src =
        "route GET \"/users\" { if const req.header(\"Host\") { return 200 } else { return 500 } "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, match_const_selects_variant_case_without_checking_other_arms) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match const state { case .ok(x): if x "
        "== 200 { return 200 } else { return 500 } case .err: forward missing } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::If));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, function_match_const_preserves_outer_payload_binding) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "func pick(x: Result) -> i32 {\n"
        "  match x {\n"
        "    case .ok(v) => {\n"
        "      match const true {\n"
        "        case true => v\n"
        "        case false => 500\n"
        "      }\n"
        "    }\n"
        "    case .err => 404\n"
        "  }\n"
        "}\n"
        "route GET \"/users\" { let code = pick(Result.ok(200)) if code == 200 { return 200 } else "
        "{ return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::I32);
}
TEST(frontend, match_const_rejects_runtime_subject) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "route GET \"/users\" { match const req.header(\"Host\") { case .timeout: return 200 case "
        "_: return 500 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_variant_payload_type_mismatch) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(\"x\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend,
     analyze_rejects_generic_variant_constructor_when_not_all_type_arguments_can_be_inferred) {
    const auto src = R"rut(
variant Pair<T, U> { one(T), two(U) }
route GET "/users" {
    let state = Pair.one(200)
    match state {
    case .one(v): return 200
    case .two(v): return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    CHECK(!hir);
}
TEST(frontend,
     analyze_rejects_generic_struct_constructor_when_not_all_type_arguments_can_be_inferred) {
    const auto src = R"rut(
struct Holder<T, U> { value: T, tag: i32 }
route GET "/users" {
    let box = Holder(value: 200, tag: 1)
    if box.tag == 1 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_missing_variant_payload) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_payload_on_payloadless_variant_case) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.err(1) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_variant_match_missing_case_without_wildcard) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "route GET \"/users\" { let state = AuthState.timeout match state { case .timeout: return "
        "200 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_duplicate_variant_match_case) {
    const char* src =
        "variant AuthState { timeout, forbidden }\n"
        "route GET \"/users\" { let state = AuthState.timeout match state { case .timeout: return "
        "200 case .timeout: return 403 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, lower_forward_route_to_rir) {
    const char* src = "upstream api\nroute GET \"/users\" { forward api }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->upstreams.len, 1u);
    REQUIRE_EQ(hir->routes.len, 1u);
    CHECK_EQ(hir->upstreams[0].id, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.direct_term.kind),
             static_cast<u8>(HirTerminatorKind::ForwardUpstream));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions.len, 1u);
    REQUIRE_EQ(mir->functions[0].blocks.len, 1u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::ForwardUpstream));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.func_count, 1u);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 1u);
    REQUIRE_EQ(fn.blocks[0].inst_count, 2u);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ConstI32));
    CHECK_EQ(fn.blocks[0].insts[0].imm.i32_val, 1);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::RetForward));
    rir.destroy();
}
TEST(frontend, let_if_lowers_to_branching_rir) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let ok = true if ok { return 200 } else { forward api } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[1].route.statements.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[1].route.statements[0].kind),
             static_cast<u8>(AstStmtKind::Let));
    CHECK_EQ(static_cast<u8>(ast->items[1].route.statements[1].kind),
             static_cast<u8>(AstStmtKind::If));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::If));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    REQUIRE_EQ(mir->functions[0].blocks.len, 3u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.func_count, 1u);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    CHECK_EQ(fn.blocks[0].inst_count, 2u);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ConstBool));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::Br));
    CHECK_EQ(static_cast<u8>(fn.blocks[1].insts[0].op), static_cast<u8>(rir::Opcode::RetStatus));
    CHECK_EQ(static_cast<u8>(fn.blocks[2].insts[1].op), static_cast<u8>(rir::Opcode::RetForward));
    rir.destroy();
}
TEST(frontend, route_if_branch_block_with_let_lowers_via_match_shape) {
    const char* src =
        "route GET \"/users\" { let ok = true if ok { let code = 200 if code == 200 { return 200 } "
        "else { return 500 } } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.match_arms[0].body_kind),
             static_cast<u8>(HirMatchArm::BodyKind::If));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE(mir->functions[0].blocks.len >= 5u);
}
TEST(frontend, route_if_branch_block_with_guard_is_supported) {
    const char* src =
        "route GET \"/users\" { let ok = true if ok { let failed = error(7) guard failed else { "
        "return 401 } return 200 } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    REQUIRE_EQ(hir->routes[0].control.match_arms[0].guards.len, 1u);
}
TEST(frontend, guard_lowers_to_fail_and_continue_blocks) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let failed = error(7) guard failed else { return 401 } forward api "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[1].route.statements.len, 3u);
    CHECK_EQ(static_cast<u8>(ast->items[1].route.statements[1].kind),
             static_cast<u8>(AstStmtKind::Guard));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].guards.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Direct));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].blocks.len, 3u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK_FALSE(mir->functions[0].blocks[0].term.cond.bool_value);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[1].term.kind),
             static_cast<u8>(MirTerminatorKind::ForwardUpstream));
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[2].term.kind),
             static_cast<u8>(MirTerminatorKind::ReturnStatus));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    REQUIRE(fn.blocks[0].inst_count > 0);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::Br));
    CHECK_EQ(static_cast<u8>(fn.blocks[1].insts[1].op), static_cast<u8>(rir::Opcode::RetForward));
    CHECK_EQ(fn.blocks[2].insts[0].imm.i32_val, 401);
    rir.destroy();
}
TEST(frontend, guard_else_block_with_let_is_supported) {
    const char* src =
        "route GET \"/users\" { let failed = error(7) guard failed else { let code = 401 if code "
        "== 401 { return 401 } else { return 500 } } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].guards.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].guards[0].fail_kind),
             static_cast<u8>(HirGuard::FailKind::Body));
    CHECK_EQ(static_cast<u8>(hir->routes[0].guards[0].fail_body.body_kind),
             static_cast<u8>(HirGuardBody::BodyKind::If));
}
TEST(frontend, guard_let_binds_success_value) {
    const char* src =
        "route GET \"/users\" { guard let code = 200 else { return 401 } if code == 200 { return "
        "200 } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].route.statements[0].kind),
             static_cast<u8>(AstStmtKind::Guard));
    CHECK(ast->items[0].route.statements[0].bind_value);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].name.eq(lit("code")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::If));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    REQUIRE_EQ(mir->functions[0].blocks.len, 5u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[1].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
}
TEST(frontend, guard_let_does_not_unwrap_nil) {
    const char* src =
        "route GET \"/users\" { guard let maybe = nil else { return 401 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].name.eq(lit("maybe")));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::Unknown));
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].guards[0].cond.kind),
             static_cast<u8>(HirExprKind::BoolLit));
    CHECK(hir->routes[0].guards[0].cond.bool_value);
}
TEST(frontend, equality_expression_lowers_to_cmp_eq) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let code = 200 if code == 200 { forward api } else { return 404 } "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::I32));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[0].type), static_cast<u8>(MirTypeKind::I32));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ConstI32));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::Br));
    rir.destroy();
}
TEST(frontend, or_builtin_falls_back_from_nil) {
    const char* src =
        "route GET \"/users\" { let code = or(nil, 200) if code == 200 { return 200 } else { "
        "return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[0].route.statements[0].kind),
             static_cast<u8>(AstStmtKind::Let));
    CHECK_EQ(static_cast<u8>(ast->items[0].route.statements[0].expr.kind),
             static_cast<u8>(AstExprKind::Or));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir->routes[0].locals[0].init.int_value, 200);
}
TEST(frontend, req_header_flows_as_optional_str) {
    const char* src =
        "route GET \"/users\" { let host = req.header(\"Host\") let value = or(host, \"fallback\") "
        "return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 3u);
    CHECK_EQ(static_cast<u8>(ast->items[0].route.statements[0].expr.kind),
             static_cast<u8>(AstExprKind::ReqHeader));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::Str));
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::ReqHeader));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].type), static_cast<u8>(HirTypeKind::Str));
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[0].init.kind),
             static_cast<u8>(MirValueKind::ReqHeader));
    REQUIRE(mir->functions[0].locals[1].init.lhs != nullptr);
    REQUIRE(mir->functions[0].locals[1].init.rhs != nullptr);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[1].init.kind),
             static_cast<u8>(MirValueKind::Or));
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[1].init.lhs->kind),
             static_cast<u8>(MirValueKind::LocalRef));
    CHECK_EQ(mir->functions[0].locals[1].init.lhs->local_index, 0u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[1].init.rhs->kind),
             static_cast<u8>(MirValueKind::StrConst));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ReqHeader));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::ConstStr));
    rir.destroy();
}
TEST(frontend, req_header_alias_flows_as_optional_str) {
    const char* src =
        "route GET \"/users\" { let host = req.header(\"Host\") let alias = host let value = "
        "or(alias, \"fallback\") return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 3u);
    CHECK(hir->routes[0].locals[1].may_nil);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::LocalRef));
    CHECK_EQ(hir->routes[0].locals[1].init.local_index, 0u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[2].type), static_cast<u8>(HirTypeKind::Str));
    CHECK_FALSE(hir->routes[0].locals[2].may_nil);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 3u);
    REQUIRE(mir->functions[0].locals[2].init.lhs != nullptr);
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[2].init.kind),
             static_cast<u8>(MirValueKind::Or));
    CHECK_EQ(static_cast<u8>(mir->functions[0].locals[2].init.lhs->kind),
             static_cast<u8>(MirValueKind::LocalRef));
    CHECK_EQ(mir->functions[0].locals[2].init.lhs->local_index, 1u);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ReqHeader));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::ConstStr));
    rir.destroy();
}
TEST(frontend, or_builtin_falls_back_from_nil_local) {
    const char* src =
        "route GET \"/users\" { let maybe = nil let code = or(maybe, 200) if code == 200 { return "
        "200 } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 3u);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].type), static_cast<u8>(HirTypeKind::Unknown));
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::Nil));
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir->routes[0].locals[1].init.int_value, 200);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].locals.len, 2u);
}
TEST(frontend, or_builtin_falls_back_from_error_local) {
    const char* src =
        "route GET \"/users\" { let failed = error(7) let code = or(failed, 200) if code == 200 { "
        "return 200 } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].route.statements.len, 3u);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[0].init.kind),
             static_cast<u8>(HirExprKind::Error));
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[1].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir->routes[0].locals[1].init.int_value, 200);
}
TEST(frontend, or_builtin_falls_back_from_error_alias) {
    const char* src =
        "route GET \"/users\" { let failed = error(7) let alias = failed let code = or(alias, 200) "
        "if code == 200 { return 200 } else { return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 3u);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[2].type), static_cast<u8>(HirTypeKind::I32));
    CHECK_FALSE(hir->routes[0].locals[2].may_nil);
    CHECK_FALSE(hir->routes[0].locals[2].may_error);
    CHECK_EQ(static_cast<u8>(hir->routes[0].locals[2].init.kind),
             static_cast<u8>(HirExprKind::IntLit));
    CHECK_EQ(hir->routes[0].locals[2].init.int_value, 200);
}
TEST(frontend, analyze_rejects_unknown_upstream) {
    const char* src = "route GET \"/users\" { forward api }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnknownUpstream));
}
TEST(frontend, match_lowers_to_cmp_eq_chain) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let code = 200 match code { case 200: forward api case _: return "
        "404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[1].route.statements.len, 2u);
    CHECK_EQ(static_cast<u8>(ast->items[1].route.statements[1].kind),
             static_cast<u8>(AstStmtKind::Match));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    REQUIRE_EQ(hir->routes[0].control.match_arms.len, 2u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].blocks.len, 3u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK(mir->functions[0].blocks[0].term.use_cmp);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[2].op), static_cast<u8>(rir::Opcode::CmpEq));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[3].op), static_cast<u8>(rir::Opcode::Br));
    CHECK_EQ(static_cast<u8>(fn.blocks[1].insts[1].op), static_cast<u8>(rir::Opcode::RetForward));
    CHECK_EQ(static_cast<u8>(fn.blocks[2].insts[0].op), static_cast<u8>(rir::Opcode::RetStatus));
    rir.destroy();
}
TEST(frontend, guard_then_match_lowers_to_guard_and_match_blocks) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let failed = error(7) let code = 200 guard code else { return 401 "
        "} match code { case 200: forward api case _: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK_EQ(static_cast<u8>(hir->routes[0].control.kind), static_cast<u8>(HirControlKind::Match));
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].blocks.len, 5u);
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK_EQ(static_cast<u8>(mir->functions[0].blocks[1].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK(mir->functions[0].blocks[1].term.use_cmp);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.functions[0].block_count, 5u);
    rir.destroy();
}
TEST(frontend, multiple_top_level_guards_are_allowed) {
    const char* src =
        "route GET \"/users\" { let ok = 200 guard ok else { return 401 } let failed = error(7) "
        "guard failed else { return 402 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].guards.len, 2u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].blocks.len, 5u);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.functions[0].block_count, 5u);
    rir.destroy();
}
TEST(frontend, guard_match_lowers_to_fail_side_match_arms) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout) guard match failed else { case "
        ".timeout: return 503 case _: return 500 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].guards.len, 1u);
    REQUIRE_EQ(hir->guard_match_arms.len, 2u);
    const auto& guard = hir->routes[0].guards[0];
    CHECK_EQ(static_cast<u8>(guard.fail_kind), static_cast<u8>(HirGuard::FailKind::Match));
    REQUIRE_EQ(guard.fail_match_count, 2u);
    CHECK_EQ(guard.fail_match_start, 0u);
    CHECK(!hir->guard_match_arms[0].is_wildcard);
    CHECK(hir->guard_match_arms[1].is_wildcard);
    CHECK_EQ(hir->guard_match_arms[0].direct_term.status_code, 503);
    CHECK_EQ(hir->guard_match_arms[1].direct_term.status_code, 500);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    CHECK(mir->functions[0].blocks.len >= 5u);
}
TEST(frontend, analyze_rejects_guard_match_without_wildcard) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout) guard match failed else { case "
        ".timeout: return 503 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_guard_match_wildcard_before_last) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout) guard match failed else { case _: "
        "return 500 case .timeout: return 503 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_guard_match_duplicate_case) {
    const char* src =
        "route GET \"/users\" { let failed = error(.timeout) guard match failed else { case "
        ".timeout: return 503 case .timeout: return 504 case _: return 500 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_guard_match_pattern_type_mismatch) {
    const char* src =
        "variant AuthError { timeout, forbidden }\n"
        "route GET \"/users\" { let failed = error(.timeout) guard match failed else { case "
        "AuthError.timeout: return 503 case _: return 500 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_match_arm_block_guard_match_on_non_error_value) {
    const char* src =
        "variant Result { ok(i32), err }\n"
        "route GET \"/users\" { let state = Result.ok(200) match state { case .ok(x): { guard "
        "match x else { case .timeout: return 401 case _: return 402 } if x == 200 { return 200 } "
        "else { return 500 } } case .err: return 404 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_match_without_wildcard) {
    const char* src =
        "route GET \"/users\" { let code = 200 match code { case 200: return 200 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, guard_lowers_from_error_alias) {
    const char* src =
        "upstream api\n"
        "route GET \"/users\" { let failed = error(7) let alias = failed guard alias else { return "
        "401 } forward api }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].guards.len, 1u);
    CHECK_FALSE(hir->routes[0].guards[0].cond.bool_value);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    REQUIRE_EQ(mir->functions[0].blocks.len, 3u);
}
TEST(frontend, analyze_rejects_match_wildcard_before_last) {
    const char* src =
        "route GET \"/users\" { let code = 200 match code { case _: return 404 case 200: return "
        "200 } }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_match_pattern_type_mismatch) {
    const char* src =
        "route GET \"/users\" { let ok = true match ok { case 200: return 200 case _: return 404 } "
        "}\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_or_with_mismatched_types) {
    const char* src =
        "route GET \"/users\" { let code = 200 let fallback = or(code, true) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_or_with_fallible_fallback) {
    const char* src = "route GET \"/users\" { let code = or(nil, error(7)) return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_guard_let_binding_error_value) {
    const char* src =
        "route GET \"/users\" { guard let code = error(7) else { return 401 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_guard_let_binding_error_alias) {
    const char* src =
        "route GET \"/users\" { let failed = error(7) let alias = failed guard let code = alias "
        "else { return 401 } return 200 }\n";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, build_mir_preserves_runtime_or_value) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal maybe{};
    maybe.span = {1, 1, 1, 1};
    maybe.name = lit("maybe");
    maybe.type = HirTypeKind::I32;
    maybe.may_nil = true;
    maybe.init.kind = HirExprKind::IntLit;
    maybe.init.type = HirTypeKind::I32;
    maybe.init.int_value = 123;
    REQUIRE(route.locals.push(maybe));
    HirLocal code{};
    code.span = {1, 1, 1, 1};
    code.name = lit("code");
    code.type = HirTypeKind::I32;
    code.init.kind = HirExprKind::Or;
    code.init.type = HirTypeKind::I32;
    HirExpr lhs{};
    lhs.kind = HirExprKind::LocalRef;
    lhs.type = HirTypeKind::I32;
    lhs.may_nil = true;
    lhs.may_error = false;
    lhs.local_index = 0;
    REQUIRE(route.exprs.push(lhs));
    HirExpr rhs{};
    rhs.kind = HirExprKind::IntLit;
    rhs.type = HirTypeKind::I32;
    rhs.int_value = 200;
    REQUIRE(route.exprs.push(rhs));
    code.init.lhs = &route.exprs[0];
    code.init.rhs = &route.exprs[1];
    REQUIRE(route.locals.push(code));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    REQUIRE_EQ(mir.value()->functions.len, 1u);
    REQUIRE_EQ(mir.value()->functions[0].locals.len, 2u);
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].locals[1].init.kind),
             static_cast<u8>(MirValueKind::Or));
    REQUIRE_NE(mir.value()->functions[0].locals[1].init.lhs, nullptr);
    REQUIRE_NE(mir.value()->functions[0].locals[1].init.rhs, nullptr);
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].locals[1].init.lhs->kind),
             static_cast<u8>(MirValueKind::LocalRef));
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].locals[1].init.rhs->kind),
             static_cast<u8>(MirValueKind::IntConst));
}
TEST(frontend, build_mir_preserves_runtime_no_error_guard) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal value{};
    value.span = {1, 1, 1, 1};
    value.name = lit("value");
    value.type = HirTypeKind::I32;
    value.may_error = true;
    value.init.kind = HirExprKind::IntLit;
    value.init.type = HirTypeKind::I32;
    value.init.int_value = 7;
    REQUIRE(route.locals.push(value));
    HirGuard guard{};
    guard.span = {1, 1, 1, 1};
    guard.cond.kind = HirExprKind::NoError;
    guard.cond.type = HirTypeKind::Bool;
    HirExpr ref{};
    ref.kind = HirExprKind::LocalRef;
    ref.type = HirTypeKind::I32;
    ref.may_error = true;
    ref.local_index = 0;
    REQUIRE(route.exprs.push(ref));
    guard.cond.lhs = &route.exprs[0];
    guard.fail_term.kind = HirTerminatorKind::ReturnStatus;
    guard.fail_term.status_code = 401;
    REQUIRE(route.guards.push(guard));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    REQUIRE_EQ(mir.value()->functions.len, 1u);
    REQUIRE_EQ(mir.value()->functions[0].blocks.len, 3u);
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].blocks[0].term.kind),
             static_cast<u8>(MirTerminatorKind::Branch));
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].blocks[0].term.cond.kind),
             static_cast<u8>(MirValueKind::NoError));
    REQUIRE_NE(mir.value()->functions[0].blocks[0].term.cond.lhs, nullptr);
    CHECK_EQ(static_cast<u8>(mir.value()->functions[0].blocks[0].term.cond.lhs->kind),
             static_cast<u8>(MirValueKind::LocalRef));
}
TEST(frontend, lower_to_rir_supports_runtime_optional_or_value) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal maybe{};
    maybe.span = {1, 1, 1, 1};
    maybe.name = lit("maybe");
    maybe.type = HirTypeKind::I32;
    maybe.may_nil = true;
    maybe.init.kind = HirExprKind::IntLit;
    maybe.init.type = HirTypeKind::I32;
    maybe.init.int_value = 123;
    REQUIRE(route.locals.push(maybe));
    HirLocal code{};
    code.span = {1, 1, 1, 1};
    code.name = lit("code");
    code.type = HirTypeKind::I32;
    code.init.kind = HirExprKind::Or;
    code.init.type = HirTypeKind::I32;
    HirExpr lhs{};
    lhs.kind = HirExprKind::LocalRef;
    lhs.type = HirTypeKind::I32;
    lhs.may_nil = true;
    lhs.may_error = false;
    lhs.local_index = 0;
    REQUIRE(route.exprs.push(lhs));
    HirExpr rhs{};
    rhs.kind = HirExprKind::IntLit;
    rhs.type = HirTypeKind::I32;
    rhs.int_value = 200;
    REQUIRE(route.exprs.push(rhs));
    code.init.lhs = &route.exprs[0];
    code.init.rhs = &route.exprs[1];
    REQUIRE(route.locals.push(code));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.func_count, 1u);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 1u);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ConstI32));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::OptWrap));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[2].op), static_cast<u8>(rir::Opcode::ConstI32));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[3].op), static_cast<u8>(rir::Opcode::OptIsNil));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[4].op), static_cast<u8>(rir::Opcode::OptUnwrap));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[5].op), static_cast<u8>(rir::Opcode::Select));
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[6].op), static_cast<u8>(rir::Opcode::RetStatus));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_error_or_value) {
    auto* mir = new MirModule{};
    MirFunction fn{};
    fn.span = Span{0, 0, 1, 1};
    fn.method = 'G';
    fn.path = lit("/users");
    fn.name = lit("route");
    MirLocal failed{};
    failed.span = fn.span;
    failed.name = lit("failed");
    failed.type = MirTypeKind::I32;
    failed.may_error = true;
    failed.init.kind = MirValueKind::Error;
    failed.init.type = MirTypeKind::Unknown;
    failed.init.may_error = true;
    failed.init.int_value = 7;
    REQUIRE(fn.locals.push(failed));
    MirValue lhs{};
    lhs.kind = MirValueKind::LocalRef;
    lhs.type = MirTypeKind::I32;
    lhs.may_error = true;
    lhs.local_index = 0;
    MirValue rhs{};
    rhs.kind = MirValueKind::IntConst;
    rhs.type = MirTypeKind::I32;
    rhs.int_value = 200;
    MirValue orv{};
    orv.kind = MirValueKind::Or;
    orv.type = MirTypeKind::I32;
    orv.may_error = true;
    orv.lhs = &lhs;
    orv.rhs = &rhs;
    REQUIRE(fn.values.push(lhs));
    REQUIRE(fn.values.push(rhs));
    REQUIRE(fn.values.push(orv));
    MirLocal code{};
    code.span = fn.span;
    code.name = lit("code");
    code.type = MirTypeKind::I32;
    code.init = fn.values[2];
    REQUIRE(fn.locals.push(code));
    MirBlock entry{};
    entry.label = lit("entry");
    entry.term.kind = MirTerminatorKind::ReturnStatus;
    entry.term.span = fn.span;
    entry.term.status_code = 200;
    REQUIRE(fn.blocks.push(entry));
    REQUIRE(mir->functions.push(fn));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir, rir);
    REQUIRE(lowered);
    const auto& out_fn = rir.module.functions[0];
    REQUIRE_EQ(out_fn.block_count, 1u);
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::StructCreate));
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::StructField));
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::OptIsNil));
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::OptUnwrap));
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::Select));
    REQUIRE(out_fn.blocks[0].inst_count > 0);
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[out_fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::RetStatus));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_optional_error_or_value) {
    auto* mir = new MirModule{};
    MirFunction fn{};
    fn.span = Span{0, 0, 1, 1};
    fn.method = 'G';
    fn.path = lit("/users");
    fn.name = lit("route");
    MirLocal maybe_failed{};
    maybe_failed.span = fn.span;
    maybe_failed.name = lit("maybe_failed");
    maybe_failed.type = MirTypeKind::I32;
    maybe_failed.may_nil = true;
    maybe_failed.may_error = true;
    maybe_failed.init.kind = MirValueKind::Nil;
    maybe_failed.init.type = MirTypeKind::Unknown;
    maybe_failed.init.may_nil = true;
    REQUIRE(fn.locals.push(maybe_failed));
    MirValue lhs{};
    lhs.kind = MirValueKind::LocalRef;
    lhs.type = MirTypeKind::I32;
    lhs.may_nil = true;
    lhs.may_error = true;
    lhs.local_index = 0;
    MirValue rhs{};
    rhs.kind = MirValueKind::IntConst;
    rhs.type = MirTypeKind::I32;
    rhs.int_value = 200;
    MirValue orv{};
    orv.kind = MirValueKind::Or;
    orv.type = MirTypeKind::I32;
    orv.may_nil = true;
    orv.may_error = true;
    orv.lhs = &lhs;
    orv.rhs = &rhs;
    REQUIRE(fn.values.push(lhs));
    REQUIRE(fn.values.push(rhs));
    REQUIRE(fn.values.push(orv));
    MirLocal code{};
    code.span = fn.span;
    code.name = lit("code");
    code.type = MirTypeKind::I32;
    code.init = fn.values[2];
    REQUIRE(fn.locals.push(code));
    MirBlock entry{};
    entry.label = lit("entry");
    entry.term.kind = MirTerminatorKind::ReturnStatus;
    entry.term.span = fn.span;
    entry.term.status_code = 200;
    REQUIRE(fn.blocks.push(entry));
    REQUIRE(mir->functions.push(fn));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir, rir);
    REQUIRE(lowered);
    const auto& out_fn = rir.module.functions[0];
    REQUIRE_EQ(out_fn.block_count, 1u);
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::StructCreate));
    CHECK(block_op_count(out_fn.blocks[0], rir::Opcode::OptIsNil) >= 2u);
    CHECK(block_has_op(out_fn.blocks[0], rir::Opcode::OptUnwrap));
    CHECK(block_op_count(out_fn.blocks[0], rir::Opcode::Select) >= 2u);
    REQUIRE(out_fn.blocks[0].inst_count > 0);
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[out_fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::RetStatus));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_optional_str_or_value) {
    auto* mir = new MirModule{};
    MirFunction fn{};
    fn.span = Span{0, 0, 1, 1};
    fn.method = 'G';
    fn.path = lit("/users");
    fn.name = lit("route");
    MirLocal maybe{};
    maybe.span = fn.span;
    maybe.name = lit("maybe");
    maybe.type = MirTypeKind::Str;
    maybe.may_nil = true;
    maybe.init.kind = MirValueKind::StrConst;
    maybe.init.type = MirTypeKind::Str;
    maybe.init.str_value = lit("api");
    REQUIRE(fn.locals.push(maybe));
    MirBlock entry{};
    entry.label = lit("entry");
    entry.term.kind = MirTerminatorKind::ReturnStatus;
    entry.term.span = fn.span;
    entry.term.status_code = 200;
    REQUIRE(fn.blocks.push(entry));
    MirValue lhs{};
    lhs.kind = MirValueKind::LocalRef;
    lhs.type = MirTypeKind::Str;
    lhs.may_nil = true;
    lhs.local_index = 0;
    MirValue rhs{};
    rhs.kind = MirValueKind::StrConst;
    rhs.type = MirTypeKind::Str;
    rhs.str_value = lit("fallback");
    MirValue orv{};
    orv.kind = MirValueKind::Or;
    orv.type = MirTypeKind::Str;
    orv.may_nil = false;
    orv.lhs = &lhs;
    orv.rhs = &rhs;
    REQUIRE(fn.values.push(lhs));
    REQUIRE(fn.values.push(rhs));
    REQUIRE(fn.values.push(orv));
    MirLocal code{};
    code.span = fn.span;
    code.name = lit("code");
    code.type = MirTypeKind::Str;
    code.init = fn.values[2];
    REQUIRE(fn.locals.push(code));
    REQUIRE(mir->functions.push(fn));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir, rir);
    REQUIRE(lowered);
    const auto& out_fn = rir.module.functions[0];
    REQUIRE_EQ(out_fn.block_count, 1u);
    REQUIRE_EQ(out_fn.blocks[0].inst_count, 7u);
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[0].op), static_cast<u8>(rir::Opcode::ConstStr));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[1].op), static_cast<u8>(rir::Opcode::OptWrap));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[2].op), static_cast<u8>(rir::Opcode::ConstStr));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[3].op), static_cast<u8>(rir::Opcode::OptIsNil));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[4].op),
             static_cast<u8>(rir::Opcode::OptUnwrap));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[5].op), static_cast<u8>(rir::Opcode::Select));
    CHECK_EQ(static_cast<u8>(out_fn.blocks[0].insts[6].op),
             static_cast<u8>(rir::Opcode::RetStatus));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_no_error_guard) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal value{};
    value.span = {1, 1, 1, 1};
    value.name = lit("value");
    value.type = HirTypeKind::I32;
    value.may_error = true;
    value.init.kind = HirExprKind::IntLit;
    value.init.type = HirTypeKind::I32;
    value.init.int_value = 7;
    REQUIRE(route.locals.push(value));
    HirGuard guard{};
    guard.span = {1, 1, 1, 1};
    guard.cond.kind = HirExprKind::NoError;
    guard.cond.type = HirTypeKind::Bool;
    HirExpr ref{};
    ref.kind = HirExprKind::LocalRef;
    ref.type = HirTypeKind::I32;
    ref.may_error = true;
    ref.local_index = 0;
    REQUIRE(route.exprs.push(ref));
    guard.cond.lhs = &route.exprs[0];
    guard.fail_term.kind = HirTerminatorKind::ReturnStatus;
    guard.fail_term.status_code = 401;
    REQUIRE(route.guards.push(guard));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir.value(), rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.func_count, 1u);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructCreate));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructField));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::OptIsNil));
    REQUIRE(fn.blocks[0].inst_count > 0);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::Br));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_no_error_guard_on_optional_error) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal maybe_failed{};
    maybe_failed.span = route.span;
    maybe_failed.name = lit("maybe_failed");
    maybe_failed.type = HirTypeKind::I32;
    maybe_failed.may_nil = true;
    maybe_failed.may_error = true;
    maybe_failed.init.kind = HirExprKind::Nil;
    maybe_failed.init.type = HirTypeKind::Unknown;
    maybe_failed.init.may_nil = true;
    REQUIRE(route.locals.push(maybe_failed));
    HirExpr subject{};
    subject.kind = HirExprKind::LocalRef;
    subject.type = HirTypeKind::I32;
    subject.may_nil = true;
    subject.may_error = true;
    subject.local_index = 0;
    REQUIRE(route.exprs.push(subject));
    HirGuard guard{};
    guard.span = route.span;
    guard.cond.kind = HirExprKind::NoError;
    guard.cond.type = HirTypeKind::Bool;
    guard.cond.lhs = &route.exprs[0];
    guard.fail_term.kind = HirTerminatorKind::ReturnStatus;
    guard.fail_term.status_code = 401;
    REQUIRE(route.guards.push(guard));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    REQUIRE_EQ(fn.block_count, 3u);
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructCreate));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructField));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::OptIsNil));
    REQUIRE(fn.blocks[0].inst_count > 0);
    CHECK_EQ(static_cast<u8>(fn.blocks[0].insts[fn.blocks[0].inst_count - 1].op),
             static_cast<u8>(rir::Opcode::Br));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_error_code_field) {
    auto* hir = new HirModule{};
    HirRoute route{};
    route.span = {1, 1, 1, 1};
    route.method = 'G';
    route.path = lit("/users");
    HirLocal failed{};
    failed.span = route.span;
    failed.name = lit("failed");
    failed.type = HirTypeKind::I32;
    failed.may_error = true;
    failed.init.kind = HirExprKind::Error;
    failed.init.type = HirTypeKind::Unknown;
    failed.init.may_error = true;
    failed.init.int_value = 7;
    failed.init.msg = lit("boom");
    REQUIRE(route.locals.push(failed));
    HirExpr failed_ref{};
    failed_ref.kind = HirExprKind::LocalRef;
    failed_ref.type = HirTypeKind::I32;
    failed_ref.may_error = true;
    failed_ref.local_index = 0;
    REQUIRE(route.exprs.push(failed_ref));
    HirLocal code{};
    code.span = route.span;
    code.name = lit("code");
    code.type = HirTypeKind::I32;
    code.init.kind = HirExprKind::Field;
    code.init.type = HirTypeKind::I32;
    code.init.lhs = &route.exprs[0];
    code.init.str_value = lit("code");
    REQUIRE(route.locals.push(code));
    route.control.kind = HirControlKind::Direct;
    route.control.direct_term.kind = HirTerminatorKind::ReturnStatus;
    route.control.direct_term.status_code = 200;
    REQUIRE(hir->routes.push(route));
    auto mir = build_mir(*hir);
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir.value(), rir);
    REQUIRE(lowered);
    const auto& fn = rir.module.functions[0];
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::StructField));
    CHECK(block_has_op(fn.blocks[0], rir::Opcode::OptUnwrap));
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_runtime_error_kind_match) {
    auto* mir = new MirModule{};
    MirVariant err_variant{};
    err_variant.span = {1, 1, 1, 1};
    err_variant.name = lit("AuthError");
    MirVariant::CaseDecl timeout{};
    timeout.name = lit("timeout");
    MirVariant::CaseDecl forbidden{};
    forbidden.name = lit("forbidden");
    REQUIRE(err_variant.cases.push(timeout));
    REQUIRE(err_variant.cases.push(forbidden));
    REQUIRE(mir->variants.push(err_variant));
    MirFunction fn{};
    fn.span = Span{1, 1, 1, 1};
    fn.method = 'G';
    fn.path = lit("/users");
    fn.name = lit("route");
    MirLocal failed{};
    failed.span = fn.span;
    failed.name = lit("failed");
    failed.type = MirTypeKind::I32;
    failed.may_error = true;
    failed.error_variant_index = 0;
    failed.init.kind = MirValueKind::Error;
    failed.init.type = MirTypeKind::Unknown;
    failed.init.may_error = true;
    failed.init.error_variant_index = 0;
    failed.init.error_case_index = 1;
    REQUIRE(fn.locals.push(failed));
    MirValue subject{};
    subject.kind = MirValueKind::LocalRef;
    subject.type = MirTypeKind::I32;
    subject.may_error = true;
    subject.local_index = 0;
    subject.error_variant_index = 0;
    REQUIRE(fn.values.push(subject));
    fn.blocks.len = 0;
    MirBlock test{};
    test.label = lit("match_test");
    test.term.kind = MirTerminatorKind::Branch;
    test.term.use_cmp = true;
    test.term.span = fn.span;
    test.term.lhs = fn.values[0];
    MirValue timeout_pat{};
    timeout_pat.kind = MirValueKind::VariantCase;
    timeout_pat.type = MirTypeKind::Variant;
    timeout_pat.variant_index = 0;
    timeout_pat.case_index = 0;
    timeout_pat.int_value = 0;
    REQUIRE(fn.values.push(timeout_pat));
    test.term.rhs = fn.values[1];
    test.term.then_block = 1;
    test.term.else_block = 2;
    REQUIRE(fn.blocks.push(test));
    MirBlock then_block{};
    then_block.label = lit("then");
    then_block.term.kind = MirTerminatorKind::ReturnStatus;
    then_block.term.span = fn.span;
    then_block.term.status_code = 503;
    REQUIRE(fn.blocks.push(then_block));
    MirBlock else_block{};
    else_block.label = lit("else");
    else_block.term.kind = MirTerminatorKind::ReturnStatus;
    else_block.term.span = fn.span;
    else_block.term.status_code = 403;
    REQUIRE(fn.blocks.push(else_block));
    REQUIRE(mir->functions.push(fn));
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(*mir, rir);
    REQUIRE(lowered);
    REQUIRE_EQ(rir.module.func_count, 1u);
    const auto& out_fn = rir.module.functions[0];
    REQUIRE_EQ(out_fn.block_count, 3u);
    bool saw_struct_create = false;
    bool saw_struct_field = false;
    bool saw_opt_unwrap = false;
    for (u32 bi = 0; bi < out_fn.block_count; bi++) {
        for (u32 ii = 0; ii < out_fn.blocks[bi].inst_count; ii++) {
            saw_struct_create |= out_fn.blocks[bi].insts[ii].op == rir::Opcode::StructCreate;
            saw_struct_field |= out_fn.blocks[bi].insts[ii].op == rir::Opcode::StructField;
            saw_opt_unwrap |= out_fn.blocks[bi].insts[ii].op == rir::Opcode::OptUnwrap;
        }
    }
    CHECK(saw_struct_create);
    CHECK(saw_struct_field);
    CHECK(saw_opt_unwrap);
    rir.destroy();
}
TEST(frontend, source_function_call_inlines_i32_expression_body) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = id(200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    REQUIRE_EQ(hir->routes.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_function_call_inlines_i32_expression_body_without_return_annotation) {
    const auto src = R"(
func id(x: i32) => x
route GET "/users" {
    let code = id(200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_generic_function_inlines_i32_expression_body) {
    const auto src = R"rut(
func id<T>(x: T) -> T => x
route GET "/users" {
    let code = id(200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].type_params.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::Generic);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_generic_function_reuses_same_type_parameter_shape) {
    const auto src = R"rut(
func first<T>(x: T, y: T) -> T => x
route GET "/users" {
    let code = first(200, 500)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_generic_function_accepts_explicit_type_arguments) {
    const auto src = R"rut(
func id<T>(x: T) -> T => x
route GET "/users" {
    let code = id<i32>(200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_generic_function_supports_nested_generic_param_and_return_shapes) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
struct Holder<T> { state: Result<T> }
func unwrap<T>(x: Holder<T>) -> Result<T> => x.state
route GET "/users" {
    let state = unwrap(Holder(state: Result.ok(200)))
    match state {
    case .ok(v): return 200
    case .err: return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
    }
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].type_params.len, 1u);
    CHECK_EQ(hir->functions[0].params[0].type, HirTypeKind::Struct);
    CHECK_EQ(hir->functions[0].return_type, HirTypeKind::Variant);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK_EQ(hir->routes[0].locals[0].type, HirTypeKind::Variant);
}
TEST(frontend, protocol_method_requirements_are_recorded_in_hir) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->protocols.len, 4u);
    REQUIRE_EQ(hir->protocols[3].methods.len, 1u);
    CHECK(hir->protocols[3].methods[0].name.eq(lit("hash")));
    CHECK_EQ(hir->protocols[3].methods[0].params.len, 0u);
    CHECK(hir->protocols[3].methods[0].return_type_name.eq(lit("i32")));
}
TEST(frontend, protocol_method_requirement_shapes_are_recorded_in_hir) {
    const auto src = R"rut(
protocol Boxed {
    func wrap(x: i32) -> i32
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE(hir->protocols.len >= 1u);
    const auto& proto = hir->protocols[hir->protocols.len - 1];
    CHECK(proto.name.eq(lit("Boxed")));
    REQUIRE_EQ(proto.methods.len, 1u);
    const auto& method = proto.methods[0];
    REQUIRE_EQ(method.params.len, 1u);
    CHECK_EQ(method.params[0].type, HirTypeKind::I32);
    CHECK(method.params[0].shape_index != 0xffffffffu);
    CHECK_EQ(method.return_type, HirTypeKind::I32);
    CHECK(method.return_shape_index != 0xffffffffu);
}
TEST(frontend, generic_protocol_constraint_survives_if_merge_in_hir) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
func pick<T: Hashable>(x: T, ok: bool) -> T {
    if ok { x } else { x }
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    CHECK_EQ(static_cast<u8>(body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_EQ(body.type, HirTypeKind::Generic);
    CHECK_EQ(body.generic_protocol_count, 1u);
    CHECK_NE(body.generic_protocol_indices[0], 0xffffffffu);
}
TEST(frontend, generic_protocol_constraint_survives_match_payload_binding_in_hir) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
variant Wrap<T> { some(T) }
func unwrap<T: Hashable>(state: Wrap<T>) -> T {
    match state {
    case .some(v) => v
    }
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    CHECK_EQ(body.type, HirTypeKind::Generic);
    CHECK_EQ(body.generic_protocol_count, 1u);
    CHECK_NE(body.generic_protocol_indices[0], 0xffffffffu);
}
TEST(frontend, generic_protocol_constraint_survives_struct_field_projection_in_hir) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
struct Holder<T> { state: T }
func unwrap<T: Hashable>(x: Holder<T>) -> i32 => x.state.hash()
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    REQUIRE(body.lhs != nullptr);
    CHECK_EQ(body.lhs->type, HirTypeKind::Generic);
    CHECK_EQ(body.lhs->generic_protocol_count, 1u);
    CHECK_NE(body.lhs->generic_protocol_indices[0], 0xffffffffu);
}

TEST(frontend, source_struct_field_projection_supports_method_dispatch) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable { func hash(self: Box) -> i32 => self.value }
struct Holder { state: Box }
route GET "/users" {
    let holder = Holder(state: Box(value: 200))
    let code = holder.state.hash()
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, concretized_generic_call_result_clears_protocol_constraint_metadata) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
i32 impl Hashable {
    func hash(self: i32) -> i32 => self
}
func id<T: Hashable>(x: T) -> T { x }
route GET "/users" {
    let x = id(200)
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    const auto& local = hir->routes[0].locals[0];
    CHECK_EQ(local.type, HirTypeKind::I32);
    CHECK_EQ(local.generic_index, 0xffffffffu);
    CHECK_FALSE(local.generic_has_error_constraint);
    CHECK_FALSE(local.generic_has_eq_constraint);
    CHECK_FALSE(local.generic_has_ord_constraint);
    CHECK_EQ(local.generic_protocol_index, 0xffffffffu);
    CHECK_EQ(local.generic_protocol_count, 0u);
}
TEST(frontend, analyze_rejects_duplicate_protocol_method_requirement) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
    func hash() -> i32
}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, protocol_declaration_is_recorded_in_hir) {
    const auto src = R"rut(
protocol Hashable {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    REQUIRE_EQ(ast->items[0].kind, AstItemKind::Protocol);
    CHECK(ast->items[0].protocol.name.eq({"Hashable", 8}));
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->protocols.len, 4u);
    CHECK(hir->protocols[3].name.eq({"Hashable", 8}));
    CHECK(hir->protocols[3].kind == HirProtocolKind::Custom);
}
TEST(frontend, analyze_rejects_duplicate_custom_protocol_declaration) {
    const auto src = R"rut(
protocol Hashable {}
protocol Hashable {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_custom_protocol_declaration_using_builtin_name) {
    const auto src = R"rut(
protocol Eq {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, generic_function_call_accepts_custom_protocol_constraint_for_builtin_conformance) {
    const auto src = R"rut(
protocol Hashable {}
i32 impl Hashable {}
func hash<T: Hashable>(x: T) -> i32 => 200
route GET "/users" {
    if hash(200) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->conformances.len, 1u);
    CHECK(hir->conformances[0].type == HirTypeKind::I32);
}
TEST(frontend, generic_function_call_accepts_custom_protocol_constraint_for_struct_conformance) {
    const auto src = R"rut(
protocol Hashable {}
struct Box { value: i32 }
Box impl Hashable {}
func hash<T: Hashable>(x: T) -> i32 => 200
route GET "/users" {
    if hash(Box(value: 200)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->conformances.len, 1u);
    CHECK(hir->conformances[0].type == HirTypeKind::Struct);
}
TEST(frontend, analyze_rejects_duplicate_custom_protocol_conformance) {
    const auto src = R"rut(
protocol Hashable {}
i32 impl Hashable {}
i32 impl Hashable {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_conformance_to_unknown_protocol) {
    const auto src = R"rut(
i32 impl Hashable {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_generic_struct_custom_protocol_conformance) {
    const auto src = R"rut(
protocol Hashable {}
struct Box<T> { value: T }
Box impl Hashable {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, generic_struct_instance_custom_protocol_conformance_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box<T> { value: T }
Box<i32> impl Hashable {}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     analyze_rejects_generic_function_call_with_custom_protocol_constraint_without_impls) {
    const auto src = R"rut(
protocol Hashable {}
func hash<T: Hashable>(x: T) -> i32 => 200
route GET "/users" {
    if hash(200) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_generic_function_accepts_error_constraint_and_standard_field_access) {
    const auto src = R"rut(
struct AuthError { err: Error, retry: i32 }
func codeOf<E: Error>(x: E) -> i32 => x.code
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(hir->functions[0].type_params.len, 1u);
    CHECK(hir->functions[0].type_params[0].has_error_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Field);
    CHECK(hir->functions[0].body.type == HirTypeKind::I32);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_and_ne_method) {
    const auto src = R"rut(
func diff<T: Eq>(x: T, y: T) -> bool => x.ne(y)
route GET "/users" {
    if diff(200, 300) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_and_le_ge_methods) {
    const auto src = R"rut(
func clampOk<T: Ord>(x: T, lo: T, hi: T) -> bool => x.ge(lo).eq(x.le(hi))
route GET "/users" {
    if clampOk(5, 1, 9) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_error_standard_fields_are_accessible_via_methods) {
    const auto src = R"rut(
route GET "/users" {
    let failed = error(.timeout, "timed out")
    let code = failed.code()
    let line = failed.line()
    if code == line { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 3u);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_and_eq_method) {
    const auto src = R"rut(
func same<T: Eq>(x: T, y: T) -> bool => x.eq(y)
route GET "/users" {
    if same(200, 200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_and_lt_method) {
    const auto src = R"rut(
func min<T: Ord>(x: T, y: T) -> T {
    if x.lt(y) { x } else { y }
}
route GET "/users" {
    if min(200, 300) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_concrete_custom_protocol_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 200).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_duplicate_impl_for_same_protocol_and_type) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable { func hash(self: Box) -> i32 => self.value }
Box impl Hashable { func hash(self: Box) -> i32 => self.value }
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_concrete_generic_struct_impl_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box<T> { value: T }
Box<i32> impl Hashable {
    func hash(self: Box<i32>) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 200).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_struct_impl_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box<T> { value: T }
Box<T> impl Hashable {
    func hash(self: Box<T>) -> i32 => 200
}
route GET "/users" {
    if Box(value: 123).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    source_generic_receiver_custom_protocol_method_dispatch_with_generic_impl_target_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box<T> { value: T }
Box<T> impl Hashable {
    func hash(self: Box<T>) -> i32 => 200
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 123)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    source_generic_receiver_custom_protocol_method_dispatch_with_generic_impl_target_tuple_of_struct_arg_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Item { value: i32 }
struct Box<T> { value: T }
Box<T> impl Hashable {
    func hash(self: Box<T>) -> i32 => 200
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: (Item(value: 7), 9))) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_impl_target_accepts_renamed_placeholder) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box<T> { value: T }
Box<U> impl Hashable {
    func hash(self: Box<U>) -> i32 => 200
}
route GET "/users" {
    if Box(value: 7).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_impl_target_accepts_multiple_type_params) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Pair<T, U> { left: T, right: U }
Pair<A, B> impl Hashable {
    func hash(self: Pair<A, B>) -> i32 => 200
}
route GET "/users" {
    if Pair(left: 7, right: "x").hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_generic_impl_target_with_duplicate_placeholder_name) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Pair<T, U> { left: T, right: U }
Pair<A, A> impl Hashable {
    func hash(self: Pair<A, A>) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
}
TEST(frontend, analyze_rejects_overlapping_generic_and_concrete_impl_for_same_protocol) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box<T> { value: T }
Box<T> impl Hashable {
    func hash(self: Box<T>) -> i32 => 200
}
Box<i32> impl Hashable {
    func hash(self: Box<i32>) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_impl_method_with_mismatched_receiver_type) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: i32) -> i32 => self
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_generic_receiver_custom_protocol_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 200)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_concrete_custom_protocol_method_dispatch_with_parameter_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash(x: i32) -> i32 }
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box, x: i32) -> i32 => x
}
route GET "/users" {
    if Box(value: 200).hash(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_impl_method_with_mismatched_requirement_parameter_type) {
    const auto src = R"rut(
protocol Hashable { func hash(x: i32) -> i32 }
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box, x: str) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_impl_method_with_mismatched_requirement_return_type) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) }
struct Box { value: i32 }
Box impl Pairable {
    func pair(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_impl_method_with_mismatched_imported_namespace_same_name_parameter_type) {
    const std::string dir = "/tmp/rut_import_namespace_protocol_requirement_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "protocol Consumer { func take(x: Box) -> i32 }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box { value: str }
struct Holder { value: i32 }
Holder impl proto.Consumer {
    func take(self: Holder, x: Box) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_impl_method_with_mismatched_imported_namespace_same_name_return_type) {
    const std::string dir =
        "/tmp/rut_import_namespace_protocol_requirement_return_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box { value: i32 }\n";
        out << "protocol Producer { func make() -> Box }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box { value: str }
struct Holder { value: i32 }
Holder impl proto.Producer {
    func make(self: Holder) -> Box => Box(value: "x")
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_impl_method_with_mismatched_concrete_generic_requirement_parameter_type) {
    const auto src = R"rut(
protocol Consumer { func take(x: Box<i32>) -> i32 }
struct Box<T> { value: T }
struct Holder { value: i32 }
Holder impl Consumer {
    func take(self: Holder, x: Box<str>) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_impl_method_with_mismatched_imported_namespace_concrete_generic_return_type) {
    const std::string dir =
        "/tmp/rut_import_namespace_protocol_requirement_generic_return_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "protocol Producer { func make() -> Box<i32> }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Holder { value: i32 }
Holder impl proto.Producer {
    func make(self: Holder) -> proto.Box<str> => proto.Box(value: "x")
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_impl_method_with_mismatched_imported_namespace_concrete_generic_parameter_type) {
    const std::string dir =
        "/tmp/rut_import_namespace_protocol_requirement_generic_param_mismatch_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "struct Box<T> { value: T }\n";
        out << "protocol Consumer { func take(x: Box<i32>) -> i32 }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box<T> { value: T }
struct Holder { value: i32 }
Holder impl proto.Consumer {
    func take(self: Holder, x: Box<str>) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_import_namespace_impl_target_with_local_same_name_receiver_type) {
    const std::string dir = "/tmp/rut_import_namespace_impl_target_same_name_receiver_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box { value: i32 }\n";
    }
    const auto src = R"rut(
import * as proto from "proto.rut"
struct Box { value: str }
proto.Box impl proto.Hashable {
    func hash(self: Box) -> i32 => 200
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_multi_protocol_impl_block_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
protocol Adder { func add(x: i32) -> i32 }
struct Box { value: i32 }
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
    func add(self: Box, x: i32) -> i32 => x
}
route GET "/users" {
    if Box(value: 7).hash() == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_concrete_custom_protocol_default_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box { value: i32 }
Box impl Hashable {}
route GET "/users" {
    if Box(value: 7).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_receiver_custom_protocol_default_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box { value: i32 }
Box impl Hashable {}
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_concrete_custom_protocol_default_method_dispatch_with_parameter_is_supported) {
    const auto src = R"rut(
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Adder {}
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_custom_protocol_default_method_dispatch_with_parameter_is_supported) {
    const auto src = R"rut(
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Adder {}
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" {
    if run(Box(value: 7)) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_receiver_custom_protocol_default_method_supports_tuple_return) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
func second(a: i32, b: i32) -> i32 => b
func run<T: Pairable>(x: T) -> i32 => x.pair() | second(_2, _1)
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_custom_protocol_default_method_tuple_return_supports_ordering) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
func run<T: Pairable>(x: T) -> (i32, i32) => x.pair()
route GET "/users" {
    if run(Box(value: 7)) < (200, 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_custom_protocol_default_method_tuple_return_supports_equality) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
func run<T: Pairable>(x: T) -> (i32, i32) => x.pair()
route GET "/users" {
    if run(Box(value: 7)) == (200, 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_empty_impl_when_protocol_method_has_no_default_body) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable {}
route GET "/users" {
    if Box(value: 7).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_generic_receiver_custom_protocol_default_method_dispatch_without_conformance) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box { value: i32 }
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_concrete_custom_protocol_default_method_dispatch_without_conformance) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box { value: i32 }
route GET "/users" {
    if Box(value: 7).hash() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_custom_protocol_default_method_supports_tuple_return) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = Box(value: 7).pair() | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_tuple_return_supports_ordering) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
route GET "/users" {
    if Box(value: 7).pair() < (200, 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_tuple_return_supports_equality) {
    const auto src = R"rut(
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Pairable {}
route GET "/users" {
    if Box(value: 7).pair() == (200, 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_optional_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => nil }
struct Box { value: i32 }
Box impl MaybeCode {}
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_error_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => error(.timeout) }
struct Box { value: i32 }
Box impl MaybeCode {}
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_block_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code() -> i32 {
        let x = 200
        x
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
route GET "/users" {
    if Box(value: 7).code() == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}

TEST(frontend, source_generic_receiver_custom_protocol_default_method_supports_block_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code() -> i32 {
        let x = 200
        x
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
func run<T: MaybeCode>(x: T) -> i32 => x.code()
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_block_body_with_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Adder {}
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_custom_protocol_default_method_supports_block_body_with_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Adder {}
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" {
    if run(Box(value: 7)) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_if_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
route GET "/users" {
    if Box(value: 7).code(true) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_receiver_custom_protocol_default_method_supports_if_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
func run<T: MaybeCode>(x: T) -> i32 => x.code(true)
route GET "/users" {
    if run(Box(value: 7)) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_guard_prefix) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        let y = maybefail(ok)
        guard y else { 401 }
        200
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
func maybefail(ok: bool) -> i32 { if ok { 200 } else { error(.timeout) } }
route GET "/users" {
    if Box(value: 7).code(true) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_custom_protocol_default_method_supports_guard_match_prefix) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        let y = maybefail(ok)
        guard match y else { case .timeout => 401 case _ => 500 }
        200
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
func maybefail(ok: bool) -> i32 { if ok { 200 } else { error(.timeout) } }
route GET "/users" {
    if Box(value: 7).code(false) == 401 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_protocol_default_method_guard_match_without_wildcard) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        let y = maybefail(ok)
        guard match y else { case .timeout => 401 }
        200
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
func maybefail(ok: bool) -> i32 { if ok { 200 } else { error(.timeout) } }
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_protocol_default_method_guard_match_on_non_error_value) {
    const auto src = R"rut(
protocol MaybeCode {
    func code() -> i32 {
        let y = 200
        guard match y else { case _ => 401 }
        200
    }
}
struct Box { value: i32 }
Box impl MaybeCode {}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_optional_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => nil }
struct Box { value: i32 }
Box impl MaybeCode { func code(self: Box) -> i32 => self.value }
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_error_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => error(.timeout) }
struct Box { value: i32 }
Box impl MaybeCode { func code(self: Box) -> i32 => self.value }
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_block_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code() -> i32 {
        let x = 200
        x
    }
}
struct Box { value: i32 }
Box impl MaybeCode { func code(self: Box) -> i32 => self.value }
route GET "/users" {
    if Box(value: 7).code() == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_if_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
struct Box { value: i32 }
Box impl MaybeCode { func code(self: Box, ok: bool) -> i32 => self.value }
route GET "/users" {
    if Box(value: 7).code(true) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_block_body_and_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Adder { func add(self: Box, x: i32) -> i32 => self.value }
route GET "/users" {
    if Box(value: 7).add(201) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_overrides_protocol_default_method_with_if_body_and_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box { value: i32 }
Box impl Adder { func add(self: Box, ok: bool) -> i32 => self.value }
route GET "/users" {
    if Box(value: 7).add(true) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_overrides_protocol_default_method_with_optional_return) {
    const std::string dir = "/tmp/rut_import_impl_overrides_optional_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl MaybeCode { func code(self: Box) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_overrides_protocol_default_method_with_error_return) {
    const std::string dir = "/tmp/rut_import_impl_overrides_error_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl MaybeCode { func code(self: Box) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let code = or(Box(value: 7).code(), 200)
    if code == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_overrides_protocol_default_method_with_block_body) {
    const std::string dir = "/tmp/rut_import_impl_overrides_block_body_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl MaybeCode { func code(self: Box) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).code() == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_overrides_protocol_default_method_with_if_body) {
    const std::string dir = "/tmp/rut_import_impl_overrides_if_body_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code(ok: bool) -> i32 {\n";
        out << "        if ok { 200 } else { 500 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl MaybeCode { func code(self: Box, ok: bool) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).code(true) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_impl_overrides_protocol_default_method_with_block_body_and_parameter) {
    const std::string dir =
        "/tmp/rut_import_impl_overrides_block_body_parameter_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Adder { func add(self: Box, x: i32) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(201) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_impl_overrides_protocol_default_method_with_if_body_and_parameter) {
    const std::string dir =
        "/tmp/rut_import_impl_overrides_if_body_parameter_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Adder { func add(self: Box, ok: bool) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(true) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_takes_precedence_over_protocol_default_method) {
    const std::string dir = "/tmp/rut_import_impl_precedence_over_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable { func hash(self: Box) -> i32 => self.value }\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).hash() == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, source_generic_impl_overrides_generic_receiver_protocol_default_method) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box<T> { value: T }
Box<T> impl Hashable { func hash(self: Box<T>) -> i32 => 7 }
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_impl_overrides_generic_receiver_protocol_default_method_with_optional_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => nil }
struct Box<T> { value: T }
Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_impl_overrides_generic_receiver_protocol_default_method_with_error_return) {
    const auto src = R"rut(
protocol MaybeCode { func code() -> i32 => error(.timeout) }
struct Box<T> { value: T }
Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_impl_overrides_generic_receiver_protocol_default_method_with_block_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code() -> i32 {
        let x = 200
        x
    }
}
struct Box<T> { value: T }
Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }
func run<T: MaybeCode>(x: T) -> i32 => x.code()
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_impl_overrides_generic_receiver_protocol_default_method_with_if_body) {
    const auto src = R"rut(
protocol MaybeCode {
    func code(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
struct Box<T> { value: T }
Box<T> impl MaybeCode { func code(self: Box<T>, ok: bool) -> i32 => 7 }
func run<T: MaybeCode>(x: T) -> i32 => x.code(true)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    source_generic_impl_overrides_generic_receiver_protocol_default_method_with_block_body_and_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box<T> { value: T }
Box<T> impl Adder { func add(self: Box<T>, x: i32) -> i32 => 7 }
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    source_generic_impl_overrides_generic_receiver_protocol_default_method_with_if_body_and_parameter) {
    const auto src = R"rut(
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box<T> { value: T }
Box<T> impl Adder { func add(self: Box<T>, ok: bool) -> i32 => 7 }
func run<T: Adder>(x: T) -> i32 => x.add(true)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_optional_return) {
    const std::string dir =
        "/tmp/rut_import_generic_impl_overrides_generic_receiver_optional_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => nil }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_error_return) {
    const std::string dir =
        "/tmp/rut_import_generic_impl_overrides_generic_receiver_error_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode { func code() -> i32 => error(.timeout) }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => or(x.code(), 200)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_block_body) {
    const std::string dir =
        "/tmp/"
        "rut_import_generic_impl_overrides_generic_receiver_block_body_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode { func code(self: Box<T>) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code()
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_if_body) {
    const std::string dir =
        "/tmp/rut_import_generic_impl_overrides_generic_receiver_if_body_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol MaybeCode {\n";
        out << "    func code(ok: bool) -> i32 {\n";
        out << "        if ok { 200 } else { 500 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl MaybeCode { func code(self: Box<T>, ok: bool) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: MaybeCode>(x: T) -> i32 => x.code(true)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_block_body_and_parameter) {
    const std::string dir =
        "/tmp/"
        "rut_import_generic_impl_overrides_generic_receiver_block_body_parameter_default_method_"
        "frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Adder { func add(self: Box<T>, x: i32) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Adder>(x: T) -> i32 => x.add(201)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method_with_if_body_and_parameter) {
    const std::string dir =
        "/tmp/"
        "rut_import_generic_impl_overrides_generic_receiver_if_body_parameter_default_method_"
        "frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Adder { func add(self: Box<T>, ok: bool) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Adder>(x: T) -> i32 => x.add(true)
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_generic_impl_overrides_generic_receiver_protocol_default_method) {
    const std::string dir =
        "/tmp/rut_import_generic_impl_overrides_generic_receiver_default_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable { func hash(self: Box<T>) -> i32 => 7 }\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable>(x: T) -> i32 => x.hash()
route GET "/users" {
    if run(Box(value: 123)) == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, source_generic_receiver_multi_protocol_default_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Hashable {}
Box impl Adder {}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_multi_protocol_empty_impl_block_default_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Hashable, Adder {}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_empty_impl_block_default_method_dispatch_is_supported) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Hashable, Adder {}
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).add(3) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_empty_impl_block_default_method_supports_block_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32 {
        let x = 200
        x
    }
}
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {}
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).add(3) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_empty_impl_block_default_method_supports_if_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {}
route GET "/users" {
    let h = Box(value: 7).hash(true)
    if h == 200 {
        if Box(value: 7).add(true) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_empty_impl_block_default_method_supports_tuple_return) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Hashable, Pairable {}
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).pair() == (200, 500) { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_multi_protocol_empty_impl_block_default_method_tuple_return_supports_ordering) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Hashable, Pairable {}
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).pair() < (200, 600) { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_multi_protocol_empty_impl_block_default_method_supports_block_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32 {
        let x = 200
        x
    }
}
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_multi_protocol_empty_impl_block_default_method_supports_tuple_return) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Hashable, Pairable {}
func run<T: Hashable, Pairable>(x: T) -> (i32, i32) {
    let h = x.hash()
    if h == 200 { x.pair() } else { (0, 0) }
}
route GET "/users" {
    if run(Box(value: 7)) == (200, 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    source_generic_receiver_multi_protocol_empty_impl_block_default_method_tuple_return_supports_ordering) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
protocol Pairable { func pair() -> (i32, i32) => (200, 500) }
struct Box { value: i32 }
Box impl Hashable, Pairable {}
func run<T: Hashable, Pairable>(x: T) -> (i32, i32) {
    let h = x.hash()
    if h == 200 { x.pair() } else { (0, 0) }
}
route GET "/users" {
    if run(Box(value: 7)) < (200, 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     source_generic_receiver_multi_protocol_empty_impl_block_default_method_supports_if_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash(ok: bool) -> i32 {
        if ok { 200 } else { 500 }
    }
}
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash(true)
    if h == 200 { x.add(true) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_empty_impl_for_generic_receiver_multi_protocol_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_generic_multi_protocol_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Adder { func add(x: i32) -> i32 => x }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {}\n";
        out << "Box<T> impl Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_receiver_multi_protocol_empty_impl_block_for_tuple_default_method_dispatch) {
    const std::string dir =
        "/tmp/rut_import_tuple_default_impl_generic_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Pairable>(x: T) -> (i32, i32) {
    let h = x.hash()
    if h == 200 { x.pair() } else { (0, 0) }
}
route GET "/users" {
    if run(Box(value: 7)) == (200, 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_receiver_multi_protocol_empty_impl_block_for_tuple_default_method_ordering) {
    const std::string dir =
        "/tmp/rut_import_tuple_ordering_default_impl_generic_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Pairable>(x: T) -> (i32, i32) {
    let h = x.hash()
    if h == 200 { x.pair() } else { (0, 0) }
}
route GET "/users" {
    if run(Box(value: 7)) < (200, 600) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_receiver_multi_protocol_empty_impl_block_for_if_body_default_method_dispatch) {
    const std::string dir =
        "/tmp/rut_import_if_body_default_impl_generic_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash(ok: bool) -> i32 {\n";
        out << "        if ok { 200 } else { 500 }\n";
        out << "    }\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash(true)
    if h == 200 { x.add(true) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_multi_protocol_empty_impl_block_for_tuple_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_tuple_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).pair() == (200, 500) { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_multi_protocol_empty_impl_block_for_tuple_default_method_ordering) {
    const std::string dir =
        "/tmp/rut_import_tuple_ordering_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Pairable { func pair() -> (i32, i32) => (200, 500) }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Pairable {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).pair() < (200, 600) { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_receiver_multi_protocol_empty_impl_block_for_block_body_default_method_dispatch) {
    const std::string dir =
        "/tmp/rut_import_block_body_default_impl_generic_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_multi_protocol_empty_impl_block_for_if_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_if_body_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash(ok: bool) -> i32 {\n";
        out << "        if ok { 200 } else { 500 }\n";
        out << "    }\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let h = Box(value: 7).hash(true)
    if h == 200 {
        if Box(value: 7).add(true) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_multi_protocol_empty_impl_block_for_block_body_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_block_body_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32 {\n";
        out << "        let x = 200\n";
        out << "        x\n";
        out << "    }\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).add(3) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_multi_protocol_empty_impl_block_for_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Adder { func add(x: i32) -> i32 => x }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    let h = Box(value: 7).hash()
    if h == 200 {
        if Box(value: 7).add(3) == 3 { return 200 } else { return 500 }
    } else {
        return 500
    }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    import_relative_file_merges_imported_generic_multi_protocol_empty_impl_block_for_default_method_dispatch) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_multi_protocol_block_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 200 }\n";
        out << "protocol Adder { func add(x: i32) -> i32 => x }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 7)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(
    frontend,
    analyze_rejects_generic_receiver_multi_protocol_empty_impl_block_when_required_method_is_missing) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
protocol Adder { func add(x: i32) -> i32 => x }
struct Box { value: i32 }
Box impl Hashable, Adder {}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_imported_generic_multi_protocol_empty_impl_block_when_required_method_is_missing) {
    const std::string dir =
        "/tmp/rut_import_generic_default_impl_multi_protocol_block_missing_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "protocol Adder { func add(x: i32) -> i32 => x }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 200 { x.add(3) } else { 0 }
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_imported_generic_multi_protocol_empty_impl_block_with_conflicting_default_method_names) {
    const std::string dir = "/tmp/rut_import_generic_default_impl_multi_protocol_conflict_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol P1 { func hash() -> i32 => 1 }\n";
        out << "protocol P2 { func hash() -> i32 => 2 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl P1, P2 {}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_imported_generic_multi_protocol_impl_block_with_ambiguous_method_name) {
    const std::string dir = "/tmp/rut_import_generic_impl_multi_protocol_ambiguous_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol P1 { func hash() -> i32 }\n";
        out << "protocol P2 { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl P1, P2 {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_imported_generic_multi_protocol_impl_block_with_duplicate_protocol_name) {
    const std::string dir = "/tmp/rut_import_generic_impl_multi_protocol_duplicate_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Hashable {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_imported_generic_multi_protocol_impl_block_with_unknown_protocol_name) {
    const std::string dir = "/tmp/rut_import_generic_impl_multi_protocol_unknown_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Missing {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_imported_duplicate_impl_for_same_protocol_and_type_via_empty_impl) {
    const std::string dir = "/tmp/rut_import_empty_impl_duplicate_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {}\n";
        out << "Box<T> impl Hashable {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_imported_overlapping_empty_impl_and_multi_protocol_impl) {
    const std::string dir = "/tmp/rut_import_empty_impl_overlap_multi_protocol_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 }\n";
        out << "protocol Adder { func add(x: i32) -> i32 }\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable {}\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "    func add(self: Box<T>, x: i32) -> i32 => x\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_generic_receiver_method_dispatch_when_multiple_protocol_constraints_define_same_name) {
    const auto src = R"rut(
protocol A { func hash() -> i32 => 1 }
protocol B { func hash() -> i32 => 2 }
struct Box { value: i32 }
Box impl A {}
Box impl B {}
func run<T: A, B>(x: T) -> i32 => x.hash()
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_selective_import_protocol_alias_generic_receiver_dispatch_with_local_same_name_protocol) {
    const std::string dir = "/tmp/rut_selective_import_alias_protocol_dispatch_same_name_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable { func hash() -> i32 => 1 }\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {}\n";
    }
    const auto src = R"rut(
import { Hashable as Digestible, Box as ImportedBox } from "proto.rut"
protocol Digestible {}
func run<T: Digestible>(x: T) -> i32 => x.hash()
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_concrete_receiver_method_dispatch_when_multiple_protocol_impls_define_same_name) {
    const auto src = R"rut(
protocol A { func hash() -> i32 => 1 }
protocol B { func hash() -> i32 => 2 }
struct Box { value: i32 }
Box impl A {}
Box impl B {}
route GET "/users" {
    if Box(value: 7).hash() == 1 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_impl_takes_precedence_over_protocol_default_method) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 => 200 }
struct Box { value: i32 }
Box impl Hashable { func hash(self: Box) -> i32 => self.value }
route GET "/users" {
    if Box(value: 7).hash() == 7 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_impl_may_omit_protocol_method_with_default_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
    func add(x: i32) -> i32 => x
}
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_impl_may_omit_protocol_method_with_default_body) {
    const std::string dir = "/tmp/rut_import_impl_omit_default_body_method_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "    func add(x: i32) -> i32 => x\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, analyze_rejects_impl_missing_required_protocol_method_without_default_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
    func add(x: i32) -> i32
}
struct Box { value: i32 }
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_multi_protocol_impl_may_omit_methods_with_default_bodies) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
    func add(x: i32) -> i32 => x
}
protocol Adder {
    func mul(x: i32) -> i32 => x
}
struct Box { value: i32 }
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_impl_may_omit_methods_with_block_body_default) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_multi_protocol_impl_may_omit_methods_with_if_body_default) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box { value: i32 }
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" {
    if Box(value: 7).add(true) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_multi_protocol_impl_with_default_bodies) {
    const std::string dir = "/tmp/rut_import_multi_protocol_impl_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "    func add(x: i32) -> i32 => x\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func mul(x: i32) -> i32 => x\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_multi_protocol_impl_with_block_body_default) {
    const std::string dir = "/tmp/rut_import_multi_protocol_impl_block_body_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(201) == 201 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, import_relative_file_merges_imported_multi_protocol_impl_with_if_body_default) {
    const std::string dir = "/tmp/rut_import_multi_protocol_impl_if_body_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box { value: i32 }\n";
        out << "Box impl Hashable, Adder {\n";
        out << "    func hash(self: Box) -> i32 => self.value\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" {
    if Box(value: 7).add(true) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend, source_generic_multi_protocol_impl_may_omit_methods_with_default_bodies) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(x: i32) -> i32 => x
}
struct Box<T> { value: T }
Box<T> impl Hashable, Adder {
    func hash(self: Box<T>) -> i32 => 7
}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_multi_protocol_impl_may_omit_methods_with_block_body_default) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(x: i32) -> i32 {
        let y = x
        y
    }
}
struct Box<T> { value: T }
Box<T> impl Hashable, Adder {
    func hash(self: Box<T>) -> i32 => 7
}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend, source_generic_multi_protocol_impl_may_omit_methods_with_if_body_default) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(ok: bool) -> i32 {
        if ok { 3 } else { 0 }
    }
}
struct Box<T> { value: T }
Box<T> impl Hashable, Adder {
    func hash(self: Box<T>) -> i32 => 7
}
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(true) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_merges_imported_generic_multi_protocol_impl_with_default_bodies) {
    const std::string dir = "/tmp/rut_import_generic_multi_protocol_impl_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 => x\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_merges_imported_generic_multi_protocol_impl_with_block_body_default) {
    const std::string dir =
        "/tmp/rut_import_generic_multi_protocol_impl_block_body_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32 {\n";
        out << "        let y = x\n";
        out << "        y\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(3) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     import_relative_file_merges_imported_generic_multi_protocol_impl_with_if_body_default) {
    const std::string dir = "/tmp/rut_import_generic_multi_protocol_impl_if_body_default_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(ok: bool) -> i32 {\n";
        out << "        if ok { 3 } else { 0 }\n";
        out << "    }\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
func run<T: Hashable, Adder>(x: T) -> i32 {
    let h = x.hash()
    if h == 7 { x.add(true) } else { 0 }
}
route GET "/users" {
    if run(Box(value: 11)) == 3 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE(hir);
}
TEST(frontend,
     analyze_rejects_imported_multi_protocol_impl_missing_required_method_without_default_body) {
    const std::string dir = "/tmp/rut_import_multi_protocol_impl_missing_required_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "    func add(x: i32) -> i32 => x\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func mul(x: i32) -> i32\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(
    frontend,
    analyze_rejects_imported_generic_multi_protocol_impl_missing_required_method_without_default_body) {
    const std::string dir = "/tmp/rut_import_generic_multi_protocol_impl_missing_required_frontend";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir + "/proto.rut", std::ios::binary);
        out << "protocol Hashable {\n";
        out << "    func hash() -> i32\n";
        out << "}\n";
        out << "protocol Adder {\n";
        out << "    func add(x: i32) -> i32\n";
        out << "}\n";
        out << "struct Box<T> { value: T }\n";
        out << "Box<T> impl Hashable, Adder {\n";
        out << "    func hash(self: Box<T>) -> i32 => 7\n";
        out << "}\n";
    }
    const auto src = R"rut(
import "proto.rut"
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap_with_path(ast.value(), dir + "/main.rut");
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_multi_protocol_impl_missing_required_method_without_default_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
    func add(x: i32) -> i32 => x
}
protocol Adder {
    func mul(x: i32) -> i32
}
struct Box { value: i32 }
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend,
     analyze_rejects_generic_multi_protocol_impl_missing_required_method_without_default_body) {
    const auto src = R"rut(
protocol Hashable {
    func hash() -> i32
}
protocol Adder {
    func add(x: i32) -> i32
}
struct Box<T> { value: T }
Box<T> impl Hashable, Adder {
    func hash(self: Box<T>) -> i32 => 7
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_multi_protocol_impl_block_with_conflicting_default_method_names) {
    const auto src = R"rut(
protocol P1 { func hash() -> i32 => 1 }
protocol P2 { func hash() -> i32 => 2 }
struct Box { value: i32 }
Box impl P1, P2 {}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_multi_protocol_impl_block_with_ambiguous_method_name) {
    const auto src = R"rut(
protocol P1 { func hash() -> i32 }
protocol P2 { func hash() -> i32 }
struct Box { value: i32 }
Box impl P1, P2 {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_multi_protocol_impl_block_with_duplicate_protocol_name) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable, Hashable {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_duplicate_impl_for_same_protocol_and_type_via_empty_impl) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
struct Box { value: i32 }
Box impl Hashable {}
Box impl Hashable {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_overlapping_empty_impl_and_multi_protocol_impl) {
    const auto src = R"rut(
protocol Hashable { func hash() -> i32 }
protocol Adder { func add(x: i32) -> i32 }
struct Box { value: i32 }
Box impl Hashable {}
Box impl Hashable, Adder {
    func hash(self: Box) -> i32 => self.value
    func add(self: Box, x: i32) -> i32 => x
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_impl_block_with_unknown_protocol_name) {
    const auto src = R"rut(
struct Box { value: i32 }
Box impl Missing {
    func hash(self: Box) -> i32 => self.value
}
route GET "/users" { return 200 }
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_and_equality) {
    const auto src = R"rut(
func same<T: Eq>(x: T, y: T) -> bool => x == y
route GET "/users" {
    if same("a", "a") { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].type_params[0].has_eq_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
    CHECK(hir->functions[0].body.type == HirTypeKind::Bool);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_for_tuple) {
    const auto src = R"rut(
func same<T: Eq>(x: T, y: T) -> bool => x == y
route GET "/users" {
    if same((200, 500), (200, 500)) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_eq_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_for_tuple_of_struct) {
    const auto src = R"rut(
struct Box { value: i32 }
func same<T: Eq>(x: T, y: T) -> bool => x == y
route GET "/users" {
    if same((Box(value: 200), 500), (Box(value: 200), 500)) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_eq_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_for_struct) {
    const auto src = R"rut(
struct Box<T> { value: T }
func same<T: Eq>(x: T, y: T) -> bool => x == y
route GET "/users" {
    if same(Box(value: 200), Box(value: 200)) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_eq_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_eq_constraint_for_variant_payload) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
func same<T: Eq>(x: T, y: T) -> bool => x == y
route GET "/users" {
    if same(Result.ok(200), Result.ok(500)) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_eq_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::Eq);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_and_lt) {
    const auto src = R"rut(
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min(200, 500) == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::IfElse);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, analyze_rejects_generic_function_lt_without_ord_constraint) {
    const auto src = R"rut(
func min<T>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_and_lt_for_str) {
    const auto src = R"rut(
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min("alpha", "beta") == "alpha" { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    CHECK(hir->functions[0].body.kind == HirExprKind::IfElse);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_for_tuple) {
    const auto src = R"rut(
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min((200, 500), (200, 600)) == (200, 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_for_tuple_of_struct) {
    const auto src = R"rut(
struct Box { value: i32 }
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min((Box(value: 200), 500), (Box(value: 200), 600)) == (Box(value: 200), 500) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_for_struct) {
    const auto src = R"rut(
struct Box<T> { value: T }
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min(Box(value: 200), Box(value: 500)) == Box(value: 200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, source_generic_function_accepts_ord_constraint_for_variant) {
    const auto src = R"rut(
variant Result<T> { ok(T), err }
func min<T: Ord>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min(Result<i32>.ok(200), Result<i32>.ok(500)) == Result<i32>.ok(200) { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].type_params[0].has_ord_constraint);
    REQUIRE(hir->functions[0].body.lhs != nullptr);
    CHECK(hir->functions[0].body.lhs->kind == HirExprKind::Lt);
}
TEST(frontend, analyze_rejects_generic_function_lt_for_str_without_ord_constraint) {
    const auto src = R"rut(
func min<T>(x: T, y: T) -> T {
    if x < y { x } else { y }
}
route GET "/users" {
    if min("alpha", "beta") == "alpha" { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
    CHECK(hir.error().code == FrontendError::UnsupportedSyntax);
}
TEST(frontend, analyze_rejects_generic_function_equality_without_eq_constraint) {
    const auto src = R"rut(
func same<T>(x: T, y: T) -> bool => x == y
route GET "/users" {
    return 200
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_generic_function_call_with_inconsistent_type_binding) {
    const auto src = R"rut(
func first<T>(x: T, y: T) -> T => x
route GET "/users" {
    let code = first(200, "oops")
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    CHECK(!hir);
}
TEST(frontend, analyze_rejects_generic_function_call_violating_error_constraint) {
    const auto src = R"rut(
func codeOf<E: Error>(x: E) -> i32 => x.code
route GET "/users" {
    let code = codeOf(200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir);
}
TEST(frontend, analyze_rejects_generic_function_call_with_wrong_type_arg_count) {
    const auto src = R"rut(
func id<T>(x: T) -> T => x
route GET "/users" {
    let code = id<i32, i32>(200)
    if code == 200 { return 200 } else { return 500 }
}
)rut";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    CHECK(!hir);
}
TEST(frontend, source_function_call_inlines_variant_expression_body) {
    const auto src = R"(
variant AuthState { ok, err }
func success() -> AuthState => AuthState.ok
route GET "/users" {
    match success() {
    case .ok: return 200
    case _: return 500
    }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    if (!hir) {
        rut::test::out("analyze err code=");
        rut::test::out_int(static_cast<int>(hir.error().code));
        rut::test::out(" line=");
        rut::test::out_int(static_cast<int>(hir.error().span.line));
        rut::test::out(" col=");
        rut::test::out_int(static_cast<int>(hir.error().span.col));
        rut::test::out("\n");
        REQUIRE(hir);
    }
    REQUIRE_EQ(hir->functions.len, 1u);
    REQUIRE_EQ(hir->routes.len, 1u);
    CHECK(hir->routes[0].control.kind == HirControlKind::Match);
    CHECK(hir->routes[0].control.match_expr.type == HirTypeKind::Variant);
}
TEST(frontend, source_function_call_inlines_variant_expression_body_without_return_annotation) {
    const auto src = R"(
variant AuthState { ok, err }
func success() => AuthState.ok
route GET "/users" {
    match success() {
    case .ok: return 200
    case _: return 500
    }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::Variant);
    CHECK(hir->functions[0].return_variant_index != 0xffffffffu);
    REQUIRE_EQ(hir->routes.len, 1u);
    CHECK(hir->routes[0].control.kind == HirControlKind::Match);
    CHECK(hir->routes[0].control.match_expr.type == HirTypeKind::Variant);
}
TEST(frontend, source_function_supports_explicit_tuple_return_type) {
    const auto src = R"(
func pair() -> (i32, i32) { (200, 500) }
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = pair() | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[0].return_type == HirTypeKind::Tuple);
    CHECK_EQ(hir->functions[0].return_tuple_len, 2u);
    CHECK(hir->functions[0].return_tuple_types[0] == HirTypeKind::I32);
    CHECK(hir->functions[0].return_tuple_types[1] == HirTypeKind::I32);
}
TEST(frontend, source_function_supports_explicit_tuple_param_type) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
func pick(pair: (i32, i32)) -> i32 => pair | second(_2, _1)
route GET "/users" {
    let code = pick((200, 500))
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[1].params[0].type == HirTypeKind::Tuple);
    CHECK_EQ(hir->functions[1].params[0].tuple_len, 2u);
}
TEST(frontend, source_function_tuple_return_tracks_struct_slots_separately) {
    const auto src = R"(
struct Box { value: i32 }
func make() -> (Box, i32) => (Box(value: 200), 1)
route GET "/users" { return 200 }
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::Tuple);
    CHECK_EQ(hir->functions[0].return_tuple_len, 2u);
    CHECK(hir->functions[0].return_tuple_types[0] == HirTypeKind::Struct);
    CHECK(hir->functions[0].return_tuple_struct_indices[0] != 0xffffffffu);
}

TEST(frontend, function_param_local_ref_preserves_shape_index) {
    const auto src = R"(
struct Box { value: i32 }
func id(x: (Box, i32)) -> (Box, i32) => x
route GET "/users" { return 200 }
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    CHECK_EQ(static_cast<u8>(body.kind), static_cast<u8>(HirExprKind::LocalRef));
    CHECK_EQ(body.type, HirTypeKind::Tuple);
    CHECK(body.shape_index != 0xffffffffu);
    REQUIRE_EQ(body.tuple_len, 2u);
    CHECK_EQ(body.tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(body.tuple_struct_indices[0], 0u);
}

TEST(frontend, source_route_let_supports_explicit_tuple_type) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let pair: (i32, i32) = (200, 500)
    let code = pair | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Tuple);
    CHECK_EQ(hir->routes[0].locals[0].tuple_len, 2u);
}
TEST(frontend, source_function_block_let_supports_explicit_tuple_type) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
func pick() -> i32 {
    let pair: (i32, i32) = (200, 500)
    pair | second(_2, _1)
}
route GET "/users" {
    let code = pick()
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    CHECK(hir->functions[1].return_type == HirTypeKind::I32);
    CHECK(hir->functions[1].body.type == HirTypeKind::I32);
}
TEST(frontend, analyze_rejects_function_without_return_annotation_when_base_type_is_unknown) {
    const auto src = R"(
func maybe() => nil
route GET "/users" { return 200 }
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    CHECK(!hir);
}
TEST(frontend, source_function_call_propagates_optional_value_flow) {
    const auto src = R"(
func maybe() -> i32 => nil
route GET "/users" {
    let code = or(maybe(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_function_block_body_allows_pure_nil_with_explicit_return_type) {
    const auto src = R"(
func maybe() -> i32 {
    nil
}
route GET "/users" {
    let code = or(maybe(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK(hir->functions[0].body.shape_index != 0xffffffffu);
    CHECK(hir->functions[0].body.may_nil);
    CHECK_FALSE(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_infers_optional_return_from_if_without_annotation) {
    const auto src = R"(
func maybe(ok: bool) {
    if ok { 200 } else { nil }
}
route GET "/users" {
    let code = or(maybe(true), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK(hir->functions[0].body.may_nil);
    CHECK_FALSE(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_call_propagates_error_value_flow) {
    const auto src = R"(
func fail() -> i32 => error(.timeout)
route GET "/users" {
    let code = or(fail(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_function_block_body_allows_pure_error_with_explicit_return_type) {
    const auto src = R"(
func fail() -> i32 {
    error(.timeout)
}
route GET "/users" {
    let code = or(fail(), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK(hir->functions[0].body.shape_index != 0xffffffffu);
    CHECK_FALSE(hir->functions[0].body.may_nil);
    CHECK(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_infers_error_return_from_if_without_annotation) {
    const auto src = R"(
func maybefail(ok: bool) {
    if ok { 200 } else { error(.timeout) }
}
route GET "/users" {
    let code = or(maybefail(true), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK_FALSE(hir->functions[0].body.may_nil);
    CHECK(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_allows_pure_nil_if_with_explicit_return_type) {
    const auto src = R"(
func maybe(ok: bool) -> i32 {
    if ok { nil } else { nil }
}
route GET "/users" {
    let code = or(maybe(true), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK(hir->functions[0].body.shape_index != 0xffffffffu);
    CHECK(hir->functions[0].body.may_nil);
    CHECK_FALSE(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_block_body_with_let_prefix_inlines) {
    const auto src = R"(
func wrap(x: i32) -> i32 {
    let y = x
    y
}
route GET "/users" {
    let code = wrap(200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_function_block_body_with_guard_prefix_inlines) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { 200 } else { error(.timeout) }
}
func wrap(ok: bool) -> i32 {
    let y = maybefail(ok)
    guard y else { 401 }
    200
}
route GET "/users" {
    let code = wrap(false)
    if code == 401 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->functions[1].body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_FALSE(hir->functions[1].body.may_nil);
    CHECK_FALSE(hir->functions[1].body.may_error);
}
TEST(frontend, source_function_block_body_with_guard_let_prefix_binds_value) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { 200 } else { error(.timeout) }
}
func wrap(ok: bool) -> i32 {
    guard let y = maybefail(ok) else { 401 }
    y
}
route GET "/users" {
    let code = wrap(true)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->functions[1].body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_FALSE(hir->functions[1].body.may_nil);
    CHECK_FALSE(hir->functions[1].body.may_error);
}
TEST(frontend, source_function_block_body_with_guard_match_prefix_inlines) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { 200 } else { error(.timeout) }
}
func wrap(ok: bool) -> i32 {
    let y = maybefail(ok)
    guard match y else { case .timeout => 401 case _ => 500 }
    200
}
route GET "/users" {
    let code = wrap(false)
    if code == 401 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 2u);
    CHECK_EQ(static_cast<u8>(hir->functions[1].body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_FALSE(hir->functions[1].body.may_nil);
    CHECK_FALSE(hir->functions[1].body.may_error);
}
TEST(frontend, analyze_rejects_function_block_guard_match_without_wildcard) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { 200 } else { error(.timeout) }
}
func wrap(ok: bool) -> i32 {
    let y = maybefail(ok)
    guard match y else { case .timeout => 401 }
    200
}
route GET "/users" { return 200 }
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
}
TEST(frontend, analyze_rejects_function_block_guard_match_on_non_error_value) {
    const auto src = R"(
func wrap(x: i32) -> i32 {
    guard match x else { case .timeout => 401 case _ => 500 }
    200
}
route GET "/users" { return 200 }
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(!hir);
}
TEST(frontend, source_function_block_body_with_final_if_inlines) {
    const auto src = R"(
func wrap(x: i32) -> i32 {
    let y = x
    if y == 200 { 200 } else { 500 }
}
route GET "/users" {
    let code = wrap(200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->functions[0].body.kind), static_cast<u8>(HirExprKind::IfElse));
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}

TEST(frontend, source_function_if_merging_tuple_of_struct_preserves_struct_slots) {
    const auto src = R"(
struct Box { value: i32 }
func pick(flag: bool) -> (Box, i32) {
    if flag { (Box(value: 200), 1) } else { (Box(value: 500), 2) }
}
route GET "/users" {
    let pair = pick(true)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    CHECK_EQ(static_cast<u8>(body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_EQ(body.type, HirTypeKind::Tuple);
    REQUIRE_EQ(body.tuple_len, 2u);
    CHECK_EQ(body.tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(body.tuple_struct_indices[0], 0u);
}

TEST(frontend, source_function_guard_merging_tuple_of_struct_preserves_struct_slots) {
    const auto src = R"(
struct Box { value: i32 }
func pick(flag: bool) -> (Box, i32) {
    guard flag else { (Box(value: 500), 2) }
    (Box(value: 200), 1)
}
route GET "/users" {
    let pair = pick(true)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    const auto& body = hir->functions[0].body;
    CHECK_EQ(static_cast<u8>(body.kind), static_cast<u8>(HirExprKind::IfElse));
    CHECK_EQ(body.type, HirTypeKind::Tuple);
    REQUIRE_EQ(body.tuple_len, 2u);
    CHECK_EQ(body.tuple_types[0], HirTypeKind::Struct);
    CHECK_EQ(body.tuple_struct_indices[0], 0u);
}
TEST(frontend, source_pipe_unwrapped_tuple_of_struct_preserves_param_shape) {
    const auto src = R"(
struct Box { value: i32 }
func code(x: (Box, i32)) -> i32 => 200
func load(ok: bool) -> (Box, i32) {
    if ok { (Box(value: 200), 1) } else { error(.timeout) }
}
route GET "/users" {
    let code = load(true) | code(_)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    const auto& code = hir->routes[0].locals[0];
    CHECK_EQ(code.type, HirTypeKind::I32);
    CHECK_FALSE(code.may_nil);
    CHECK_TRUE(code.may_error);
}

TEST(frontend, source_function_block_body_with_final_match_inlines) {
    const auto src = R"(
variant Result { ok, err }
func pick(x: Result) -> i32 {
    let y = x
    match y {
        case .ok => 200
        case .err => 500
    }
}
route GET "/users" {
    let code = pick(Result.ok)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->functions[0].body.kind), static_cast<u8>(HirExprKind::IfElse));
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_function_match_arm_block_with_let_inlines) {
    const auto src = R"(
variant Result { ok, err }
func pick(x: Result) -> i32 {
    match x {
        case .ok => { let y = 200 y }
        case .err => { let z = 500 z }
    }
}
route GET "/users" {
    let code = pick(Result.ok)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK_EQ(static_cast<u8>(hir->functions[0].body.kind), static_cast<u8>(HirExprKind::IfElse));
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_function_infers_optional_return_from_match_without_annotation) {
    const auto src = R"(
variant Result { ok, err }
func pick(x: Result) {
    match x {
        case .ok => 200
        case .err => nil
    }
}
route GET "/users" {
    let code = or(pick(Result.ok), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK(hir->functions[0].body.may_nil);
    CHECK_FALSE(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_infers_error_return_from_match_without_annotation) {
    const auto src = R"(
variant Result { ok, err }
func pick(x: Result) {
    match x {
        case .ok => 200
        case .err => error(.timeout)
    }
}
route GET "/users" {
    let code = or(pick(Result.ok), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK_FALSE(hir->functions[0].body.may_nil);
    CHECK(hir->functions[0].body.may_error);
}
TEST(frontend, source_function_allows_pure_error_match_with_explicit_return_type) {
    const auto src = R"(
variant Result { ok, err }
func pick(x: Result) -> i32 {
    match x {
        case .ok => error(.timeout)
        case .err => error(.timeout)
    }
}
route GET "/users" {
    let code = or(pick(Result.ok), 200)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    CHECK(hir->functions[0].return_type == HirTypeKind::I32);
    CHECK_FALSE(hir->functions[0].body.may_nil);
    CHECK(hir->functions[0].body.may_error);
}
TEST(frontend, source_pipe_single_stage_inlines_call) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = 200 | id(_)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
}
TEST(frontend, source_pipe_chains_multiple_stages) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = 200 | id(_) | id(_)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_respects_placeholder_position) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = 200 | second(500, _)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_accepts_placeholder_slot_one_alias) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = 200 | second(500, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_accepts_tuple_literal_multi_slot_placeholders) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = (200, 500) | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_accepts_tuple_returning_function_multi_slot_placeholders) {
    const auto src = R"(
func pair() { (200, 500) }
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = pair() | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_accepts_tuple_local_alias_multi_slot_placeholders) {
    const auto src = R"(
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let pair = (200, 500)
    let code = pair | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_chains_tuple_returning_stage_multi_slot_placeholders) {
    const auto src = R"(
func swap(a: i32, b: i32) { (b, a) }
func second(a: i32, b: i32) -> i32 => b
route GET "/users" {
    let code = (200, 500) | swap(_1, _2) | second(_1, _2)
    if code == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 1u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
}
TEST(frontend, source_pipe_propagates_known_nil_as_optional_value_flow) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = nil | id(_)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK(hir->routes[0].locals[0].shape_index != 0xffffffffu);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_propagates_known_error_as_error_value_flow) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let failed = error(.timeout)
    let code = failed | id(_)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 3u);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::I32);
    CHECK(hir->routes[0].locals[1].shape_index != 0xffffffffu);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK(hir->routes[0].locals[1].may_error);
    CHECK(hir->routes[0].locals[2].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[2].may_nil);
    CHECK_FALSE(hir->routes[0].locals[2].may_error);
}
TEST(frontend, analyze_rejects_pipe_without_placeholder) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = 200 | id(200)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_pipe_with_non_stage_rhs) {
    const auto src = R"(
route GET "/users" {
    let code = 200 | 404
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_pipe_with_placeholder_slot_two) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = 200 | id(_2)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, analyze_rejects_pipe_runtime_fallible_lhs_with_placeholder_slot_two) {
    const auto src = R"(
func id(x: str) -> str => x
route GET "/users" {
    let code = req.header("Host") | id(_2)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE_FALSE(hir.has_value());
    CHECK_EQ(static_cast<u8>(hir.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, parse_rejects_pipe_with_placeholder_slot_eleven) {
    const auto src = R"(
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = 200 | id(_11)
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE_FALSE(ast.has_value());
    CHECK_EQ(static_cast<u8>(ast.error().code), static_cast<u8>(FrontendError::UnsupportedSyntax));
}
TEST(frontend, source_pipe_runtime_optional_lhs_flows_via_or) {
    const auto src = R"(
func id(x: str) -> str => x
route GET "/users" {
    let host = req.header("Host") | id(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK(hir->routes[0].locals[1].init.kind == HirExprKind::Or);
    CHECK_NE(hir->routes[0].locals[1].init.shape_index, 0xffffffffu);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_error_lhs_flows_via_or) {
    const auto src = R"(
func fail() -> i32 => error(.timeout)
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = fail() | id(_)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_optional_error_lhs_flows_via_or) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { nil } else { error(.timeout) }
}
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = maybefail(true) | id(_)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::I32);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::I32);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_optional_lhs_flows_into_optional_stage_via_or) {
    const auto src = R"(
func drop(x: str) -> str { nil }
route GET "/users" {
    let host = req.header("Host") | drop(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK_FALSE(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_optional_lhs_flows_into_error_stage_via_or) {
    const auto src = R"(
func failstage(x: str) -> str => error(.timeout)
route GET "/users" {
    let host = req.header("Host") | failstage(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_error_lhs_flows_into_optional_stage_via_or) {
    const auto src = R"(
func fail() -> str => error(.timeout)
func drop(x: str) -> str { nil }
route GET "/users" {
    let host = fail() | drop(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_error_lhs_flows_into_error_stage_via_or) {
    const auto src = R"(
func fail() -> str => error(.timeout)
func failstage(x: str) -> str => error(.timeout)
route GET "/users" {
    let host = fail() | failstage(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_optional_lhs_flows_into_optional_error_stage_via_or) {
    const auto src = R"(
func tri(x: str) -> str {
    if x == "host" { nil } else { error(.timeout) }
}
route GET "/users" {
    let host = req.header("Host") | tri(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, source_pipe_runtime_error_lhs_flows_into_optional_error_stage_via_or) {
    const auto src = R"(
func fail() -> str => error(.timeout)
func tri(x: str) -> str {
    if x == "host" { nil } else { error(.timeout) }
}
route GET "/users" {
    let host = fail() | tri(_)
    let safe = or(host, "missing")
    return 200
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->routes[0].locals.len, 2u);
    CHECK(hir->routes[0].locals[0].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[0].may_nil);
    CHECK(hir->routes[0].locals[0].may_error);
    CHECK(hir->routes[0].locals[1].type == HirTypeKind::Str);
    CHECK_FALSE(hir->routes[0].locals[1].may_nil);
    CHECK_FALSE(hir->routes[0].locals[1].may_error);
}
TEST(frontend, lower_to_rir_supports_source_pipe_runtime_optional_error_value_flow) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { nil } else { error(.timeout) }
}
func id(x: i32) -> i32 => x
route GET "/users" {
    let code = maybefail(true) | id(_)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}
TEST(frontend, lower_to_rir_supports_source_runtime_optional_error_value_flow) {
    const auto src = R"(
func maybefail(ok: bool) -> i32 {
    if ok { nil } else { error(.timeout) }
}
route GET "/users" {
    let code = maybefail(true)
    let safe = or(code, 200)
    if safe == 200 { return 200 } else { return 500 }
}
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    FrontendRirModule rir{};
    auto lowered = lower_to_rir(mir.value(), rir);
    REQUIRE(lowered);
    rir.destroy();
}

TEST(frontend, mir_type_shapes_track_carrier_readiness_via_type_decls) {
    const auto src = R"(
struct Box { value: i32 }
func keep(x: (i32, bool), y: (Box, i32)) -> (i32, bool) => x
)";
    auto lexed = lex(lit(src));
    REQUIRE(lexed);
    auto ast = parse_file_heap(lexed.value());
    REQUIRE(ast);
    auto hir = analyze_file_heap(ast.value());
    REQUIRE(hir);
    REQUIRE_EQ(hir->functions.len, 1u);
    auto mir = build_mir_heap(hir.value());
    REQUIRE(mir);
    const auto scalar_tuple_shape = hir->functions[0].params[0].shape_index;
    const auto struct_tuple_shape = hir->functions[0].params[1].shape_index;
    REQUIRE(scalar_tuple_shape < mir->type_shapes.len);
    REQUIRE(struct_tuple_shape < mir->type_shapes.len);
    CHECK(mir->type_shapes[scalar_tuple_shape].is_concrete);
    CHECK(mir->type_shapes[scalar_tuple_shape].carrier_ready);
    CHECK(mir->type_shapes[struct_tuple_shape].is_concrete);
    CHECK(mir->type_shapes[struct_tuple_shape].carrier_ready);
}

int main(int argc, char** argv) {
    return rut::test::run_all(argc, argv);
}
