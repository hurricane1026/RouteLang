#include "rut/jit/runtime_helpers.h"

#include "rut/runtime/connection.h"
#include "rut/runtime/http_parser.h"

using namespace rut;

// ── Request Access ─────────────────────────────────────────────────

void rut_helper_req_path(const u8* req_data, u32 req_len, const char** out_ptr, u32* out_len) {
    // Fast path: parse just enough to extract the path.
    HttpParser parser;
    ParsedRequest req;
    parser.reset();
    if (parser.parse(req_data, req_len, &req) == ParseStatus::Complete) {
        *out_ptr = req.path.ptr;
        *out_len = req.path.len;
        return;
    }

    // Fallback: minimal manual extraction.
    // Skip method (find first space), extract path until next space.
    u32 i = 0;
    while (i < req_len && req_data[i] != ' ') i++;
    if (i >= req_len) {
        *out_ptr = "/";
        *out_len = 1;
        return;
    }
    i++;  // skip space
    u32 path_start = i;
    while (i < req_len && req_data[i] != ' ' && req_data[i] != '?' && req_data[i] != '\r') {
        i++;
    }
    *out_ptr = reinterpret_cast<const char*>(req_data + path_start);
    *out_len = i - path_start;
}

u8 rut_helper_req_method(const u8* req_data, u32 req_len) {
    HttpParser parser;
    ParsedRequest req;
    parser.reset();
    if (parser.parse(req_data, req_len, &req) == ParseStatus::Complete) {
        return static_cast<u8>(req.method);
    }

    // Fallback: return Unknown
    return static_cast<u8>(HttpMethod::Unknown);
}

void rut_helper_req_header(const u8* req_data,
                           u32 req_len,
                           const char* name,
                           u32 name_len,
                           u8* out_has_value,
                           const char** out_ptr,
                           u32* out_len) {
    *out_has_value = 0;
    *out_ptr = nullptr;
    *out_len = 0;

    HttpParser parser;
    ParsedRequest req;
    parser.reset();
    if (parser.parse(req_data, req_len, &req) != ParseStatus::Complete) return;

    // Linear scan through parsed headers (case-insensitive name match).
    for (u32 i = 0; i < req.header_count; i++) {
        auto& h = req.headers[i];
        if (h.name.len != name_len) continue;
        bool match = true;
        for (u32 j = 0; j < name_len; j++) {
            u8 a = static_cast<u8>(h.name.ptr[j]);
            u8 b = static_cast<u8>(name[j]);
            // ASCII case-insensitive comparison
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            *out_has_value = 1;
            *out_ptr = h.value.ptr;
            *out_len = h.value.len;
            return;
        }
    }
}

u32 rut_helper_req_remote_addr(void* conn) {
    auto* c = static_cast<Connection*>(conn);
    return c->peer_addr;
}

// ── String Operations ──────────────────────────────────────────────

u8 rut_helper_str_has_prefix(const char* s, u32 s_len, const char* pfx, u32 pfx_len) {
    if (pfx_len > s_len) return 0;
    for (u32 i = 0; i < pfx_len; i++) {
        if (s[i] != pfx[i]) return 0;
    }
    return 1;
}

u8 rut_helper_str_eq(const char* a, u32 a_len, const char* b, u32 b_len) {
    if (a_len != b_len) return 0;
    for (u32 i = 0; i < a_len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

i32 rut_helper_str_cmp(const char* a, u32 a_len, const char* b, u32 b_len) {
    u32 n = a_len < b_len ? a_len : b_len;
    for (u32 i = 0; i < n; i++) {
        unsigned char ac = static_cast<unsigned char>(a[i]);
        unsigned char bc = static_cast<unsigned char>(b[i]);
        if (ac < bc) return -1;
        if (ac > bc) return 1;
    }
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

void rut_helper_str_trim_prefix(
    const char* s, u32 s_len, const char* pfx, u32 pfx_len, const char** out_ptr, u32* out_len) {
    if (pfx_len <= s_len) {
        bool match = true;
        for (u32 i = 0; i < pfx_len; i++) {
            if (s[i] != pfx[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            *out_ptr = s + pfx_len;
            *out_len = s_len - pfx_len;
            return;
        }
    }
    *out_ptr = s;
    *out_len = s_len;
}
