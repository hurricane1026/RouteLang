#pragma once

#include "core/expected.h"
#include "rut/common/types.h"
#include "rut/runtime/error.h"

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace rut {

struct TlsServerContext {
    SSL_CTX* ssl_ctx;
};

core::Expected<TlsServerContext*, Error> create_tls_server_context(const char* cert_path,
                                                                   const char* key_path);
void destroy_tls_server_context(TlsServerContext* ctx);
core::Expected<SSL*, Error> create_tls_server_ssl(TlsServerContext* ctx, i32 fd);
void destroy_tls_server_ssl(SSL* ssl);

}  // namespace rut
