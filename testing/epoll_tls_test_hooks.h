#pragma once

#include "rut/common/types.h"

#include <openssl/base.h>

namespace rut {

// Test-only seam for forcing specific TLS state-machine transitions in epoll.
struct EpollTlsHooks {
    i32 (*ssl_accept)(SSL* ssl);
    i32 (*ssl_read)(SSL* ssl, void* buf, i32 len);
    i32 (*ssl_write)(SSL* ssl, const void* buf, i32 len);
    i32 (*ssl_get_error)(SSL* ssl, i32 rc);
};

void set_epoll_tls_hooks_for_test(const EpollTlsHooks* hooks);
void reset_epoll_tls_hooks_for_test();

}  // namespace rut
