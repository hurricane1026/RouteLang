#pragma once

#include "rut/common/types.h"

#include <stddef.h>

namespace rut::test_fault {

struct FaultState {
    int mmap_fail_call = 0;
    int mmap_call_count = 0;
    bool mprotect_fail = false;

    int fake_socket_fd = -1;

    int recv_fd = -1;
    int recv_eintrs = 0;
    size_t recv_len = 0;
    u8 recv_data[512]{};
};

struct IoFaultConfig {
    int fd = -1;
    int poll_timeouts = 0;
    int poll_eintrs = 0;
    int poll_fatals = 0;
    int read_eintrs = 0;
    int read_fatals = 0;
    size_t read_short_len = 0;
    int read_shorts = 0;
    int write_eagains = 0;
    int write_eintrs = 0;
    int write_fatals = 0;
    size_t write_short_len = 0;
    int write_shorts = 0;
    int send_eagains = 0;
    int send_eintrs = 0;
    int send_fatals = 0;
    size_t send_short_len = 0;
    int send_shorts = 0;
    int connect_errno = 0;
    int connect_failures = 0;
    int close_errno = 0;
    int close_failures = 0;
    int fcntl_errno = 0;
    int fcntl_failures = 0;
};

struct SyscallFaultConfig {
    int epoll_create1_errno = 0;
    int epoll_create1_failures = 0;
    int epoll_ctl_errno = 0;
    int epoll_ctl_failures = 0;
    int epoll_wait_eintrs = 0;
    int epoll_wait_errno = 0;
    int epoll_wait_failures = 0;
    int timerfd_create_errno = 0;
    int timerfd_create_failures = 0;
    int timerfd_settime_errno = 0;
    int timerfd_settime_failures = 0;
    int accept4_errno = 0;
    int accept4_failures = 0;
    int open_errno = 0;
    int open_failures = 0;
    int mkstemp_errno = 0;
    int mkstemp_failures = 0;
    int unlink_errno = 0;
    int unlink_failures = 0;
};

FaultState& state();
void reset();

class ScopedFaultState {
public:
    ScopedFaultState(const ScopedFaultState&) = delete;
    ScopedFaultState& operator=(const ScopedFaultState&) = delete;
    ~ScopedFaultState();

protected:
    ScopedFaultState();

private:
    FaultState previous_;
};

class ScopedMemoryFault : private ScopedFaultState {
public:
    explicit ScopedMemoryFault(int mmap_fail_call = 0, bool mprotect_fail = false);
};

class ScopedFakeSocket : private ScopedFaultState {
public:
    explicit ScopedFakeSocket(int fd);
};

class ScopedRecvData : private ScopedFaultState {
public:
    ScopedRecvData(int fd, const char* data, size_t len, int eintrs = 0);
};

class ScopedIoFault {
public:
    explicit ScopedIoFault(const IoFaultConfig& config);
    ScopedIoFault(const ScopedIoFault&) = delete;
    ScopedIoFault& operator=(const ScopedIoFault&) = delete;
    ~ScopedIoFault();

    int remaining_read_eintrs() const;
    int remaining_write_eintrs() const;
    int remaining_send_eagains() const;
    int remaining_connect_failures() const;

private:
    IoFaultConfig previous_;
};

class ScopedSyscallFault {
public:
    explicit ScopedSyscallFault(const SyscallFaultConfig& config);
    ScopedSyscallFault(const ScopedSyscallFault&) = delete;
    ScopedSyscallFault& operator=(const ScopedSyscallFault&) = delete;
    ~ScopedSyscallFault();

private:
    SyscallFaultConfig previous_;
};

}  // namespace rut::test_fault
