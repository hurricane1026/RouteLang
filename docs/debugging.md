# Debugging Runtime Connections

`rut/runtime/debug.h` provides lightweight helpers for inspecting a
`Connection` without changing dispatch behavior.

Use `make_conn_debug_snapshot(conn)` when a test or diagnostic path needs a
stable value object. Use `format_conn_debug(conn, buf, len)` when a compact text
line is more useful for logs or assertion failures.

The formatted line includes:

- connection id, client fd, and upstream fd
- `ConnState` as a readable name
- active callback slot mask (`recv`, `send`, `up_recv`, `up_send`)
- armed backend operation mask (`recv`, `send`, `up_recv`, `up_send`, `yield`)
- pending operation count
- response status
- yielded JIT handler state, when present
- client recv, client send, and upstream recv buffer lengths

Example:

```text
conn{id=7 state=Sending fd=42 upstream_fd=-1 slots=send armed=send|yield pending_ops=2 resp=204 handler=pending:3 recv_buf=10 send_buf=20 upstream_recv_buf=30}
```

These helpers are intentionally header-only and allocation-free. They are safe
to call from tests and debug logging paths, but they do not enforce invariants
by themselves. Pair them with the state invariant tests when adding new
callback-slot transitions.
