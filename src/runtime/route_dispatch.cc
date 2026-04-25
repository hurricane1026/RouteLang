#include "rut/runtime/route_dispatch.h"

#include "rut/runtime/route_table.h"

namespace rut {

namespace {

// Linear scan — same algorithm as the pre-PR RouteConfig::match()
// inline body. Pulled into a static function so the dispatch vtable
// can hold a stable pointer to it.
//
// Semantics (preserved verbatim from the inline version):
//   - First-match-wins across the routes[] array.
//   - Method filter: r.method == 0 matches any request method.
//   - Byte-prefix match: a route with path /api matches request
//     paths /api, /api/v1, /api?q=1, etc.
//   - No segment awareness — /api can match /apix as well, since
//     this impl predates segment-aware routing. Configs that need
//     segment semantics should be steered to a different dispatch
//     by the selector.
u16 linear_scan_match(const RouteConfig* cfg, Str path, u8 method) {
    for (u32 i = 0; i < cfg->route_count; i++) {
        const auto& r = cfg->routes[i];
        if (r.method != 0 && r.method != method) continue;
        if (path.len < r.path_len) continue;
        bool matched = true;
        for (u32 j = 0; j < r.path_len; j++) {
            if (static_cast<u8>(path.ptr[j]) != static_cast<u8>(r.path[j])) {
                matched = false;
                break;
            }
        }
        if (matched) return static_cast<u16>(i);
    }
    return kRouteIdxInvalid;
}

}  // namespace

const RouteDispatch kLinearScanDispatch = {&linear_scan_match};

}  // namespace rut
