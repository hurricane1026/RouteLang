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
    int write_eagains = 0;
    int write_eintrs = 0;
    int write_fatals = 0;
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

private:
    IoFaultConfig previous_;
};

}  // namespace rut::test_fault
