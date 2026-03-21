#pragma once

#include "rout/common/types.h"

namespace rout {

// --- HTTP method enum (sequential values) ---
enum class HttpMethod : u8 {
    GET = 0,
    POST = 1,
    PUT = 2,
    DELETE = 3,
    PATCH = 4,
    HEAD = 5,
    OPTIONS = 6,
    CONNECT = 7,
    TRACE = 8,
    Unknown = 255,
};

enum class HttpVersion : u8 {
    Http10 = 0,
    Http11 = 1,
    Unknown = 255,
};

// A single HTTP header: name + value, both non-owning views into recv_buf.
struct Header {
    Str name;
    Str value;
};

// Maximum headers we store per request. Beyond this, parsing returns error.
static constexpr u32 kMaxHeaders = 64;

// Result of an incremental parse call.
enum class ParseStatus : u8 {
    Complete,    // Full request-line + headers parsed; body may follow.
    Incomplete,  // Need more data — call parse() again after next recv.
    Error,       // Malformed request — close connection.
};

// Parsed HTTP request — all Str fields point into the original recv buffer.
// Zero-copy: no allocations, no memcpy for method/path/headers.
struct ParsedRequest {
    HttpMethod method;
    Str path;  // e.g. "/api/users?id=1"
    HttpVersion version;

    Header headers[kMaxHeaders];
    u32 header_count;

    u32 content_length;       // From Content-Length header, 0 if absent.
    bool keep_alive;          // Derived from Connection header + HTTP version.
    bool chunked;             // Transfer-Encoding: chunked
    bool has_content_length;  // True if Content-Length header was seen.

    void reset() {
        method = HttpMethod::Unknown;
        path = {nullptr, 0};
        version = HttpVersion::Unknown;
        header_count = 0;
        content_length = 0;
        keep_alive = false;
        chunked = false;
        has_content_length = false;
    }
};

// Incremental HTTP/1.x request parser.
//
// Usage:
//   HttpParser parser;
//   parser.reset();
//   // On each recv:
//   ParseStatus s = parser.parse(buf, len, &request);
//   if (s == ParseStatus::Complete) { /* headers done, body at buf + parser.header_end */ }
//   if (s == ParseStatus::Incomplete) { /* wait for more data */ }
//   if (s == ParseStatus::Error) { /* 400, close connection */ }
//
// Design: single-pass, always starts from buf[0]. Each call is a full
// reparse of the provided buffer. On Complete, `header_end` is the offset
// past the final \r\n\r\n (first body byte).
struct HttpParser {
    u32 header_end;  // Set on Complete: offset of first body byte.

    void reset() { header_end = 0; }

    // Parse request-line + headers from buf[0..len).
    // On Complete, populates `req` and sets `header_end`.
    ParseStatus parse(const u8* buf, u32 len, ParsedRequest* req);
};

// --- Utility ---

// Convert HttpMethod enum to string (for logging/responses).
Str http_method_str(HttpMethod m);

}  // namespace rout
