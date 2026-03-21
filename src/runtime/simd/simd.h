#pragma once

#include "rout/common/types.h"

namespace rout::simd {

// Find \r\n\r\n in buffer starting from `from`.
// Returns offset past terminator (first body byte), or 0 if not found.
u32 find_header_end(const u8* buf, u32 len, u32 from);

// Scan header value: find \r while validating chars.
// Returns position of \r, or `end` if not found, or (u32)-1 on invalid char.
u32 scan_header_value(const u8* buf, u32 pos, u32 end);

// Scan URI: find space while validating.
// Returns position of space, or `end` if not found, or (u32)-1 on invalid.
u32 scan_uri(const u8* buf, u32 pos, u32 end);

// Scan header name: find ':' while validating token chars.
// Returns position of ':', or (u32)-1 on invalid/not found.
u32 scan_header_name(const u8* buf, u32 pos, u32 end);

}  // namespace rout::simd
