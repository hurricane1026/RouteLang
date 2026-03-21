#include "rout/runtime/io_uring_backend.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <string.h>  // memset
#include <sys/mman.h>
#include <sys/socket.h>  // SOCK_NONBLOCK, SOCK_CLOEXEC
#include <sys/syscall.h>
#include <unistd.h>

// IORING_OP_CANCEL may not be defined on older kernel headers
#ifndef IORING_OP_ASYNC_CANCEL
#define IORING_OP_ASYNC_CANCEL 14
#endif

#ifndef IORING_ASYNC_CANCEL_ALL
#define IORING_ASYNC_CANCEL_ALL (1U << 0)
#endif

namespace rout {

// --- Syscall wrappers (no liburing) ---

static i32 io_uring_setup(u32 entries, struct io_uring_params* p) {
    i32 ret = static_cast<i32>(syscall(__NR_io_uring_setup, entries, p));
    return ret >= 0 ? ret : -errno;
}

static i32 io_uring_enter(i32 fd, u32 to_submit, u32 min_complete, u32 flags) {
    i32 ret = static_cast<i32>(
        syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, nullptr, 0));
    return ret >= 0 ? ret : -errno;
}

static i32 io_uring_register(i32 fd, u32 opcode, const void* arg, u32 nr_args) {
    i32 ret = static_cast<i32>(syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
    return ret >= 0 ? ret : -errno;
}

// --- user_data encoding ---
// Layout: [63:8] = conn_id (up to 56 bits used, stored in u32 on decode), [7:0] = IoEventType
// Enough for 16M connections per shard.

u64 IoUringBackend::encode_user_data(u32 conn_id, IoEventType type) {
    return (static_cast<u64>(conn_id) << 8) | static_cast<u64>(type);
}

void IoUringBackend::decode_user_data(u64 data, u32& conn_id, IoEventType& type) {
    type = static_cast<IoEventType>(data & 0xFF);
    conn_id = static_cast<u32>(data >> 8);
}

// --- SQE helpers ---

io_uring_sqe* IoUringBackend::get_sqe() {
    u32 tail = __atomic_load_n(sq_tail, __ATOMIC_RELAXED);
    u32 head = __atomic_load_n(sq_head, __ATOMIC_ACQUIRE);
    u32 mask = *sq_ring_mask;

    if (tail - head >= sq_ring_entries) {
        return nullptr;  // SQ is full
    }

    io_uring_sqe* sqe = &sq_entries[tail & mask];
    sq_array[tail & mask] = tail & mask;
    return sqe;
}

static void sqe_advance_tail(u32* sq_tail) {
    u32 tail = __atomic_load_n(sq_tail, __ATOMIC_RELAXED);
    __atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
}

// --- Init ---

i32 IoUringBackend::init(u32 /*shard_id*/, i32 lfd) {
    listen_fd = lfd;

    // Setup io_uring with desired flags
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
    // Note: SQPOLL requires CAP_SYS_NICE or io_uring_register credentials.
    // Omit for now, add as optimization later.

    constexpr u32 kRingEntries = 16384;
    ring_fd = io_uring_setup(kRingEntries, &params);
    if (ring_fd < 0) return ring_fd;

    sq_ring_entries = params.sq_entries;
    cq_ring_entries = params.cq_entries;

    // Map SQ ring
    sq_ring_sz = params.sq_off.array + params.sq_entries * sizeof(u32);
    sq_ring_ptr = mmap(nullptr,
                       sq_ring_sz,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE,
                       ring_fd,
                       IORING_OFF_SQ_RING);
    if (sq_ring_ptr == MAP_FAILED) {
        shutdown();
        return -errno;
    }

    auto* sq_base = static_cast<u8*>(sq_ring_ptr);
    sq_head = reinterpret_cast<u32*>(sq_base + params.sq_off.head);
    sq_tail = reinterpret_cast<u32*>(sq_base + params.sq_off.tail);
    sq_ring_mask = reinterpret_cast<u32*>(sq_base + params.sq_off.ring_mask);
    sq_array = reinterpret_cast<u32*>(sq_base + params.sq_off.array);

    // Map SQEs
    sqes_sz = params.sq_entries * sizeof(io_uring_sqe);
    sqes_ptr = mmap(nullptr,
                    sqes_sz,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE,
                    ring_fd,
                    IORING_OFF_SQES);
    if (sqes_ptr == MAP_FAILED) {
        shutdown();
        return -errno;
    }
    sq_entries = static_cast<io_uring_sqe*>(sqes_ptr);

    // Map CQ ring
    cq_ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);
    cq_ring_ptr = mmap(nullptr,
                       cq_ring_sz,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE,
                       ring_fd,
                       IORING_OFF_CQ_RING);
    if (cq_ring_ptr == MAP_FAILED) {
        shutdown();
        return -errno;
    }

    auto* cq_base = static_cast<u8*>(cq_ring_ptr);
    cq_head = reinterpret_cast<u32*>(cq_base + params.cq_off.head);
    cq_tail = reinterpret_cast<u32*>(cq_base + params.cq_off.tail);
    cq_ring_mask = reinterpret_cast<u32*>(cq_base + params.cq_off.ring_mask);
    cq_entries = reinterpret_cast<io_uring_cqe*>(cq_base + params.cq_off.cqes);

    // Setup provided buffer ring for zero-copy recv
    i32 rc = setup_buf_ring();
    if (rc < 0) return rc;

    return 0;
}

