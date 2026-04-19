#pragma once

#include "core/expected.h"
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
    // Returns 0 if the body table or pool is full.
    u16 add_response_body(const char* data, u32 len) {
        if (response_body_count >= kMaxResponseBodies) return 0;
        if (body_pool_used + len > kResponseBodyPoolBytes) return 0;
        char* dst = body_pool + body_pool_used;
        for (u32 i = 0; i < len; i++) dst[i] = data[i];
        body_pool_used += len;
        const u32 idx = response_body_count++;
        response_bodies[idx] = {dst, len};
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
