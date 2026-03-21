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

// Store pointers to each shard's loop->running flag for signal handler.
// Writing false to a bool is async-signal-safe.
static bool* g_running_flags[kMaxShards];
static u32 g_shard_count = 0;

static void signal_handler(int /*sig*/) {
    for (u32 i = 0; i < g_shard_count; i++) {
        if (g_running_flags[i]) *g_running_flags[i] = false;
    }
}

template <typename Backend>
static i32 run_shards(u16 port, u32 shard_count, bool pin_cpus) {
    Shard<Backend> shards[kMaxShards];

    // Create one SO_REUSEPORT listen socket per shard.
    // Kernel distributes incoming connections across sockets.
    for (u32 i = 0; i < shard_count; i++) {
        i32 lfd = create_listen_socket(port);
        if (lfd < 0) {
            write_str("Failed to create listen socket for shard ");
            write_u32(i);
            write_str("\n");
            // Cleanup already-initialized shards
            for (u32 j = 0; j < i; j++) {
                shards[j].stop();
                shards[j].join();
                shards[j].shutdown();
            }
            return 1;
        }
        shards[i].owns_listen_fd = true;

        i32 rc = shards[i].init(i, lfd);
        if (rc < 0) {
            write_str("Failed to init shard ");
            write_u32(i);
            write_str("\n");
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
    getsockname(shards[0].listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len);
    port = __builtin_bswap16(bound_addr.sin_port);

    write_str("Listening on port ");
    write_u32(port);
    write_str(" with ");
    write_u32(shard_count);
    write_str(" shard(s)\n");

    // Register running flags for signal handler (before spawning threads)
    for (u32 i = 0; i < shard_count; i++) {
        g_running_flags[i] = &shards[i].loop->running;
    }
    g_shard_count = shard_count;

    // Install signal handlers for graceful shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Spawn shard threads
    for (u32 i = 0; i < shard_count; i++) {
        i32 pin = pin_cpus ? static_cast<i32>(i) : -1;
        i32 rc = shards[i].spawn(pin);
        if (rc < 0) {
            write_str("Failed to spawn shard ");
            write_u32(i);
            write_str("\n");
            // Stop all already-spawned shards
            for (u32 j = 0; j < i; j++) shards[j].stop();
            for (u32 j = 0; j < i; j++) shards[j].join();
            for (u32 j = 0; j < shard_count; j++) shards[j].shutdown();
            return 1;
        }
    }

    // Wait for all shard threads to finish (blocked by signal or stop)
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
            bool is_shards = true;
            const char* expect = "--shards";
            for (int k = 0; expect[k]; k++) {
                if (argv[i][k] != expect[k]) {
                    is_shards = false;
                    break;
                }
            }
            if (is_shards) {
                i++;
                shard_count = 0;
                for (const char* p = argv[i]; *p >= '0' && *p <= '9'; p++)
                    shard_count = shard_count * 10 + static_cast<u32>(*p - '0');
            }
        }
        // Check --no-pin
        bool is_nopin = true;
        const char* expect_np = "--no-pin";
        for (int k = 0; expect_np[k]; k++) {
            if (argv[i][k] != expect_np[k]) {
                is_nopin = false;
                break;
            }
        }
        if (is_nopin) pin_cpus = false;
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
