#include "fault_injection.h"

#include <atomic>

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace rut::test_fault {
namespace {

thread_local FaultState g_state{};

using MmapFn = void* (*)(void*, size_t, int, int, int, off_t);
using MprotectFn = int (*)(void*, size_t, int);
using SocketFn = int (*)(int, int, int);
using RecvFn = ssize_t (*)(int, void*, size_t, int);
using PollFn = int (*)(struct pollfd*, nfds_t, int);
using ReadFn = ssize_t (*)(int, void*, size_t);
using WriteFn = ssize_t (*)(int, const void*, size_t);

MmapFn g_real_mmap = nullptr;
MprotectFn g_real_mprotect = nullptr;
SocketFn g_real_socket = nullptr;
RecvFn g_real_recv = nullptr;
PollFn g_real_poll = nullptr;
ReadFn g_real_read = nullptr;
WriteFn g_real_write = nullptr;
pthread_once_t g_syscall_once = PTHREAD_ONCE_INIT;

std::atomic<int> g_io_fault_fd{-1};
std::atomic<int> g_poll_timeout_count{0};
std::atomic<int> g_poll_eintr_count{0};
std::atomic<int> g_poll_fatal_count{0};
std::atomic<int> g_read_eintr_count{0};
std::atomic<int> g_write_eagain_count{0};
std::atomic<int> g_write_eintr_count{0};
std::atomic<int> g_write_fatal_count{0};

void resolve_syscalls() {
    g_real_mmap = reinterpret_cast<MmapFn>(dlsym(RTLD_NEXT, "mmap"));
    g_real_mprotect = reinterpret_cast<MprotectFn>(dlsym(RTLD_NEXT, "mprotect"));
    g_real_socket = reinterpret_cast<SocketFn>(dlsym(RTLD_NEXT, "socket"));
    g_real_recv = reinterpret_cast<RecvFn>(dlsym(RTLD_NEXT, "recv"));
    g_real_poll = reinterpret_cast<PollFn>(dlsym(RTLD_NEXT, "poll"));
    g_real_read = reinterpret_cast<ReadFn>(dlsym(RTLD_NEXT, "read"));
    g_real_write = reinterpret_cast<WriteFn>(dlsym(RTLD_NEXT, "write"));
}

bool consume_fault(std::atomic<int>& counter) {
    int remaining = counter.load(std::memory_order_relaxed);
    while (remaining > 0) {
        if (counter.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

IoFaultConfig current_io_fault_config() {
    IoFaultConfig config;
    config.fd = g_io_fault_fd.load(std::memory_order_relaxed);
    config.poll_timeouts = g_poll_timeout_count.load(std::memory_order_relaxed);
    config.poll_eintrs = g_poll_eintr_count.load(std::memory_order_relaxed);
    config.poll_fatals = g_poll_fatal_count.load(std::memory_order_relaxed);
    config.read_eintrs = g_read_eintr_count.load(std::memory_order_relaxed);
    config.write_eagains = g_write_eagain_count.load(std::memory_order_relaxed);
    config.write_eintrs = g_write_eintr_count.load(std::memory_order_relaxed);
    config.write_fatals = g_write_fatal_count.load(std::memory_order_relaxed);
    return config;
}

void apply_io_fault_config(const IoFaultConfig& config) {
    g_io_fault_fd.store(config.fd, std::memory_order_relaxed);
    g_poll_timeout_count.store(config.poll_timeouts, std::memory_order_relaxed);
    g_poll_eintr_count.store(config.poll_eintrs, std::memory_order_relaxed);
    g_poll_fatal_count.store(config.poll_fatals, std::memory_order_relaxed);
    g_read_eintr_count.store(config.read_eintrs, std::memory_order_relaxed);
    g_write_eagain_count.store(config.write_eagains, std::memory_order_relaxed);
    g_write_eintr_count.store(config.write_eintrs, std::memory_order_relaxed);
    g_write_fatal_count.store(config.write_fatals, std::memory_order_relaxed);
}

}  // namespace

FaultState& state() {
    return g_state;
}

void reset() {
    g_state = FaultState{};
}

ScopedFaultState::ScopedFaultState() : previous_(g_state) {}

ScopedFaultState::~ScopedFaultState() {
    g_state = previous_;
}

ScopedMemoryFault::ScopedMemoryFault(int mmap_fail_call, bool mprotect_fail) {
    g_state.mmap_fail_call = mmap_fail_call;
    g_state.mmap_call_count = 0;
    g_state.mprotect_fail = mprotect_fail;
}

ScopedFakeSocket::ScopedFakeSocket(int fd) {
    g_state.fake_socket_fd = fd;
}

ScopedRecvData::ScopedRecvData(int fd, const char* data, size_t len, int eintrs) {
    g_state.recv_fd = fd;
    g_state.recv_eintrs = eintrs;
    g_state.recv_len = len < sizeof(g_state.recv_data) ? len : sizeof(g_state.recv_data);
    for (size_t i = 0; i < g_state.recv_len; i++) {
        g_state.recv_data[i] = static_cast<u8>(data[i]);
    }
}

ScopedIoFault::ScopedIoFault(const IoFaultConfig& config) : previous_(current_io_fault_config()) {
    apply_io_fault_config(config);
}

ScopedIoFault::~ScopedIoFault() {
    apply_io_fault_config(previous_);
}

int ScopedIoFault::remaining_read_eintrs() const {
    return g_read_eintr_count.load(std::memory_order_relaxed);
}

int ScopedIoFault::remaining_write_eintrs() const {
    return g_write_eintr_count.load(std::memory_order_relaxed);
}

}  // namespace rut::test_fault

extern "C" void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    auto& state = rut::test_fault::state();
    if (state.mmap_fail_call > 0 && ++state.mmap_call_count == state.mmap_fail_call) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (!rut::test_fault::g_real_mmap) {
        errno = ENOSYS;
        return MAP_FAILED;
    }
    return rut::test_fault::g_real_mmap(addr, len, prot, flags, fd, offset);
}

extern "C" int mprotect(void* addr, size_t len, int prot) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    auto& state = rut::test_fault::state();
    if (state.mprotect_fail) {
        errno = ENOMEM;
        return -1;
    }
    if (!rut::test_fault::g_real_mprotect) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_mprotect(addr, len, prot);
}

extern "C" int socket(int domain, int type, int protocol) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    auto& state = rut::test_fault::state();
    if (state.fake_socket_fd >= 0 && domain == AF_INET && (type & SOCK_STREAM) == SOCK_STREAM) {
        int fd = state.fake_socket_fd;
        state.fake_socket_fd = -1;
        return fd;
    }
    if (!rut::test_fault::g_real_socket) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_socket(domain, type, protocol);
}

