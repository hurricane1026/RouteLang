#pragma once

#include "epoll_tls_hooks.h"

namespace rut {

void set_epoll_tls_hooks_for_test(const EpollTlsHooks* hooks);
void reset_epoll_tls_hooks_for_test();

}  // namespace rut
