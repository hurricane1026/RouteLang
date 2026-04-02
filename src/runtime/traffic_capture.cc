#include "rut/runtime/traffic_capture.h"

#include <errno.h>
#include <unistd.h>

namespace rut {

void capture_file_header_init(CaptureFileHeader* hdr) {
    __builtin_memset(hdr, 0, sizeof(*hdr));
    hdr->magic[0] = 'R';
    hdr->magic[1] = 'U';
    hdr->magic[2] = 'T';
    hdr->magic[3] = 'C';
    hdr->magic[4] = 'A';
    hdr->magic[5] = 'P';
    hdr->magic[6] = '0';
    hdr->magic[7] = '1';
    hdr->version = 1;
    hdr->entry_size = sizeof(CaptureEntry);
}

bool capture_file_header_valid(const CaptureFileHeader* hdr) {
    return hdr->magic[0] == 'R' && hdr->magic[1] == 'U' && hdr->magic[2] == 'T' &&
           hdr->magic[3] == 'C' && hdr->magic[4] == 'A' && hdr->magic[5] == 'P' &&
           hdr->magic[6] == '0' && hdr->magic[7] == '1' && hdr->version == 1 &&
           hdr->entry_size == sizeof(CaptureEntry);
}

i32 capture_write_entry(i32 fd, const CaptureEntry& entry) {
    const u8* p = reinterpret_cast<const u8*>(&entry);
    u32 remaining = sizeof(CaptureEntry);
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        remaining -= static_cast<u32>(n);
    }
    return 0;
}

i32 capture_read_entry(i32 fd, CaptureEntry& entry) {
    u8* p = reinterpret_cast<u8*>(&entry);
    u32 remaining = sizeof(CaptureEntry);
    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        remaining -= static_cast<u32>(n);
    }
    return 0;
}

}  // namespace rut
