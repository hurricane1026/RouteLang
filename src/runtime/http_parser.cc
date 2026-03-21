#include "rout/runtime/http_parser.h"

#include "runtime/simd/char_tables.h"
#include "runtime/simd/simd.h"

namespace rout {

// ============================================================================
// Branch prediction
// ============================================================================

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// ============================================================================
// Integer-based helpers
// ============================================================================

static inline u32 load_u32(const u8* p) {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline u64 load_u64(const u8* p) {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static constexpr u32 u32_lit(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
           (static_cast<u32>(static_cast<u8>(c)) << 16) |
           (static_cast<u32>(static_cast<u8>(d)) << 24);
}

static constexpr u64 u64_lit(const char* s) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<u64>(static_cast<u8>(s[i])) << (i * 8);
    return v;
}

// ============================================================================
// str_ci_eq — u64 batch comparison
// ============================================================================

static inline bool str_ci_eq(const u8* a, const char* b, u32 len) {
    constexpr u64 kMask8 = 0x2020202020202020ULL;
    constexpr u32 kMask4 = 0x20202020U;
    u32 i = 0;
    for (; i + 8 <= len; i += 8) {
        u64 va = load_u64(a + i);
        u64 vb;
        __builtin_memcpy(&vb, b + i, 8);
        if ((va | kMask8) != (vb | kMask8)) return false;
    }
    if (i + 4 <= len) {
        u32 va = load_u32(a + i);
        u32 vb;
        __builtin_memcpy(&vb, b + i, 4);
        if ((va | kMask4) != (vb | kMask4)) return false;
        i += 4;
    }
    for (; i < len; i++) {
        if ((a[i] | 0x20) != (static_cast<u8>(b[i]) | 0x20)) return false;
    }
    return true;
}

// ============================================================================
// parse_uint — branchless digit check
// ============================================================================

static inline i64 parse_uint(const u8* p, u32 len) {
    if (UNLIKELY(len == 0)) return -1;
    u64 val = 0;
    for (u32 i = 0; i < len; i++) {
        u32 d = p[i] - '0';
        if (UNLIKELY(d > 9)) return -1;
        u64 next = val * 10 + d;
        if (UNLIKELY(next < val)) return -1;
        val = next;
    }
    if (UNLIKELY(val > 0xFFFFFFFF)) return -1;
    return static_cast<i64>(val);
}

// ============================================================================
// Method parsing — integer compare
// ============================================================================

static inline HttpMethod parse_method(const u8* p, u32 len) {
    switch (len) {
        case 3:
            // Compare "GET" or "PUT" — only first 3 bytes matter
            if (p[0] == 'G' && load_u32(p) == (u32_lit('G', 'E', 'T', p[3])))
                return HttpMethod::GET;
            if (p[0] == 'P' && p[1] == 'U' && p[2] == 'T') return HttpMethod::PUT;
            break;
        case 4: {
            u32 v = load_u32(p);
            if (v == u32_lit('P', 'O', 'S', 'T')) return HttpMethod::POST;
            if (v == u32_lit('H', 'E', 'A', 'D')) return HttpMethod::HEAD;
            break;
        }
        case 5:
            if (load_u32(p) == u32_lit('P', 'A', 'T', 'C') && p[4] == 'H') return HttpMethod::PATCH;
            if (load_u32(p) == u32_lit('T', 'R', 'A', 'C') && p[4] == 'E') return HttpMethod::TRACE;
            break;
        case 6:
            if (load_u32(p) == u32_lit('D', 'E', 'L', 'E') && p[4] == 'T' && p[5] == 'E')
                return HttpMethod::DELETE;
            break;
        case 7: {
            u32 lo = load_u32(p);
            if (lo == u32_lit('O', 'P', 'T', 'I') && p[4] == 'O' && p[5] == 'N' && p[6] == 'S')
                return HttpMethod::OPTIONS;
            if (lo == u32_lit('C', 'O', 'N', 'N') && p[4] == 'E' && p[5] == 'C' && p[6] == 'T')
                return HttpMethod::CONNECT;
            break;
        }
    }
    return HttpMethod::Unknown;
}

// ============================================================================
// Version constants
// ============================================================================

static constexpr u64 kHttp11 = u64_lit("HTTP/1.1");
static constexpr u64 kHttp10 = u64_lit("HTTP/1.0");

// ============================================================================
// Inline fast-path scanners for short data
// These avoid function call overhead for typical short header names/values.
// Fall through to SIMD for long data.
// ============================================================================

// Scan for ':' validating token chars. Handles first 32 bytes inline.
static inline u32 fast_scan_header_name(const u8* buf, u32 pos, u32 end) {
    // Fast scalar path: most header names are < 24 bytes
    u32 fast_end = end - pos > 24 ? pos + 24 : end;
    while (pos < fast_end) {
        u8 c = buf[pos];
        if (c == ':') return pos;
        if (UNLIKELY(!kTokenTable[c])) return static_cast<u32>(-1);
        pos++;
    }
    // Long name — fall into SIMD
    return simd::scan_header_name(buf, pos, end);
}

// Header value: go straight to SIMD. Values are often long (User-Agent, Cookie, etc.)
// Scalar prefix would slow down the common case.
static inline u32 fast_scan_header_value(const u8* buf, u32 pos, u32 end) {
    return simd::scan_header_value(buf, pos, end);
}

// ============================================================================
// Semantic header matching — first-byte + length dispatch
// Avoids str_ci_eq call for most headers.
// ============================================================================

// Connection: close / keep-alive
static inline void match_connection(const u8* val, u32 vlen, ParsedRequest* req) {
    if (vlen == 5) {
        // "close" — check as u32 + 1 byte
        u32 lo = load_u32(val) | 0x20202020U;
        if (lo == u32_lit('c', 'l', 'o', 's') && (val[4] | 0x20) == 'e') {
            req->keep_alive = false;
        }
    } else if (vlen == 10) {
        // "keep-alive"
        u64 v = load_u64(val) | 0x2020202020202020ULL;
        if (v == u64_lit("keep-ali") && (val[8] | 0x20) == 'v' && (val[9] | 0x20) == 'e') {
            req->keep_alive = true;
        }
    }
}

// Check and apply semantic headers inline.
// Returns quickly for non-semantic headers via first-byte + length dispatch.
static inline ParseStatus apply_semantic_header(
    const u8* name, u32 name_len, const u8* val, u32 vlen, ParsedRequest* req) {
    // Dispatch on (first_byte | 0x20) and length for fast rejection
    u8 first = name[0] | 0x20;

    if (first == 'c') {
        if (name_len == 14 && str_ci_eq(name + 1, "ontent-length", 13)) {
            i64 cl = parse_uint(val, vlen);
            if (UNLIKELY(cl < 0)) return ParseStatus::Error;
            req->content_length = static_cast<u32>(cl);
            return ParseStatus::Complete;
        }
        if (name_len == 10 && str_ci_eq(name + 1, "onnection", 9)) {
            match_connection(val, vlen, req);
            return ParseStatus::Complete;
        }
    } else if (first == 't') {
        if (name_len == 17 && str_ci_eq(name + 1, "ransfer-encoding", 16)) {
            if (vlen >= 7 && str_ci_eq(val + vlen - 7, "chunked", 7)) {
                req->chunked = true;
            }
            return ParseStatus::Complete;
        }
    }
    return ParseStatus::Complete;  // not a semantic header — no-op
}

// ============================================================================
// Core parser
// ============================================================================

ParseStatus HttpParser::parse(const u8* buf, u32 len, ParsedRequest* req) {
    u32 end = simd::find_header_end(buf, len, parsed_offset);
    if (end == 0) {
        parsed_offset = len;
        return ParseStatus::Incomplete;
    }

    req->reset();
    u32 pos = 0;

    // --- Request line ---

    // Method: short, always uppercase, scan for space
    u32 method_start = pos;
    while (LIKELY(pos < end) && buf[pos] >= 'A' && buf[pos] <= 'Z') pos++;
    if (UNLIKELY(pos == method_start || pos >= end || buf[pos] != ' ')) return ParseStatus::Error;

    req->method = parse_method(buf + method_start, pos - method_start);
    if (UNLIKELY(req->method == HttpMethod::Unknown)) return ParseStatus::Error;
    pos++;

    // URI — SIMD accelerated
    u32 uri_start = pos;
    u32 uri_end = simd::scan_uri(buf, pos, end);
    if (UNLIKELY(uri_end == static_cast<u32>(-1) || uri_end == uri_start || uri_end >= end))
        return ParseStatus::Error;
    req->path = {reinterpret_cast<const char*>(buf + uri_start), uri_end - uri_start};
    pos = uri_end + 1;

    // Version — single u64 compare
    if (UNLIKELY(pos + 10 > end)) return ParseStatus::Error;
    u64 ver = load_u64(buf + pos);
    if (LIKELY(ver == kHttp11)) {
        req->version = HttpVersion::Http11;
        req->keep_alive = true;
    } else if (ver == kHttp10) {
        req->version = HttpVersion::Http10;
        req->keep_alive = false;
    } else {
        return ParseStatus::Error;
    }
    pos += 8;

    if (UNLIKELY(buf[pos] != '\r' || buf[pos + 1] != '\n')) return ParseStatus::Error;
    pos += 2;

    // --- Headers ---
    u32 hdr_count = 0;

    while (LIKELY(pos < end)) {
        // End of headers?
        if (buf[pos] == '\r') {
            if (LIKELY(buf[pos + 1] == '\n')) break;
            return ParseStatus::Error;
        }

        // Header name — inline fast path
        u32 name_start = pos;
        u32 colon_pos = fast_scan_header_name(buf, pos, end);
        if (UNLIKELY(colon_pos == static_cast<u32>(-1) || colon_pos == name_start))
            return ParseStatus::Error;
        u32 name_len = colon_pos - name_start;
        pos = colon_pos + 1;

        // Skip OWS — optimized for common case (single space)
        if (LIKELY(pos < end && buf[pos] == ' ')) {
            pos++;
            // Rare: multiple spaces or tabs
            while (UNLIKELY(pos < end && (buf[pos] == ' ' || buf[pos] == '\t'))) pos++;
        } else {
            while (pos < end && buf[pos] == '\t') pos++;
        }

        // Header value — inline fast path
        u32 value_start = pos;
        u32 cr_pos = fast_scan_header_value(buf, pos, end);
        if (UNLIKELY(cr_pos == static_cast<u32>(-1) || cr_pos + 1 >= end ||
                     buf[cr_pos + 1] != '\n'))
            return ParseStatus::Error;
        pos = cr_pos;

        // Trim trailing OWS — scan backward
        u32 value_end = pos;
        while (value_end > value_start &&
               (buf[value_end - 1] == ' ' || buf[value_end - 1] == '\t')) {
            value_end--;
        }

        // Store header
        if (UNLIKELY(hdr_count >= kMaxHeaders)) return ParseStatus::Error;
        Header* h = &req->headers[hdr_count++];
        h->name = {reinterpret_cast<const char*>(buf + name_start), name_len};
        h->value = {reinterpret_cast<const char*>(buf + value_start), value_end - value_start};

        // Semantic header detection — fast first-byte dispatch
        ParseStatus sem = apply_semantic_header(
            buf + name_start, name_len, buf + value_start, value_end - value_start, req);
        if (UNLIKELY(sem == ParseStatus::Error)) return ParseStatus::Error;

        pos += 2;  // skip \r\n
    }

    req->header_count = hdr_count;
    header_end = end;
    return ParseStatus::Complete;
}

// ============================================================================
// Utility
// ============================================================================

Str http_method_str(HttpMethod m) {
    switch (m) {
        case HttpMethod::GET:
            return {"GET", 3};
        case HttpMethod::POST:
            return {"POST", 4};
        case HttpMethod::PUT:
            return {"PUT", 3};
        case HttpMethod::DELETE:
            return {"DELETE", 6};
        case HttpMethod::PATCH:
            return {"PATCH", 5};
        case HttpMethod::HEAD:
            return {"HEAD", 4};
        case HttpMethod::OPTIONS:
            return {"OPTIONS", 7};
        case HttpMethod::CONNECT:
            return {"CONNECT", 7};
        case HttpMethod::TRACE:
            return {"TRACE", 5};
        case HttpMethod::Unknown:
            return {"UNKNOWN", 7};
    }
    return {"UNKNOWN", 7};
}

}  // namespace rout
