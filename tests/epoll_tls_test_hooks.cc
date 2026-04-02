#include "epoll_tls_test_hooks.h"

#include <atomic>

namespace rut {

namespace {

std::atomic<const EpollTlsHooks*> g_test_tls_hooks = nullptr;

}  // namespace

const EpollTlsHooks* get_epoll_tls_hooks_for_test() {
    return g_test_tls_hooks.load(std::memory_order_acquire);
}

void set_epoll_tls_hooks_for_test(const EpollTlsHooks* hooks) {
    g_test_tls_hooks.store(hooks, std::memory_order_release);
}

void reset_epoll_tls_hooks_for_test() {
    g_test_tls_hooks.store(nullptr, std::memory_order_release);
}

}  // namespace rut
