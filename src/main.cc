#include "rout/runtime/epoll_backend.h"
#include "rout/runtime/event_loop.h"
#include "rout/runtime/io_uring_backend.h"
#include "rout/runtime/socket.h"

#include <linux/io_uring.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace rout;

static void write_str(const char* s) {
    u32 len = 0;
    while (s[len]) len++;
    (void)write(1, s, len);
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

    // Get actual port (kernel may assign ephemeral if port==0)
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

    // EventLoop contains large arrays (Connection[16384] × ~8KB each ≈ 130MB).
    // Must be mmap'd, not stack-allocated.
    if (detect_io_uring()) {
        write_str("Backend: io_uring\n");
        auto* loop = static_cast<EventLoop<IoUringBackend>*>(mmap(nullptr,
                                                                  sizeof(EventLoop<IoUringBackend>),
                                                                  PROT_READ | PROT_WRITE,
                                                                  MAP_PRIVATE | MAP_ANONYMOUS,
                                                                  -1,
                                                                  0));
        if (loop == MAP_FAILED) {
            write_str("Failed to mmap io_uring loop\n");
            close(listen_fd);
            return 1;
        }
        if (loop->init(0, listen_fd) < 0) {
            write_str("Failed to init io_uring\n");
            loop->shutdown();
            munmap(loop, sizeof(EventLoop<IoUringBackend>));
            close(listen_fd);
            return 1;
        }
        loop->run();
        loop->shutdown();
        munmap(loop, sizeof(EventLoop<IoUringBackend>));
    } else {
        write_str("Backend: epoll\n");
        auto* loop = static_cast<EventLoop<EpollBackend>*>(mmap(nullptr,
                                                                sizeof(EventLoop<EpollBackend>),
                                                                PROT_READ | PROT_WRITE,
                                                                MAP_PRIVATE | MAP_ANONYMOUS,
                                                                -1,
                                                                0));
        if (loop == MAP_FAILED) {
            write_str("Failed to mmap epoll loop\n");
            close(listen_fd);
            return 1;
        }
        if (loop->init(0, listen_fd) < 0) {
            write_str("Failed to init epoll\n");
            loop->shutdown();
            munmap(loop, sizeof(EventLoop<EpollBackend>));
            close(listen_fd);
            return 1;
        }
        loop->run();
        loop->shutdown();
        munmap(loop, sizeof(EventLoop<EpollBackend>));
    }

    close(listen_fd);
    return 0;
}
