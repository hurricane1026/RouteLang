#include "char_tables.h"
#include "simd.h"

namespace rout::simd {

u32 find_header_end(const u8* buf, u32 len, u32 from) {
    u32 start = from > 3 ? from - 3 : 0;
    for (u32 i = start; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

u32 scan_header_value(const u8* buf, u32 pos, u32 end) {
    while (pos < end) {
        if (buf[pos] == '\r') return pos;
        if (!kHeaderValueTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return end;
}

u32 scan_uri(const u8* buf, u32 pos, u32 end) {
    while (pos < end) {
        if (buf[pos] == ' ') return pos;
        if (!kUriTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return end;
}

u32 scan_header_name(const u8* buf, u32 pos, u32 end) {
    while (pos < end) {
        if (buf[pos] == ':') return pos;
        if (!kTokenTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return static_cast<u32>(-1);
}

}  // namespace rout::simd