// --- Provided buffer ring ---

i32 IoUringBackend::setup_buf_ring() {
    // Allocate buffer memory: kProvidedBufCount * kProvidedBufSize
    u64 total_buf_sz = static_cast<u64>(kProvidedBufCount) * kProvidedBufSize;
    buf_base = static_cast<u8*>(mmap(nullptr,
                                     total_buf_sz,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                     -1,
                                     0));
    if (buf_base == MAP_FAILED) {
        shutdown();
        return -errno;
    }

    // Allocate the buf_ring structure itself
    u64 ring_sz = sizeof(io_uring_buf_ring) + kProvidedBufCount * sizeof(io_uring_buf);
    void* ring_mem = mmap(nullptr,
                          ring_sz,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                          -1,
                          0);
    if (ring_mem == MAP_FAILED) {
        shutdown();
        return -errno;
    }
    buf_ring = static_cast<io_uring_buf_ring*>(ring_mem);

    // Register the buffer ring with io_uring
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof(reg));
    reg.ring_addr = reinterpret_cast<u64>(buf_ring);
    reg.ring_entries = kProvidedBufCount;
    reg.bgid = kBufGroupId;

    i32 rc = io_uring_register(ring_fd, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (rc < 0) {
        shutdown();
        return rc;
    }

    // Fill the ring with buffer entries
    for (u32 i = 0; i < kProvidedBufCount; i++) {
        io_uring_buf* buf = &buf_ring->bufs[i];
        buf->addr = reinterpret_cast<u64>(buf_base + static_cast<u64>(i) * kProvidedBufSize);
        buf->len = kProvidedBufSize;
        buf->bid = static_cast<u16>(i);
    }
    // Advance the ring tail to make all buffers available
    __atomic_store_n(&buf_ring->tail, static_cast<u16>(kProvidedBufCount), __ATOMIC_RELEASE);

    return 0;
}

void IoUringBackend::return_buffer(u16 buf_id) {
    // Add buffer back to the provided ring
    u16 tail = __atomic_load_n(&buf_ring->tail, __ATOMIC_RELAXED);
    u16 mask = kProvidedBufCount - 1;  // power of 2

    io_uring_buf* buf = &buf_ring->bufs[tail & mask];
    buf->addr = reinterpret_cast<u64>(buf_base + static_cast<u64>(buf_id) * kProvidedBufSize);
    buf->len = kProvidedBufSize;
    buf->bid = buf_id;

    __atomic_store_n(&buf_ring->tail, static_cast<u16>(tail + 1), __ATOMIC_RELEASE);
}

// --- Operations ---

void IoUringBackend::add_accept() {
    io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = listen_fd;
    sqe->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    sqe->ioprio = IORING_ACCEPT_MULTISHOT;  // multishot: one SQE, continuous accept
    sqe->user_data = encode_user_data(0, IoEventType::Accept);

    sqe_advance_tail(sq_tail);
    pending++;
}

