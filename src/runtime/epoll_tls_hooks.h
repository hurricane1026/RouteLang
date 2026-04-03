#pragma once

#include "rut/common/types.h"

#include <openssl/base.h>

namespace rut {

// Runtime-private seam for forcing specific TLS state-machine transitions in
// epoll tests. Production code always uses the default OpenSSL/BoringSSL hooks.
struct EpollTlsHooks {
    i32 (*ssl_accept)(SSL* ssl);
    i32 (*ssl_read)(SSL* ssl, void* buf, i32 len);
    i32 (*ssl_write)(SSL* ssl, const void* buf, i32 len);
    i32 (*ssl_get_error)(SSL* ssl, i32 rc);
};

}  // namespace rut
