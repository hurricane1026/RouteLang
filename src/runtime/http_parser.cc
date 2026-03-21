#include "rout/runtime/http_parser.h"

namespace rout {

// ============================================================================
// Lookup tables for character classification
// ============================================================================
//
// Borrowed technique from picohttpparser (Kazuho Oku, H2O):
// Instead of switch/case chains, a 256-byte table maps each byte to
// valid/invalid in one branch-free lookup.
//
// RFC 7230 §3.2.6 token characters:
//   "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
//   "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
//
// 0 = invalid, 1 = valid token character.

// clang-format off
static const u8 kTokenTable[256] = {
    //  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x00-0x0F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x1F
    0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,  // 0x20-0x2F  SP ! " # $ % & ' ( ) * + , - . /
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  // 0x30-0x3F  0-9 : ; < = > ?
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x4F  @ A-O
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,  // 0x50-0x5F  P-Z [ \ ] ^ _
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x6F  ` a-o
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,  // 0x70-0x7F  p-z { | } ~ DEL
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x80-0x8F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x90-0x9F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xA0-0xAF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xB0-0xBF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xC0-0xCF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xD0-0xDF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xE0-0xEF
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0xF0-0xFF
};
// clang-format on

// Header value: any visible ASCII + SP + HT (RFC 7230 §3.2)
// 0 = invalid, 1 = valid header-value character.
// clang-format off
static const u8 kHeaderValueTable[256] = {
    //  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,  // 0x00-0x0F  (HT=0x09 allowed)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x1F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20-0x2F  SP and printable
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x30-0x3F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x4F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x50-0x5F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x6F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,  // 0x70-0x7F  (DEL=0x7F invalid)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x80-0x8F  (obs-text)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x90-0x9F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xA0-0xAF
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xB0-0xBF
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xC0-0xCF
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xD0-0xDF
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xE0-0xEF
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0xF0-0xFF
};
// clang-format on

// URI characters: everything except CTL and SP (RFC 7230 §3.1.1)
// This is intentionally permissive — we let the router or handler
// do stricter validation.
// clang-format off
static const u8 kUriTable[256] = {
    //  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x00-0x0F
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x10-0x1F
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20-0x2F  (SP=0x20 not valid)
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x30-0x3F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x40-0x4F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x50-0x5F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x60-0x6F
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,  // 0x70-0x7F  (DEL invalid)
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // 0x80+: invalid
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
// clang-format on

// ============================================================================
// Helper functions
// ============================================================================

static inline bool is_digit(u8 c) {
    return c >= '0' && c <= '9';
}

// Case-insensitive string comparison for header names.
static bool str_ci_eq(const u8* a, const char* b, u32 len) {
    for (u32 i = 0; i < len; i++) {
        if ((a[i] | 0x20) != (static_cast<u8>(b[i]) | 0x20)) return false;
    }
    return true;
}

// Parse unsigned decimal integer from string. Returns -1 on overflow/error.
static i64 parse_uint(const u8* p, u32 len) {
    if (len == 0) return -1;
    u64 val = 0;
    for (u32 i = 0; i < len; i++) {
        if (!is_digit(p[i])) return -1;
        u64 next = val * 10 + (p[i] - '0');
        if (next < val) return -1;  // overflow
        val = next;
    }
    if (val > 0xFFFFFFFF) return -1;  // too large for u32
    return static_cast<i64>(val);
}

// ============================================================================
// Method parsing
// ============================================================================

// Quick method parse using first-character dispatch + length check.
// Handles the 9 standard HTTP methods.
static HttpMethod parse_method(const u8* p, u32 len) {
    switch (len) {
        case 3:
            if (p[0] == 'G' && p[1] == 'E' && p[2] == 'T') return HttpMethod::GET;
            if (p[0] == 'P' && p[1] == 'U' && p[2] == 'T') return HttpMethod::PUT;
            break;
        case 4:
            if (p[0] == 'P' && p[1] == 'O' && p[2] == 'S' && p[3] == 'T') return HttpMethod::POST;
            if (p[0] == 'H' && p[1] == 'E' && p[2] == 'A' && p[3] == 'D') return HttpMethod::HEAD;
            break;
        case 5:
            if (p[0] == 'P' && p[1] == 'A' && p[2] == 'T' && p[3] == 'C' && p[4] == 'H')
                return HttpMethod::PATCH;
            if (p[0] == 'T' && p[1] == 'R' && p[2] == 'A' && p[3] == 'C' && p[4] == 'E')
                return HttpMethod::TRACE;
            break;
        case 6:
            if (p[0] == 'D' && p[1] == 'E' && p[2] == 'L' && p[3] == 'E' && p[4] == 'T' &&
                p[5] == 'E')
                return HttpMethod::DELETE;
            break;
        case 7:
            if (p[0] == 'O' && p[1] == 'P' && p[2] == 'T' && p[3] == 'I' && p[4] == 'O' &&
                p[5] == 'N' && p[6] == 'S')
                return HttpMethod::OPTIONS;
            if (p[0] == 'C' && p[1] == 'O' && p[2] == 'N' && p[3] == 'N' && p[4] == 'E' &&
                p[5] == 'C' && p[6] == 'T')
                return HttpMethod::CONNECT;
            break;
    }
    return HttpMethod::Unknown;
}

// ============================================================================
// Core parser
// ============================================================================

// Find \r\n\r\n in buffer starting from `from`. Returns offset past the
// terminator (first body byte), or 0 if not found.
static u32 find_header_end(const u8* buf, u32 len, u32 from) {
    // Start scanning from `from`, but we need at least 3 bytes before
    // current position to check \r\n\r\n, so adjust.
    u32 start = from > 3 ? from - 3 : 0;
    for (u32 i = start; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

ParseStatus HttpParser::parse(const u8* buf, u32 len, ParsedRequest* req) {
    // First: do we have the complete header block (\r\n\r\n)?
    u32 end = find_header_end(buf, len, parsed_offset);
    if (end == 0) {
        parsed_offset = len;
        return ParseStatus::Incomplete;
    }

    // We have a complete header block. Parse it from the beginning.
    req->reset();
    u32 pos = 0;

    // --- Request line: METHOD SP URI SP HTTP/x.y CR LF ---

    // Parse method
    u32 method_start = pos;
    while (pos < end && buf[pos] != ' ') {
        if (!kTokenTable[buf[pos]]) return ParseStatus::Error;
        pos++;
    }
    if (pos == method_start || pos >= end) return ParseStatus::Error;

    req->method = parse_method(buf + method_start, pos - method_start);
    if (req->method == HttpMethod::Unknown) return ParseStatus::Error;
    pos++;  // skip SP

    // Parse URI
    u32 uri_start = pos;
    while (pos < end && buf[pos] != ' ') {
        if (!kUriTable[buf[pos]]) return ParseStatus::Error;
        pos++;
    }
    if (pos == uri_start || pos >= end) return ParseStatus::Error;

    req->path = {reinterpret_cast<const char*>(buf + uri_start), pos - uri_start};
    pos++;  // skip SP

    // Parse HTTP version: "HTTP/1.0" or "HTTP/1.1"
    if (pos + 10 > end) return ParseStatus::Error;  // "HTTP/1.x\r\n" = 10 bytes
    if (buf[pos] != 'H' || buf[pos + 1] != 'T' || buf[pos + 2] != 'T' || buf[pos + 3] != 'P' ||
        buf[pos + 4] != '/') {
        return ParseStatus::Error;
    }
    if (buf[pos + 5] == '1' && buf[pos + 6] == '.' && buf[pos + 7] == '1') {
        req->version = HttpVersion::Http11;
    } else if (buf[pos + 5] == '1' && buf[pos + 6] == '.' && buf[pos + 7] == '0') {
        req->version = HttpVersion::Http10;
    } else {
        return ParseStatus::Error;
    }
    pos += 8;

    // Expect CRLF
    if (pos + 2 > end || buf[pos] != '\r' || buf[pos + 1] != '\n') return ParseStatus::Error;
    pos += 2;

    // --- Headers ---
    // Default keep-alive: HTTP/1.1 = true, HTTP/1.0 = false.
    req->keep_alive = (req->version == HttpVersion::Http11);

    while (pos < end) {
        // Check for end of headers (\r\n)
        if (buf[pos] == '\r') {
            if (pos + 1 < end && buf[pos + 1] == '\n') {
                break;  // Done — this is the final \r\n
            }
            return ParseStatus::Error;
        }

        // Header name: token characters until ':'
        u32 name_start = pos;
        while (pos < end && buf[pos] != ':') {
            if (!kTokenTable[buf[pos]]) return ParseStatus::Error;
            pos++;
        }
        if (pos == name_start || pos >= end) return ParseStatus::Error;
        u32 name_len = pos - name_start;
        pos++;  // skip ':'

        // Skip optional whitespace (OWS) after ':'
        while (pos < end && (buf[pos] == ' ' || buf[pos] == '\t')) pos++;

        // Header value: until \r\n
        u32 value_start = pos;
        while (pos + 1 < end && !(buf[pos] == '\r' && buf[pos + 1] == '\n')) {
            if (!kHeaderValueTable[buf[pos]]) return ParseStatus::Error;
            pos++;
        }
        if (pos + 1 >= end) return ParseStatus::Error;

        // Trim trailing OWS from value
        u32 value_end = pos;
        while (value_end > value_start &&
               (buf[value_end - 1] == ' ' || buf[value_end - 1] == '\t')) {
            value_end--;
        }

        // Store header
        if (req->header_count >= kMaxHeaders) return ParseStatus::Error;

        Header* h = &req->headers[req->header_count++];
        h->name = {reinterpret_cast<const char*>(buf + name_start), name_len};
        h->value = {reinterpret_cast<const char*>(buf + value_start), value_end - value_start};

        // Recognize important headers inline during parsing.
        // This avoids a second pass over headers.
        if (name_len == 14 && str_ci_eq(buf + name_start, "content-length", 14)) {
            i64 cl = parse_uint(buf + value_start, value_end - value_start);
            if (cl < 0) return ParseStatus::Error;
            req->content_length = static_cast<u32>(cl);
        } else if (name_len == 10 && str_ci_eq(buf + name_start, "connection", 10)) {
            u32 vlen = value_end - value_start;
            if (vlen == 10 && str_ci_eq(buf + value_start, "keep-alive", 10)) {
                req->keep_alive = true;
            } else if (vlen == 5 && str_ci_eq(buf + value_start, "close", 5)) {
                req->keep_alive = false;
            }
        } else if (name_len == 17 && str_ci_eq(buf + name_start, "transfer-encoding", 17)) {
            u32 vlen = value_end - value_start;
            if (vlen >= 7 && str_ci_eq(buf + value_start + vlen - 7, "chunked", 7)) {
                req->chunked = true;
            }
        }

        pos += 2;  // skip \r\n
    }

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