void IoUringBackend::add_recv(i32 fd, u32 conn_id) {
    io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_RECV;
    sqe->fd = fd;
    sqe->len = kProvidedBufSize;  // max bytes to read from provided buffer
    sqe->buf_group = kBufGroupId;
    sqe->flags = IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;  // multishot: continuous recv
    sqe->user_data = encode_user_data(conn_id, IoEventType::Recv);

    sqe_advance_tail(sq_tail);
    pending++;
}

void IoUringBackend::add_send(i32 fd, u32 conn_id, const u8* buf, u32 len) {
    io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_SEND;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<u64>(buf);
    sqe->len = len;
    sqe->user_data = encode_user_data(conn_id, IoEventType::Send);

    sqe_advance_tail(sq_tail);
    pending++;
}

void IoUringBackend::add_connect(i32 fd, u32 conn_id, const void* addr, u32 addr_len) {
    io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_CONNECT;
    sqe->fd = fd;
    sqe->addr = reinterpret_cast<u64>(addr);
    sqe->off = addr_len;  // connect uses off field for addrlen
    sqe->user_data = encode_user_data(conn_id, IoEventType::UpstreamConnect);

    sqe_advance_tail(sq_tail);
    pending++;
}

void IoUringBackend::cancel(i32 fd, u32 conn_id) {
    io_uring_sqe* sqe = get_sqe();
    if (!sqe) return;

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = fd;
    // Cancel by user_data — matches any pending SQE with this conn_id for Recv
    sqe->addr = encode_user_data(conn_id, IoEventType::Recv);
    sqe->cancel_flags = IORING_ASYNC_CANCEL_ALL;
    sqe->user_data = 0;  // we don't care about the cancel completion

    sqe_advance_tail(sq_tail);
    pending++;
}

// --- Wait (submit + harvest) ---

u32 IoUringBackend::wait(IoEvent* events, u32 max_events) {
    // Submit pending SQEs and wait for at least 1 CQE
    u32 flags = IORING_ENTER_GETEVENTS;
    i32 ret;
    for (;;) {
        if (pending > 0) {
            ret = io_uring_enter(ring_fd, pending, 1, flags);
            if (ret >= 0) pending = 0;
        } else {
            ret = io_uring_enter(ring_fd, 0, 1, flags);
        }
        if (ret >= 0) break;
        if (ret == -EINTR) continue;
        return 0;  // real error
    }

    // Harvest CQEs
    u32 head = __atomic_load_n(cq_head, __ATOMIC_ACQUIRE);
    u32 tail = __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE);
    u32 mask = *cq_ring_mask;
    u32 count = 0;

    while (head != tail && count < max_events) {
        io_uring_cqe* cqe = &cq_entries[head & mask];

        u32 conn_id;
        IoEventType type;
        decode_user_data(cqe->user_data, conn_id, type);

        events[count].conn_id = conn_id;
        events[count].type = type;
        events[count].result = cqe->res;

        // Extract buffer ID from CQE flags (for provided buffer ring)
        if (cqe->flags & IORING_CQE_F_BUFFER) {
            events[count].buf_id = static_cast<u16>(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            events[count].has_buf = 1;
        } else {
            events[count].buf_id = 0;
            events[count].has_buf = 0;
        }

        head++;
        count++;
    }

    __atomic_store_n(cq_head, head, __ATOMIC_RELEASE);
    return count;
}

// --- Shutdown ---

void IoUringBackend::shutdown() {
    if (buf_base != nullptr) {
        munmap(buf_base, static_cast<u64>(kProvidedBufCount) * kProvidedBufSize);
        buf_base = nullptr;
    }
    if (buf_ring != nullptr) {
        u64 ring_sz = sizeof(io_uring_buf_ring) + kProvidedBufCount * sizeof(io_uring_buf);
        munmap(buf_ring, ring_sz);
        buf_ring = nullptr;
    }
    if (sqes_ptr != nullptr) {
        munmap(sqes_ptr, sqes_sz);
        sqes_ptr = nullptr;
    }
    if (cq_ring_ptr != nullptr) {
        munmap(cq_ring_ptr, cq_ring_sz);
        cq_ring_ptr = nullptr;
    }
    if (sq_ring_ptr != nullptr) {
        munmap(sq_ring_ptr, sq_ring_sz);
        sq_ring_ptr = nullptr;
    }
    if (ring_fd >= 0) {
        close(ring_fd);
        ring_fd = -1;
    }
}

}  // namespace rout
