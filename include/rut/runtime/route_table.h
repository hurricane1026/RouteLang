#pragma once

#include "core/expected.h"
#include "rut/common/http_header_validation.h"
#include "rut/common/types.h"
#include "rut/jit/handler_abi.h"
#include "rut/runtime/error.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>

namespace rut {

// Action for a matched route.
enum class RouteAction : u8 {
    Static,      // respond with fixed status (e.g., 200 OK, 404)
    Proxy,       // forward to upstream target
    JitHandler,  // invoke JIT-compiled handler, may yield for I/O/timer
};

// Upstream target — address:port for a backend server.
struct UpstreamTarget {
    static constexpr u32 kMaxUpstreamNameLen = 32;

    struct sockaddr_in addr;
    // Short name for logging/debugging (e.g., "api-v1")
    char name[kMaxUpstreamNameLen];
    u32 name_len;

    void set_name(const char* n) {
        name_len = 0;
        while (n[name_len] && name_len < sizeof(name) - 1) {
            name[name_len] = n[name_len];
            name_len++;
        }
        name[name_len] = '\0';
    }

    // Helper: set address from IP (host order) + port (host order)
    void set_addr(u32 ip, u16 port) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = __builtin_bswap32(ip);
        addr.sin_port = __builtin_bswap16(port);
    }
};

// Single route entry: matches method + path prefix → action.
struct RouteEntry {
    static constexpr u32 kMaxPathLen = 128;

    // Match criteria
    char path[kMaxPathLen];  // path prefix (e.g., "/api/v1/")
    u32 path_len;
    u8 method;  // 0 = any, 'G' = GET, 'P' = POST, etc. (first char)

    // Action
    RouteAction action;
    u16 upstream_id;              // index into RouteConfig::upstreams (if action == Proxy)
    u16 status_code;              // status code (if action == Static, e.g., 200, 404)
    jit::HandlerFn fn = nullptr;  // JIT-compiled handler (if action == JitHandler)
};

// RouteConfig — immutable after construction, atomically swappable.
// Contains route entries + upstream targets. The entire config is replaced
// on hot reload (RCU pattern: new config built, atomic swap, old reclaimed).
//
// Phase 2: simple linear scan (adequate for <100 routes).
// Phase 3: radix trie from Rutlang compiler (O(path_length) lookup).
struct RouteConfig {
    static constexpr u32 kMaxRoutes = 128;
    static constexpr u32 kMaxUpstreams = 64;
    // Response-body table. Populated at compile/config time; referenced
    // by JIT handlers via a 1-based index packed into
    // HandlerResult.upstream_id for ReturnStatus (0 = no custom body,
    // use the default status reason phrase).
    static constexpr u32 kMaxResponseBodies = 128;
    static constexpr u32 kResponseBodyPoolBytes = 8 * 1024;
    // Response-headers tables. Parallel to response_bodies but
    // independently indexed (JIT handlers pack a separate `headers_idx`
    // into HandlerResult.next_state for ReturnStatus, 0 = no custom
    // headers). All (key, value) pairs across every set share one flat
    // pool; each set is an (offset, count) slice into it. Header bytes
    // live in a single char pool so they outlive the RouteConfig's
    // RCU lifetime the same way body_pool does.
    static constexpr u32 kMaxResponseHeaderSets = 128;
    static constexpr u32 kMaxHeaderPoolEntries = 512;
    static constexpr u32 kResponseHeaderBytesPoolBytes = 8 * 1024;
    // Per-response cap for header count. Bigger than what the AST
    // permits (16) so hand-built RouteConfigs — tests, future
    // compile→config helper — have headroom, but small enough that the
    // dispatch code can materialise a stack-local KV array without
    // any risk of silent truncation. Must match the buffer size used
    // by handle_jit_outcome in callbacks_impl.h.
    static constexpr u32 kMaxHeadersPerSet = 32;

    RouteEntry routes[kMaxRoutes];
    u32 route_count = 0;

    UpstreamTarget upstreams[kMaxUpstreams];
    u32 upstream_count = 0;

    // Body entries point into body_pool; pool is a bump-allocated char
    // buffer so body bytes live alongside the config and get reclaimed
    // with it during RCU swap.
    struct ResponseBody {
        const char* data;
        u32 len;
    };
    ResponseBody response_bodies[kMaxResponseBodies];
    u32 response_body_count = 0;
    char body_pool[kResponseBodyPoolBytes];
    u32 body_pool_used = 0;

