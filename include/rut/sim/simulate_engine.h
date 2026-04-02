#pragma once

#include "rut/common/types.h"
#include "rut/compiler/rir.h"
#include "rut/jit/handler_abi.h"
#include "rut/jit/jit_engine.h"
#include "rut/runtime/access_log.h"
#include "rut/runtime/arena.h"
#include "rut/runtime/connection.h"
#include "rut/runtime/http_parser.h"
#include "rut/runtime/traffic_capture.h"
#include "rut/runtime/traffic_replay.h"

namespace rut::sim {

struct ManifestUpstream {
    u16 id = 0;
    char name[32]{};
};

enum class ManifestAction : u8 {
    ReturnStatus = 0,
    Proxy = 1,
};

struct ManifestRoute {
    u8 method = 0;  // 0 = any, otherwise RouteConfig-style first-char match.
    char pattern[128]{};
    ManifestAction action = ManifestAction::ReturnStatus;
    u16 status_code = 200;
    u16 upstream_id = 0;
};

struct Manifest {
    static constexpr u32 kMaxRoutes = 128;
    static constexpr u32 kMaxUpstreams = 64;

    ManifestRoute routes[kMaxRoutes];
    u32 route_count = 0;

    ManifestUpstream upstreams[kMaxUpstreams];
    u32 upstream_count = 0;
};

// Compiler-owned storage for building an RIR module from a manifest.
struct ModuleContext {
    MmapArena arena;
    rir::Module module{};

    bool init(u32 func_cap, u32 struct_cap = 1);
    void destroy();
};

// Read a simple simulate manifest from disk.
//
// Grammar:
//   upstream <id> <name>
//   route <METHOD|ANY> <pattern> status <code>
//   route <METHOD|ANY> <pattern> proxy <upstream-id>
//
// Tokens are whitespace-separated; blank lines and '#' comments are ignored.
bool load_manifest(const char* path, Manifest& out);

// Build a sync-only RIR module that mirrors the manifest.
bool build_module_from_manifest(const Manifest& manifest, ModuleContext& ctx);

enum class Verdict : u8 {
    Match,
    Mismatch,
    Failed,
    Unsupported,
};

struct SimulateResult {
    u8 method = static_cast<u8>(LogHttpMethod::Other);
    char path[64]{};

    jit::HandlerAction action = jit::HandlerAction::ReturnStatus;
    u16 expected_status = 0;
    u16 actual_status = 0;
    char expected_upstream[32]{};
    char actual_upstream[32]{};

    Verdict verdict = Verdict::Failed;
};

struct SimulateSummary {
    u32 total = 0;
    u32 matched = 0;
    u32 mismatched = 0;
    u32 failed = 0;
    u32 unsupported = 0;
};

struct Engine {
    static constexpr u32 kMaxRoutes = Manifest::kMaxRoutes;
    static constexpr u32 kMaxUpstreams = Manifest::kMaxUpstreams;

    struct CompiledRoute {
        u8 method = 0;
        char pattern[128]{};
        u32 pattern_len = 0;
        jit::HandlerFn fn = nullptr;
    };

    CompiledRoute routes[kMaxRoutes];
    u32 route_count = 0;

    ManifestUpstream upstreams[kMaxUpstreams];
    u32 upstream_count = 0;

    jit::JitEngine jit;

    bool init(const rir::Module& module, const ManifestUpstream* upstream_list, u32 upstreams_len);
    void shutdown();
};

SimulateResult simulate_one(Engine& engine, const CaptureEntry& entry);
SimulateSummary simulate_file(Engine& engine, ReplayReader& reader);

u32 format_result(const SimulateResult& result, char* buf, u32 buf_size);
u32 format_summary(const SimulateSummary& summary, char* buf, u32 buf_size);

}  // namespace rut::sim
