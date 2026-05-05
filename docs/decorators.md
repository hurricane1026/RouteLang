# Route Decorators

This document describes the decorator behavior implemented today. It is a
route middleware subset, not a general language-level decorator system.

## Purpose

A route decorator runs before the route handler. It can allow the request to
continue or reject it with an HTTP status code.

Decorator functions use this convention:

```rut
func auth(_ req: i32) -> i32 => 0
```

Rules:

- The function must have at least one parameter.
- The first parameter must use the omitted-label form: `_ req: ...`.
- The return type must be `i32`.
- Returning `0` means pass through.
- Returning any non-zero value short-circuits the route and returns that value
  as the HTTP status code.

The first parameter is currently a placeholder. A real runtime `Request` type is
not implemented yet. Decorators that need request data should use the existing
request expression surface such as `req.header(...)`, `req.path`, and
`req.method` inside the function body.

## Binding To Routes

Decorators can be bound inside a `route { ... }` block.

### Wildcard Binding

`"*"` applies the decorator to every entry in the block:

```rut
func auth(_ req: i32) -> i32 => 0

route {
    @auth "*"
    GET "/users" { return 200 }
    POST "/orders" { return 201 }
}
```

### Prefix Binding

A string pattern other than `"*"` applies to route paths with that prefix:

```rut
func adminOnly(_ req: i32) -> i32 => 0

route {
    @adminOnly "/admin"
    GET "/admin/users" { return 200 }
    GET "/public/health" { return 200 }
}
```

In this example, `adminOnly` applies to `/admin/users` but not to
`/public/health`.

### Entry Decorator

A decorator can be attached directly to a single route entry:

```rut
func requestId(_ req: i32) -> i32 => 0

route {
    @requestId
    GET "/users" { return 200 }

    GET "/health" { return 200 }
}
```

Here, `requestId` applies only to `GET "/users"`.

### Merged Decorators

Block bindings and entry decorators are merged. Matching block bindings come
first in declaration order, then entry decorators:

```rut
func requestId(_ req: i32) -> i32 => 0
func auth(_ req: i32) -> i32 => 0
func maxBody(_ req: i32) -> i32 => 0

route {
    @requestId "*"
    @auth "/admin"

    @maxBody
    POST "/admin/upload" { return 200 }
}
```

Execution order for `/admin/upload` is:

1. `requestId`
2. `auth`
3. `maxBody`
4. route handler

The first decorator that returns a non-zero status stops the chain.

## `wait(...)` Routes

Decorators can be used with direct terminal `wait(...)` routes:

```rut
func auth(_ req: i32) -> i32 => 0

route {
    @auth "*"
    GET "/sleep" { wait(1000) return 200 }
}
```

The decorator runs before the timer yield is armed:

- If `auth` returns `401`, the handler immediately returns `401`.
- If `auth` returns `0`, the handler yields the timer and resumes to `return 200`.

Current limitation: decorated `wait(...)` routes must use direct terminal
control. These forms are rejected today:

```rut
route {
    @auth "*"
    GET "/x" { wait(50) if true { return 200 } else { return 500 } }
}
```

Decorated wait routes with user top-level guards or for-loops are also rejected.
Those shapes need a fuller source-ordered state-machine lowering.

## Unsupported Today

The following are design goals or future work, not current behavior:

- General-purpose decorators on arbitrary declarations.
- Response middleware.
- Decorator arguments such as `@auth(role: "admin")`.
- Conditional decorators such as `@if(...)`.
- A real `Request` parameter type for decorator signatures.
- Post-handler response mutation.

## Diagnostics

The analyzer rejects:

- Unknown decorator names.
- Decorator functions with zero parameters.
- Decorator functions whose first parameter is missing `_`.
- Decorator functions whose return type is not `i32`.
- Decorated wait routes outside the direct terminal subset described above.
