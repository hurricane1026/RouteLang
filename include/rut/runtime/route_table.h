#pragma once

#include "core/expected.h"
#include "rut/common/http_header_validation.h"
#include "rut/common/types.h"
#include "rut/jit/handler_abi.h"
#include "rut/runtime/error.h"
#include "rut/runtime/route_dispatch.h"
#include "rut/runtime/route_trie.h"

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

    // Non-copyable: the embedded `trie` stores non-owning Str views
    // pointing into routes[].path. A by-value copy would leave the
    // copy's trie referencing the original's path buffers — a use-
    // after-free as soon as the original is modified or destroyed.
    // RouteConfig is published via `const RouteConfig*` for RCU swap;
    // there's no production codepath that needs to copy one.
    RouteConfig() = default;
    RouteConfig(const RouteConfig&) = delete;
    RouteConfig& operator=(const RouteConfig&) = delete;

    RouteEntry routes[kMaxRoutes];
    u32 route_count = 0;

    // Pluggable route lookup. Set BEFORE the first add_* call (ideally
    // by the compiler-side selector at config-build time); the active
    // dispatch determines which per-impl state add_* populates. Once
    // any route has been added, set_dispatch() refuses to swap — the
    // earlier routes wouldn't be in the new dispatch's data structure
    // and lookups would silently miss them (Codex P1 caught this on
    // #43). Build a fresh RouteConfig if you need a different dispatch.
    const RouteDispatch* dispatch() const { return dispatch_; }
    bool set_dispatch(const RouteDispatch* d) {
        if (route_count > 0 || d == nullptr) return false;
        dispatch_ = d;
        return true;
    }

    // Segment-aware radix trie. Populated by add_* only when the
    // active dispatch is kSegmentTrieDispatch. ~1.2 MB inline; sized
    // to cover 128 routes × 32 distinct segments at the worst-case
    // (no prefix sharing). When a different dispatch is selected this
    // storage sits unused — a follow-up PR will move per-impl state
    // into a tagged union so only the active impl pays its cost.
    RouteTrie trie;
    static_assert(kMaxRoutes == TrieNode::kMaxChildren,
                  "RouteConfig::kMaxRoutes must equal TrieNode::kMaxChildren so a config "
                  "whose routes all share a single parent fits the trie's per-node fan-out.");

    UpstreamTarget upstreams[kMaxUpstreams];
    u32 upstream_count = 0;

    // Reject route paths that aren't in origin-form. Required by the
    // segment trie (which would otherwise silently mismatch malformed
    // configs); the linear-scan default tolerates any string but
    // applying the same gate uniformly keeps add_* semantics
    // consistent across dispatch choices.
    //   - Must be non-null, non-empty, and start with '/'. An empty
    //     string and "api" without a leading slash are rejected
    //     rather than implicitly normalized — the trie's root and
    //     "/api" terminal would otherwise collide silently.
    //   - Must not contain '?' or '#': those mark query/fragment in a
    //     URI and routing doesn't match on them. RouteTrie::match()
    //     strips them from incoming requests.
    //   - Must terminate within kMaxPathLen.
    static bool is_routable_path(const char* path) {
        if (path == nullptr || path[0] != '/') return false;
        for (u32 i = 0; i < RouteEntry::kMaxPathLen; i++) {
            const char ch = path[i];
            if (ch == '\0') return true;
            if (ch == '?' || ch == '#') return false;
        }
        return false;
    }

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

    // Populate the active dispatch's state with a newly-written
    // routes[route_count] entry. Returns false on a structural
    // capacity hit in that dispatch's data structure (e.g., trie
    // node-pool exhaustion). NOOP when the active dispatch reads
    // routes[] directly (linear scan), so route admission for the
    // default dispatch is never gated on a non-active impl's limits
    // — matches the documented "pay only for the active dispatch"
    // contract that the storage refactor will make literal.
    //
    // Per-impl branches stay narrow: the body of each branch is
    // exactly the call to that impl's `insert`. As more dispatches
    // land we add a branch here; the rest of add_* doesn't change.
    bool populate_dispatch_state(const RouteEntry& r) {
        const Str path_view{r.path, r.path_len};
        const u16 idx = static_cast<u16>(route_count);
        if (dispatch_ == &kSegmentTrieDispatch) {
            return trie.insert(path_view, r.method, idx);
        }
        // kLinearScanDispatch: routes[] IS the data.
        return true;
    }

    // Add a proxy route: path prefix → upstream target.
    // Returns false if:
    //   - the route table is full,
    //   - upstream_id is out of range,
    //   - the path is malformed (see is_routable_path),
    //   - the path is too long for RouteEntry::path,
    //   - the method byte isn't recognized,
    //   - the active dispatch's state ran out of capacity.
    bool add_proxy(const char* path, u8 method, u16 upstream_id) {
        if (route_count >= kMaxRoutes) return false;
        if (upstream_id >= upstream_count) return false;
        if (!is_routable_path(path)) return false;
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
        if (!populate_dispatch_state(r)) {
            return false;  // active dispatch at capacity — fail loud
        }
        route_count++;
        return true;
    }

    // Add a static response route. Same failure modes as add_proxy(),
    // minus the upstream-id check that doesn't apply here.
    bool add_static(const char* path, u8 method, u16 status) {
        if (route_count >= kMaxRoutes) return false;
        if (!is_routable_path(path)) return false;
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
        if (!populate_dispatch_state(r)) {
            return false;
        }
        route_count++;
        return true;
    }

    // Add a JIT-handler route. Handler is invoked on match; its HandlerResult
    // tells the runtime what to do next (return status, forward, or yield).
    // Same failure modes as add_proxy() plus null-fn check.
    bool add_jit_handler(const char* path, u8 method, jit::HandlerFn fn) {
        if (route_count >= kMaxRoutes) return false;
        if (fn == nullptr) return false;
        if (!is_routable_path(path)) return false;
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
        if (!populate_dispatch_state(r)) {
            return false;
        }
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
    // Returns 0 on any of:
    //   - count == 0 or null pointer tables
    //   - null data + non-zero len for any pair
    //   - capacity failure: sets table, (key, value) arrays (per-set
    //     cap = kMaxHeadersPerSet), or bytes pool
    //   - validation failure: key fails the HTTP tchar grammar, value
    //     contains control chars, or key names a reserved framing
    //     header (Content-Length / Transfer-Encoding / Connection)
    //   - duplicate: two keys in the set compare equal under ASCII
    //     case folding (parity with the DSL parser's dup-reject)
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

    // Match a request path. Semantics depend on the chosen dispatch
    // (`this->dispatch`), but the default linear-scan dispatch keeps
    // the historical contract: first-match-wins byte-prefix scan,
    // method 0 in a route entry matches any request method, and
    // unmatched requests return nullptr (callers fall back to the
    // default 200 OK handler).
    //
    // `method_char` is the first byte of the HTTP method ('G'=GET,
    // 'P'=POST/PUT/PATCH, 'D'=DELETE, 'H'=HEAD, 'O'=OPTIONS,
    // 'C'=CONNECT, 'T'=TRACE) or 0 for "any".
    const RouteEntry* match(const u8* path_data, u32 path_len, u8 method_char) const {
        const Str path{reinterpret_cast<const char*>(path_data), path_len};
        const u16 idx = dispatch_->match(this, path, method_char);
        if (idx >= route_count) return nullptr;  // covers kRouteIdxInvalid
        return &routes[idx];
    }

private:
    // The active dispatch vtable, set via set_dispatch() and read via
    // dispatch(). Private so callers can't assign past the route_count
    // == 0 gate — see set_dispatch() doc.
    const RouteDispatch* dispatch_ = &kLinearScanDispatch;
};

}  // namespace rut
