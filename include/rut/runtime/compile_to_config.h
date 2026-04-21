#pragma once

// Bridge header: copy the RIR module's declarative content (upstreams
// with addresses, response bodies, response header sets) into a
// RouteConfig so the runtime can serve the compiled handlers.
//
// This helper intentionally does NOT register route entries — those
// require the JitEngine to have compiled and looked up each handler
// function, which is orthogonal to the declarative content here.
// Callers typically:
//   1. jit::codegen(rir.module) → LLVM IR module
//   2. engine.compile(...); engine.lookup("handler_route_N") for each
//   3. populate_route_config(cfg, rir.module) for upstreams / bodies /
//      header sets
//   4. cfg.add_jit_handler(path, method, fn) per route
//
// Returns true on full success. On any partial failure (capacity
// exceeded, validation rejected) the function stops and returns false;
// `cfg` may have been partially populated, so callers should discard
// it rather than try to reuse it.

#include "rut/compiler/rir.h"
#include "rut/runtime/route_table.h"

namespace rut {

inline bool populate_route_config(RouteConfig& cfg, const rir::Module& mod) {
    // Helper assumes cfg starts empty — otherwise newly-added slots
    // would not begin at index 0, breaking the compiler's
    // declaration-order upstream_id / body_idx / headers_idx
    // invariant. Refuse rather than silently produce a mis-aligned
    // config. Callers that need to start from a partially-populated
    // cfg should wire things up manually.
    if (cfg.route_count != 0 || cfg.upstream_count != 0 || cfg.response_body_count != 0 ||
        cfg.response_header_set_count != 0) {
        return false;
    }

    // Defensive bounds-checks against malformed modules (e.g. a
    // hand-built rir::Module with inconsistent counts). Refuse before
    // we dereference anything out of range.
    if (mod.upstream_count > rir::Module::kMaxUpstreams) return false;
    if (mod.response_body_count > rir::Module::kMaxResponseBodies) return false;
    if (mod.header_set_count > rir::Module::kMaxHeaderSets) return false;
    if (mod.header_pool_used > rir::Module::kMaxHeaderPoolEntries) return false;
    for (u32 i = 0; i < mod.header_set_count; i++) {
        const auto& ref = mod.header_sets[i];
        if (static_cast<u32>(ref.offset) + ref.count > mod.header_pool_used) return false;
        if (ref.count > RouteConfig::kMaxHeadersPerSet) return false;
    }

    // Upstreams: the compiler emits `forward(name)` as a 0-based index
    // into the declaration order (0 = first `upstream` decl, 1 = second,
    // …). `RouteConfig::upstreams` is also declaration-order (add_upstream
    // appends). So for the indices to stay aligned, we either add an
    // entry per DSL upstream OR refuse to populate at all.
    //
    // Refusing to skip name-only upstreams: if any upstream in the
    // module lacks an address, we fail-fast. Mixing DSL-addressed and
    // name-only upstreams would desync compile-time `upstream_index`
    // from cfg slot, sending `forward(a)` to whichever backend happened
    // to occupy slot 0 after compaction. Callers with name-only
    // upstreams should wire them up manually, in DSL declaration order.
    // (Mixed-mode support would need an in-place RouteConfig::
    // bind_upstream_at(idx, ip, port) API — not worth adding until a
    // concrete use case motivates it.)
    for (u32 i = 0; i < mod.upstream_count; i++) {
        if (!mod.upstreams[i].has_address) return false;
    }
    for (u32 i = 0; i < mod.upstream_count; i++) {
        const auto& up = mod.upstreams[i];
        // add_upstream's name parameter is a NUL-terminated C string;
        // rir::Module stores Str (ptr + len) where the bytes may not
        // be NUL-terminated (arena-allocated slices). Copy into a
        // small fixed buffer matching UpstreamTarget::kMaxUpstreamNameLen
        // so the underlying set_name sees a terminator. Truncate
        // names that exceed the buffer — matches the silent-truncate
        // behavior of UpstreamTarget::set_name so the helper doesn't
        // introduce a new runtime-only failure mode. (A hard limit
        // should be enforced at the frontend with a compile-time
        // diagnostic, not here.)
        char name_buf[UpstreamTarget::kMaxUpstreamNameLen];
        // Defensive null guard: a hand-built rir::Module could set
        // len > 0 with a null ptr; the copy below would segfault.
        // lower_rir never produces this shape, but the helper is
        // reachable from arbitrary callers so fail closed.
        if (up.name.len > 0 && up.name.ptr == nullptr) return false;
        // Compute copy_len as min(len, cap-1) with no `len + 1`
        // arithmetic — that expression would wrap for a hand-built
        // module with len == 0xffffffff, making the subsequent copy
        // loop run far past name_buf.
        u32 copy_len = up.name.len;
        if (copy_len >= sizeof(name_buf)) copy_len = sizeof(name_buf) - 1;
        for (u32 j = 0; j < copy_len; j++) name_buf[j] = up.name.ptr[j];
        name_buf[copy_len] = '\0';
        auto r = cfg.add_upstream(name_buf, up.ip, up.port);
        if (!r.has_value()) return false;
        // The helper only makes sense when cfg starts empty — assert
        // that the newly-added slot matches the DSL's compile-time
        // index i, so forward() resolves correctly at runtime.
        if (r.value() != i) return false;
    }

    // Response bodies (1-based index preserved). Empty bodies don't
    // appear in the module table (lower_rir skips them), so we can
    // feed the bytes straight through.
    for (u32 i = 0; i < mod.response_body_count; i++) {
        const auto& body = mod.response_bodies[i];
        u16 idx = cfg.add_response_body(body.ptr, body.len);
        if (idx == 0) return false;
        // Belt-and-suspenders: the 1-based index must match i+1 so
        // callers that packed body_idx at compile time still resolve
        // correctly. add_response_body assigns sequentially, so this
        // invariant holds iff we start from an empty cfg.
        if (idx != i + 1) return false;
    }

    // Response header sets (1-based index preserved). Materialise the
    // (key, value) pointer tables add_response_header_set expects.
    for (u32 i = 0; i < mod.header_set_count; i++) {
        const auto& ref = mod.header_sets[i];
        const char* keys[RouteConfig::kMaxHeadersPerSet];
        u32 key_lens[RouteConfig::kMaxHeadersPerSet];
        const char* vals[RouteConfig::kMaxHeadersPerSet];
        u32 val_lens[RouteConfig::kMaxHeadersPerSet];
        if (ref.count > RouteConfig::kMaxHeadersPerSet) return false;
        for (u16 j = 0; j < ref.count; j++) {
            const auto& k = mod.header_keys[ref.offset + j];
            const auto& v = mod.header_values[ref.offset + j];
            keys[j] = k.ptr;
            key_lens[j] = k.len;
            vals[j] = v.ptr;
            val_lens[j] = v.len;
        }
        u16 idx = cfg.add_response_header_set(keys, key_lens, vals, val_lens, ref.count);
        if (idx == 0) return false;
        if (idx != i + 1) return false;
    }
    return true;
}

}  // namespace rut
