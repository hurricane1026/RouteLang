#include "rout/runtime/epoll_backend.h"
#include "rout/runtime/io_uring_backend.h"
#include "rout/runtime/shard.h"
#include "rout/runtime/socket.h"

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace rout;

static constexpr u32 kMaxShards = 64;

static void write_str(const char* s) {
    u32 len = 0;
    while (s[len]) len++;
    (void)write(1, s, len);
}

static void write_u32(u32 val) {
    char buf[12];
    i32 n = 0;
    u32 tmp = val;
    do {
        buf[n++] = static_cast<char>('0' + tmp % 10);
        tmp /= 10;
    } while (tmp);
    for (i32 i = n - 1; i >= 0; i--) (void)write(1, &buf[i], 1);
}

static bool str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
}

static bool detect_io_uring() {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    i32 fd = static_cast<i32>(syscall(__NR_io_uring_setup, 1, &params));
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

// --- Signal handling for graceful shutdown ---

static void write_error(const char* prefix, const rout::Error& err) {
    write_str(prefix);
    write_str(" (errno=");
    write_u32(static_cast<u32>(err.code));
    write_str(", source=");
    write_u32(static_cast<u32>(err.source));
    write_str(")\n");
}

template <typename Backend>
static i32 run_shards(u16 port, u32 shard_count, bool pin_cpus) {
    Shard<Backend> shards[kMaxShards];

    // Create one SO_REUSEPORT listen socket per shard.
    // If port==0 (ephemeral), create shard 0 first to get the assigned port,
    // then create remaining sockets on that concrete port.
    for (u32 i = 0; i < shard_count; i++) {
        auto lfd_result = create_listen_socket(port);
        // After shard 0, resolve ephemeral port so remaining shards bind the same port.
        if (i == 0 && port == 0 && lfd_result) {
            struct sockaddr_in a;
            socklen_t al = sizeof(a);
            if (getsockname(lfd_result.value(), reinterpret_cast<struct sockaddr*>(&a), &al) < 0) {
                write_str("Failed to resolve ephemeral port\n");
                close(lfd_result.value());
                return 1;
            }
            port = __builtin_bswap16(a.sin_port);
        }
        if (!lfd_result) {
            write_str("Failed to create listen socket for shard ");
            write_u32(i);
            write_error("", lfd_result.error());
            // Cleanup already-initialized shards
            for (u32 j = 0; j < i; j++) {
                shards[j].stop();
                shards[j].join();
                shards[j].shutdown();
            }
            return 1;
        }
        i32 lfd = lfd_result.value();
        shards[i].owns_listen_fd = true;

        auto rc = shards[i].init(i, lfd);
        if (!rc) {
            write_str("Failed to init shard ");
            write_u32(i);
            write_error("", rc.error());
            close(lfd);
            for (u32 j = 0; j < i; j++) {
                shards[j].stop();
                shards[j].join();
                shards[j].shutdown();
            }
            return 1;
        }
    }

    // Get actual port from first shard's socket
    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(
            shards[0].listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len) < 0) {
        write_str("Failed to get bound address\n");
        for (u32 j = 0; j < shard_count; j++) shards[j].shutdown();
        return 1;
    }
    port = __builtin_bswap16(bound_addr.sin_port);

    write_str("Listening on port ");
    write_u32(port);
    write_str(" with ");
    write_u32(shard_count);
    write_str(" shard(s)\n");

    // Block SIGINT/SIGTERM so sigwait() can catch them race-free.
    // Must block before spawning threads (threads inherit the mask).
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGINT);
    sigaddset(&wait_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &wait_set, nullptr);

    // Spawn shard threads
    for (u32 i = 0; i < shard_count; i++) {
        i32 pin = pin_cpus ? static_cast<i32>(i) : -1;
        auto rc = shards[i].spawn(pin);
        if (!rc) {
            write_str("Failed to spawn shard ");
            write_u32(i);
            write_error("", rc.error());
            // Stop all already-spawned shards
            for (u32 j = 0; j < i; j++) shards[j].stop();
            for (u32 j = 0; j < i; j++) shards[j].join();
            for (u32 j = 0; j < shard_count; j++) shards[j].shutdown();
            return 1;
        }
    }

    // Wait for SIGINT/SIGTERM — sigwait() is race-free (signal is blocked).
    i32 sig = 0;
    sigwait(&wait_set, &sig);

    // Stop all shards from main thread (not signal context)
    for (u32 i = 0; i < shard_count; i++) shards[i].stop();
    for (u32 i = 0; i < shard_count; i++) shards[i].join();
    for (u32 i = 0; i < shard_count; i++) shards[i].shutdown();

    return 0;
}

int main(int argc, char** argv) {
    u16 port = 8080;
    u32 shard_count = 0;  // 0 = auto-detect
    bool pin_cpus = true;

    // Simple arg parsing: [port] [--shards N] [--no-pin]
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            port = 0;
            for (const char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                port = port * 10 + static_cast<u16>(*p - '0');
        }
        // Check --shards
        if (i + 1 < argc) {
            if (str_eq(argv[i], "--shards")) {
                i++;
                shard_count = 0;
                for (const char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                    shard_count = shard_count * 10 + static_cast<u32>(*p - '0');
            }
        }
        // Check --no-pin
        if (str_eq(argv[i], "--no-pin")) pin_cpus = false;
    }

    if (shard_count == 0) shard_count = detect_cpu_count();
    if (shard_count > kMaxShards) shard_count = kMaxShards;

    if (detect_io_uring()) {
        write_str("Backend: io_uring\n");
        return run_shards<IoUringBackend>(port, shard_count, pin_cpus);
    }
    write_str("Backend: epoll\n");
    return run_shards<EpollBackend>(port, shard_count, pin_cpus);
}