    // Header entries point into header_bytes_pool; the pool is a
    // bump-allocated char buffer shared by all keys and values. Each
    // pair-entry lives in header_keys[] / header_values[] with pointers
    // into the bytes pool; each response's header set is a slice of
    // those arrays described by HeaderSetRef.
    struct HeaderEntry {
        const char* data;
        u32 len;
    };
    struct HeaderSetRef {
        u16 offset;  // into header_keys / header_values
        u16 count;
    };
    HeaderEntry header_keys[kMaxHeaderPoolEntries];
    HeaderEntry header_values[kMaxHeaderPoolEntries];
    u32 header_pool_used = 0;
    HeaderSetRef response_header_sets[kMaxResponseHeaderSets];
    u32 response_header_set_count = 0;
    char header_bytes_pool[kResponseHeaderBytesPoolBytes];
    u32 header_bytes_pool_used = 0;

    // Add a proxy route: path prefix → upstream target.
    // Returns false if table full, upstream_id invalid, or path too long.
    bool add_proxy(const char* path, u8 method, u16 upstream_id) {
        if (route_count >= kMaxRoutes) return false;
        if (upstream_id >= upstream_count) return false;
        auto& r = routes[route_count];
        r.path_len = 0;
        while (path[r.path_len] && r.path_len < sizeof(r.path) - 1) {
            r.path[r.path_len] = path[r.path_len];
            r.path_len++;
        }
        if (path[r.path_len] != '\0') return false;  // path too long (truncated)
        r.path[r.path_len] = '\0';
        r.method = method;
        r.action = RouteAction::Proxy;
        r.upstream_id = upstream_id;
        r.status_code = 0;
        r.fn = nullptr;
        route_count++;
        return true;
    }

    // Add a static response route. Returns false if table full or path too long.
    bool add_static(const char* path, u8 method, u16 status) {
        if (route_count >= kMaxRoutes) return false;
        auto& r = routes[route_count];
        r.path_len = 0;
        while (path[r.path_len] && r.path_len < sizeof(r.path) - 1) {
            r.path[r.path_len] = path[r.path_len];
            r.path_len++;
        }
        if (path[r.path_len] != '\0') return false;  // path too long
        r.path[r.path_len] = '\0';
        r.method = method;
        r.action = RouteAction::Static;
        r.upstream_id = 0;
        r.status_code = status;
        r.fn = nullptr;
        route_count++;
        return true;
    }

    // Add a JIT-handler route. Handler is invoked on match; its HandlerResult
    // tells the runtime what to do next (return status, forward, or yield).
    // Returns false if table full, path too long, or fn is null.
    bool add_jit_handler(const char* path, u8 method, jit::HandlerFn fn) {
        if (route_count >= kMaxRoutes) return false;
        if (fn == nullptr) return false;
        auto& r = routes[route_count];
        r.path_len = 0;
        while (path[r.path_len] && r.path_len < sizeof(r.path) - 1) {
            r.path[r.path_len] = path[r.path_len];
            r.path_len++;
        }
        if (path[r.path_len] != '\0') return false;  // path too long
        r.path[r.path_len] = '\0';
        r.method = method;
        r.action = RouteAction::JitHandler;
        r.upstream_id = 0;
        r.status_code = 0;
        r.fn = fn;
        route_count++;
        return true;
    }

    // Register a response body. Copies the bytes into body_pool so the
    // caller doesn't need to keep the source alive. Returns a 1-based
    // index (0 is reserved as "no body") that JIT handlers can encode
    // in the HandlerResult upstream_id slot for ReturnStatus.
    // Returns 0 if the body table or pool is full, or if the arguments
    // are nonsensical (null data with non-zero len).
    u16 add_response_body(const char* data, u32 len) {
        if (response_body_count >= kMaxResponseBodies) return 0;
        if (len > 0 && data == nullptr) return 0;
        // Subtraction-based capacity check: `body_pool_used + len`
        // would wrap on a large u32 `len` and silently pass, leading
        // to an out-of-bounds write into body_pool.
        if (len > kResponseBodyPoolBytes - body_pool_used) return 0;
        char* dst = body_pool + body_pool_used;
        for (u32 i = 0; i < len; i++) dst[i] = data[i];
        body_pool_used += len;
        const u32 idx = response_body_count++;
        response_bodies[idx] = {dst, len};
        return static_cast<u16>(idx + 1);  // 1-based; 0 reserved
    }

