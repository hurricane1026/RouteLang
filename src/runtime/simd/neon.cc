#include "char_tables.h"
#include "simd.h"

#include <arm_neon.h>

namespace rout::simd {

// NEON has no movemask. Extract per-byte bitmask via narrowing shift.
static inline u64 neon_movemask(uint8x16_t v) {
    // Narrow 16x8 → 8x8 with shift, packing each byte's high bit into 4-bit nibbles.
    uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(v), 4);
    return vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
    // Each matching byte produces 0xF nibble, non-matching produces 0x0.
    // Use ctzll / 4 to find byte index.
}

static inline bool neon_any(uint8x16_t v) {
    return vmaxvq_u8(v) != 0;
}

static inline u32 neon_first_set(u64 mask) {
    return static_cast<u32>(__builtin_ctzll(mask)) >> 2;
}

// Build a 16-bit bitmask (one bit per byte) from NEON comparison result.
// More precise than nibble approach when we need mask arithmetic.
static inline u32 neon_bitmask16(uint8x16_t v) {
    // Pack each byte's MSB into a 16-bit integer.
    // Use pairwise add to reduce: 16 bytes → 8 shorts → extract.
    static const uint8x16_t kBitSelect = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t masked = vandq_u8(v, kBitSelect);
    // Add pairs: byte[0]+byte[1], byte[2]+byte[3], ...
    uint8x8_t lo = vget_low_u8(masked);
    uint8x8_t hi = vget_high_u8(masked);
    uint8x8_t sum_lo = vpadd_u8(lo, lo);  // 4 values
    uint8x8_t sum_hi = vpadd_u8(hi, hi);
    sum_lo = vpadd_u8(sum_lo, sum_lo);  // 2 values
    sum_hi = vpadd_u8(sum_hi, sum_hi);
    sum_lo = vpadd_u8(sum_lo, sum_lo);  // 1 value
    sum_hi = vpadd_u8(sum_hi, sum_hi);
    return static_cast<u32>(vget_lane_u8(sum_lo, 0)) |
           (static_cast<u32>(vget_lane_u8(sum_hi, 0)) << 8);
}

u32 find_header_end(const u8* buf, u32 len, u32 from) {
    u32 start = from > 3 ? from - 3 : 0;
    u32 pos = start;
    const uint8x16_t vcr = vdupq_n_u8('\r');

    while (pos + 16 <= len) {
        uint8x16_t chunk = vld1q_u8(buf + pos);
        uint8x16_t cmp = vceqq_u8(chunk, vcr);
        if (neon_any(cmp)) {
            // Found at least one \r — check each position
            for (u32 i = 0; i < 16 && pos + i + 3 < len; i++) {
                if (buf[pos + i] == '\r' && buf[pos + i + 1] == '\n' && buf[pos + i + 2] == '\r' &&
                    buf[pos + i + 3] == '\n') {
                    return pos + i + 4;
                }
            }
        }
        pos += 16;
    }
    for (; pos + 3 < len; pos++) {
        if (buf[pos] == '\r' && buf[pos + 1] == '\n' && buf[pos + 2] == '\r' &&
            buf[pos + 3] == '\n')
            return pos + 4;
    }
    return 0;
}

u32 scan_header_value(const u8* buf, u32 pos, u32 end) {
    const uint8x16_t vcr = vdupq_n_u8('\r');
    const uint8x16_t v20 = vdupq_n_u8(0x20);
    const uint8x16_t vht = vdupq_n_u8(0x09);
    const uint8x16_t vdel = vdupq_n_u8(0x7F);

    while (pos + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(buf + pos);
        uint8x16_t cr_match = vceqq_u8(chunk, vcr);
        // vcltq_u8 is unsigned < (NEON advantage over SSE2)
        uint8x16_t lt20 = vcltq_u8(chunk, v20);
        uint8x16_t is_ht = vceqq_u8(chunk, vht);
        uint8x16_t bad_ctl = vbicq_u8(lt20, is_ht);  // lt20 AND NOT is_ht
        uint8x16_t is_del = vceqq_u8(chunk, vdel);
        uint8x16_t bad = vorrq_u8(bad_ctl, is_del);

        if (neon_any(vorrq_u8(cr_match, bad))) {
            // Scalar fallback for the 16-byte window
            for (u32 i = 0; i < 16 && pos + i < end; i++) {
                if (buf[pos + i] == '\r') return pos + i;
                if (!kHeaderValueTable[buf[pos + i]]) return static_cast<u32>(-1);
            }
        }
        pos += 16;
    }
    while (pos < end) {
        if (buf[pos] == '\r') return pos;
        if (!kHeaderValueTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return end;
}

u32 scan_uri(const u8* buf, u32 pos, u32 end) {
    const uint8x16_t vsp = vdupq_n_u8(' ');
    const uint8x16_t v21 = vdupq_n_u8(0x21);
    const uint8x16_t v7f = vdupq_n_u8(0x7F);

    while (pos + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(buf + pos);
        uint8x16_t sp_match = vceqq_u8(chunk, vsp);
        uint8x16_t bad = vorrq_u8(vcltq_u8(chunk, v21), vceqq_u8(chunk, v7f));

        if (neon_any(vorrq_u8(sp_match, bad))) {
            for (u32 i = 0; i < 16 && pos + i < end; i++) {
                if (buf[pos + i] == ' ') return pos + i;
                if (!kUriTable[buf[pos + i]]) return static_cast<u32>(-1);
            }
        }
        pos += 16;
    }
    while (pos < end) {
        if (buf[pos] == ' ') return pos;
        if (!kUriTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return end;
}

u32 scan_header_name(const u8* buf, u32 pos, u32 end) {
    const uint8x16_t vcolon = vdupq_n_u8(':');

    while (pos + 16 <= end) {
        uint8x16_t chunk = vld1q_u8(buf + pos);
        uint8x16_t cmp = vceqq_u8(chunk, vcolon);
        if (neon_any(cmp)) {
            for (u32 i = 0; i < 16 && pos + i < end; i++) {
                if (buf[pos + i] == ':') return pos + i;
                if (!kTokenTable[buf[pos + i]]) return static_cast<u32>(-1);
            }
        }
        for (u32 j = pos; j < pos + 16; j++) {
            if (!kTokenTable[buf[j]]) return static_cast<u32>(-1);
        }
        pos += 16;
    }
    while (pos < end) {
        if (buf[pos] == ':') return pos;
        if (!kTokenTable[buf[pos]]) return static_cast<u32>(-1);
        pos++;
    }
    return static_cast<u32>(-1);
}

}  // namespace rout::simd
