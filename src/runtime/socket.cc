#include "rout/runtime/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rout {

i32 set_nonblocking(i32 fd) {
    i32 flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -errno;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;
    return 0;
}

i32 create_listen_socket(u16 port) {
    i32 fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;

    i32 one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = __builtin_bswap16(port);  // htons without stdlib
    addr.sin_addr.s_addr = 0;                 // INADDR_ANY

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        i32 err = errno;
        close(fd);
        return -err;
    }

    if (listen(fd, 4096) < 0) {
        i32 err = errno;
        close(fd);
        return -err;
    }

    return fd;
}

}  // namespace rout
