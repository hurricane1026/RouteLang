#include "rout/runtime/epoll_backend.h"
#include "rout/runtime/event_loop.h"
#include "rout/runtime/io_uring_backend.h"
#include "rout/runtime/socket.h"

#include <linux/io_uring.h>
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

    if (detect_io_uring()) {
        write_str("Backend: io_uring\n");
        EventLoop<IoUringBackend> loop;
        if (loop.init(0, listen_fd) < 0) {
            write_str("Failed to init io_uring\n");
            return 1;
        }
        loop.run();
    } else {
        write_str("Backend: epoll\n");
        EventLoop<EpollBackend> loop;
        if (loop.init(0, listen_fd) < 0) {
            write_str("Failed to init epoll\n");
            return 1;
        }
        loop.run();
    }

    return 0;
}
