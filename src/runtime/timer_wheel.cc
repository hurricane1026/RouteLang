#include "rut/runtime/timer_wheel.h"

#include "rut/runtime/connection.h"

#include <stddef.h>  // offsetof

namespace rut {

void TimerWheel::add(Connection* c, u32 seconds) {
    u32 slot = (cursor + seconds) & (kSlots - 1);
    slots[slot].insert_after(&c->timer_node);
}

void TimerWheel::refresh(Connection* c, u32 seconds) {
    c->timer_node.remove();
    c->timer_node.init();
    add(c, seconds);
}

void TimerWheel::remove(Connection* c) {
    c->timer_node.remove();
    c->timer_node.init();
}

u64 TimerWheel::timer_node_offset() {
    return offsetof(Connection, timer_node);
}

}  // namespace rut
