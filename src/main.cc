#include "rout/runtime/epoll_backend.h"
#include "rout/runtime/event_loop.h"
#include "rout/runtime/io_uring_backend.h"
#include "rout/runtime/socket.h"

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace rout;

static void write_str(const char* s) {
    u32 len = 0;
    while (s[len]) len++;
    (void)write(1, s, len);
}

static bool detect_io_uring() {
    // Test with the same flags IoUringBackend::init() uses.
    // Kernels may support basic io_uring but not these specific features.
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;
    i32 fd = static_cast<i32>(syscall(__NR_io_uring_setup, 1, &params));
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    u16 port = 8080;
    if (argc > 1) {
        port = 0;
        for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++)
            port = port * 10 + static_cast<u16>(*p - '0');
    }

    i32 listen_fd = create_listen_socket(port);
    if (listen_fd < 0) {
        write_str("Failed to create listen socket\n");
        return 1;
    }

    // Get actual port (kernel may have assigned an ephemeral port if port==0)
    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr), &addr_len);
    port = __builtin_bswap16(bound_addr.sin_port);

    write_str("Listening on port ");
    char buf[8];
    i32 n = 0;
    u16 tmp = port;
    do {
        buf[n++] = static_cast<char>('0' + tmp % 10);
        tmp /= 10;
    } while (tmp);
    for (i32 i = n - 1; i >= 0; i--) {
        (void)write(1, &buf[i], 1);
    }
    write_str("\n");

    bool use_epoll = true;
    if (detect_io_uring()) {
        write_str("Backend: io_uring\n");
        EventLoop<IoUringBackend> loop;
        if (loop.init(0, listen_fd) < 0) {
            // io_uring detection passed but init failed (e.g., PBUF_RING
            // not supported). Fall back to epoll.
            write_str("io_uring init failed, falling back to epoll\n");
        } else {
            use_epoll = false;
            loop.run();
            loop.shutdown();
        }
    }
    if (use_epoll) {
        write_str("Backend: epoll\n");
        EventLoop<EpollBackend> loop;
        if (loop.init(0, listen_fd) < 0) {
            write_str("Failed to init epoll\n");
            close(listen_fd);
            return 1;
        }
        loop.run();
        loop.shutdown();
    }

    close(listen_fd);

    return 0;
}