extern "C" ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    auto& state = rut::test_fault::state();
    if (state.recv_fd >= 0 && sockfd == state.recv_fd) {
        if (state.recv_eintrs > 0) {
            state.recv_eintrs--;
            errno = EINTR;
            return -1;
        }
        const size_t n = len < state.recv_len ? len : state.recv_len;
        for (size_t i = 0; i < n; i++) {
            static_cast<rut::u8*>(buf)[i] = state.recv_data[i];
        }
        state.recv_fd = -1;
        state.recv_len = 0;
        return static_cast<ssize_t>(n);
    }
    if (!rut::test_fault::g_real_recv) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_recv(sockfd, buf, len, flags);
}

extern "C" int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    if (fds != nullptr && nfds == 1 && (fds[0].events & POLLOUT) != 0 &&
        fds[0].fd == rut::test_fault::g_io_fault_fd.load(std::memory_order_relaxed)) {
        if (rut::test_fault::consume_fault(rut::test_fault::g_poll_timeout_count)) return 0;
        if (rut::test_fault::consume_fault(rut::test_fault::g_poll_eintr_count)) {
            errno = EINTR;
            return -1;
        }
        if (rut::test_fault::consume_fault(rut::test_fault::g_poll_fatal_count)) {
            errno = EINVAL;
            return -1;
        }
    }
    if (!rut::test_fault::g_real_poll) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_poll(fds, nfds, timeout);
}

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    if (fd == rut::test_fault::g_io_fault_fd.load(std::memory_order_relaxed) &&
        rut::test_fault::consume_fault(rut::test_fault::g_read_eintr_count)) {
        errno = EINTR;
        return -1;
    }
    if (!rut::test_fault::g_real_read) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_read(fd, buf, count);
}

extern "C" ssize_t write(int fd, const void* buf, size_t count) {
    pthread_once(&rut::test_fault::g_syscall_once, rut::test_fault::resolve_syscalls);
    if (fd == rut::test_fault::g_io_fault_fd.load(std::memory_order_relaxed)) {
        if (rut::test_fault::consume_fault(rut::test_fault::g_write_eagain_count)) {
            errno = EAGAIN;
            return -1;
        }
        if (rut::test_fault::consume_fault(rut::test_fault::g_write_eintr_count)) {
            errno = EINTR;
            return -1;
        }
        if (rut::test_fault::consume_fault(rut::test_fault::g_write_fatal_count)) {
            errno = EPIPE;
            return -1;
        }
    }
    if (!rut::test_fault::g_real_write) {
        errno = ENOSYS;
        return -1;
    }
    return rut::test_fault::g_real_write(fd, buf, count);
}