    // Register a response header set. `keys[i]` / `key_lens[i]` and
    // `values[i]` / `value_lens[i]` describe the i-th pair (i in
    // [0, count)). Bytes are copied into header_bytes_pool so callers
    // don't need to keep the source alive. Returns a 1-based index
    // (0 reserved as "no custom headers") that JIT handlers encode in
    // HandlerResult.next_state for ReturnStatus.
    //
    // Returns 0 on any capacity failure (sets table, key/value array,
    // or bytes pool) or if arguments are nonsensical (count > 0 with
    // null pointer table, or null data + non-zero len for a pair).
    u16 add_response_header_set(const char* const* keys,
                                const u32* key_lens,
                                const char* const* values,
                                const u32* value_lens,
                                u32 count) {
        if (count == 0) return 0;
        if (keys == nullptr || values == nullptr || key_lens == nullptr || value_lens == nullptr) {
            return 0;
        }
        if (response_header_set_count >= kMaxResponseHeaderSets) return 0;
        // Per-set cap is enforced here so the dispatch formatter (which
        // uses a fixed stack buffer sized to kMaxHeadersPerSet) can
        // never silently drop trailing pairs.
        if (count > kMaxHeadersPerSet) return 0;
        // Subtraction-based capacity check on the (key, value) arrays.
        if (count > kMaxHeaderPoolEntries - header_pool_used) return 0;
        // Tally total bytes we're about to write; reject if the bytes
        // pool can't fit them. Also validate each (key, value) pair
        // via the shared HTTP header validator — parity with the DSL
        // parser — so manual callers can't accidentally emit malformed
        // or smuggling-prone responses (CR/LF injection, CL/TE
        // conflicts, etc.). Doing the scan up front avoids a partial
        // copy aborting mid-way with half-written state.
        u32 total_bytes = 0;
        for (u32 i = 0; i < count; i++) {
            if ((key_lens[i] > 0 && keys[i] == nullptr) ||
                (value_lens[i] > 0 && values[i] == nullptr)) {
                return 0;
            }
            if (validate_response_header(keys[i], key_lens[i], values[i], value_lens[i]) !=
                HttpHeaderValidation::Ok) {
                return 0;
            }
            // Case-insensitive duplicate-key check — parity with the
            // DSL parser. Two singletons with the same field name
            // (any case) would make the wire response ambiguous, so
            // we reject before allocating.
            for (u32 j = 0; j < i; j++) {
                if (http_header_name_eq_ci(keys[i], key_lens[i], keys[j], key_lens[j])) {
                    return 0;
                }
            }
            // Guard each add individually against u32 overflow.
            if (key_lens[i] > 0xffffffffu - total_bytes) return 0;
            total_bytes += key_lens[i];
            if (value_lens[i] > 0xffffffffu - total_bytes) return 0;
            total_bytes += value_lens[i];
        }
        if (total_bytes > kResponseHeaderBytesPoolBytes - header_bytes_pool_used) return 0;
        const u16 offset = static_cast<u16>(header_pool_used);
        for (u32 i = 0; i < count; i++) {
            char* key_dst = header_bytes_pool + header_bytes_pool_used;
            for (u32 j = 0; j < key_lens[i]; j++) key_dst[j] = keys[i][j];
            header_bytes_pool_used += key_lens[i];
            char* val_dst = header_bytes_pool + header_bytes_pool_used;
            for (u32 j = 0; j < value_lens[i]; j++) val_dst[j] = values[i][j];
            header_bytes_pool_used += value_lens[i];
            header_keys[offset + i] = {key_dst, key_lens[i]};
            header_values[offset + i] = {val_dst, value_lens[i]};
        }
        header_pool_used += count;
        const u32 idx = response_header_set_count++;
        response_header_sets[idx] = {offset, static_cast<u16>(count)};
        return static_cast<u16>(idx + 1);  // 1-based; 0 reserved
    }

    // Add an upstream target. Returns its index, or error if at capacity.
    core::Expected<u32, Error> add_upstream(const char* name, u32 ip, u16 port) {
        if (upstream_count >= kMaxUpstreams)
            return core::make_unexpected(Error::make(ENOSPC, Error::Source::RouteTable));
        u32 idx = upstream_count++;
        upstreams[idx].set_name(name);
        upstreams[idx].set_addr(ip, port);
        return idx;
    }

    // Match a request path (prefix match, first match wins).
    // method_char: first char of HTTP method ('G'=GET, 'P'=POST, etc.), 0=any.
    // Returns pointer to matching entry, or nullptr for no match (→ default 200 OK).
    const RouteEntry* match(const u8* path_data, u32 path_len, u8 method_char) const {
        for (u32 i = 0; i < route_count; i++) {
            auto& r = routes[i];
            // Method filter: 0 = any
            if (r.method != 0 && r.method != method_char) continue;
            // Prefix match
            if (path_len < r.path_len) continue;
            bool matched = true;
            for (u32 j = 0; j < r.path_len; j++) {
                if (path_data[j] != static_cast<u8>(r.path[j])) {
                    matched = false;
                    break;
                }
            }
            if (matched) return &r;
        }
        return nullptr;  // no match → default handler
    }
};

}  // namespace rut
