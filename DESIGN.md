# Dispatcher: Design Document

> A strongly-typed DSL and high-performance HTTP gateway runtime replacing nginx + OpenResty.

## 1. Project Overview

### 1.1 Goals

- Replace nginx configuration files and OpenResty Lua with a single strongly-typed language
- Catch most errors at compile time (type errors, route conflicts, invalid values)
- Easy for LLMs to generate and understand
- High performance: C100K+ capable, minimal latency overhead
- Hot reload without downtime
- Minimal dependencies, maximal control

### 1.2 Non-Goals

- General-purpose programming language
- L4 load balancer (this is L7 HTTP only)
- DPDK / kernel bypass networking (not worth the trade-off in cloud environments)
- Compatibility with nginx configuration format

---

## 2. Architecture Overview

```
                        ┌─────────────────────────────┐
  .dispatch source ───▶ │  Compiler (embedded in proc) │
                        │  parse → type check → IR     │
                        └──────────┬──────────────────┘
                                   │
                                   ▼
                        ┌──────────────────────┐
                        │  JIT (LLVM ORC JIT)   │
                        └──────────┬───────────┘
                                   │ native function pointers
                                   ▼
  ┌────────────────────────────────────────────────────────┐
  │                    Dispatcher Runtime                   │
  │  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
  │  │  Shard 0  │ │  Shard 1  │ │  Shard 2  │ │  Shard N  │  │
  │  │ io_uring  │ │ io_uring  │ │ io_uring  │ │ io_uring  │  │
  │  │ Router    │ │ Router    │ │ Router    │ │ Router    │  │
  │  │ Handlers  │ │ Handlers  │ │ Handlers  │ │ Handlers  │  │
  │  │ ConnPool  │ │ ConnPool  │ │ ConnPool  │ │ ConnPool  │  │
  │  │ Memory    │ │ Memory    │ │ Memory    │ │ Memory    │  │
  │  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
  │              share-nothing, per-core                    │
  └────────────────────────────────────────────────────────┘
```

### 2.1 Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Runtime | Custom C++ (not Seastar) | Seastar too complex for this use case; only ~30% of Seastar code is relevant; custom runtime is ~5000 lines fully controlled |
| Networking | io_uring (preferred) + epoll (fallback) | io_uring is optimal for Linux 6.0+; epoll fallback for older kernels (3.9+); both use kernel TCP stack |
| Thread model | per-core shard, share-nothing | Proven by Seastar/Envoy/nginx; eliminates locks and race conditions |
| JIT | LLVM ORC JIT | Same C++ toolchain, zero FFI overhead, lazy compilation support |
| HTTP parser | Custom (~500 lines) | llhttp is callback-based (bad fit), picohttpparser lacks strict mode; custom parser integrates directly with our Slice buffer + Arena, supports SIMD, full control over strictness (anti-smuggling) |
| C++ stdlib | `-nostdlib++`, musl libc only | Full memory control, faster compilation |
| Compiler flags | `-fno-exceptions -fno-rtti` | No overhead from unused C++ features |

---

## 3. The Dispatch Language

### 3.1 Design Principles

- **Swift-inspired syntax**: guard statements, named parameters, trailing closures, optional chaining, string interpolation — clean and readable, high LLM generation accuracy
- **HTTP concepts are native objects**: methods, status codes, headers, URLs, CIDR, media types are first-class language constructs, not strings
- **All functions inline at compile time**: no runtime function calls, each route compiles to a single flat state machine
- **Async is invisible**: no async/await/future/promise — user writes sequential code, compiler finds I/O points and generates state machines automatically
- **Strong typing with domain types**: Duration, ByteSize, StatusCode, IP, CIDR, MediaType with compile-time validation
- **Middleware = ordinary functions**: return a status code to reject, return nothing to pass through
- **Minimal keyword set**: `func`, `let`, `var`, `guard`, `struct`, `route`, `use`, `match`, `if`, `else`, `for`, `in`, `return`, `upstream`, `listen`, `proxy`, `extern`, `import`

### 3.2 File Extension

`.dispatch`

### 3.3 Type System

#### 3.3.1 Built-in Domain Types

HTTP concepts are native types, not strings:

```swift
// Scalar domain types — compile-time validated
Duration      // 1s, 500ms, 1m, 5m, 1h, 1d — arithmetic supported
ByteSize      // 64b, 1kb, 16kb, 1mb, 1gb — arithmetic supported
StatusCode    // 100..599 with names: 200 OK, 404 Not-Found, etc.
Method        // GET POST PUT DELETE PATCH HEAD OPTIONS ANY — keywords
IP            // v4/v6, compile-time format validation
CIDR          // 10.0.0.0/8, 172.16.0.0/12 — native syntax
Port          // 1..65535
MediaType     // application/json, text/html — native syntax
Time          // timestamp type, now() returns this
```

#### 3.3.2 User-Defined Types

```swift
struct User {
    id: string
    role: string
    name: string
}

struct Order {
    items: [OrderItem]
    total: f64
}

struct OrderItem {
    sku: string
    qty: i32
    price: f64
}
```

#### 3.3.3 Request Object

Request is a built-in type. Standard HTTP headers are native properties with correct types:

```swift
// Standard headers — accessed as properties, typed correctly
req.method              // Method
req.path                // string
req.remoteAddr          // IP
req.contentLength       // ByteSize (not string)
req.contentType         // MediaType (not string)
req.authorization       // string? (optional — header may not exist)
req.host                // string
req.userAgent           // string
req.origin              // string?
req.ifModifiedSince     // Time?
req.accept              // MediaType?

// Custom headers — accessed as properties using their name
req.X-Request-ID        // string?
req.X-User-ID           // string?
req.X-Timestamp         // string?

// Setting headers — direct assignment
req.X-User-ID = "123"
req.X-Request-ID = uuid()

// Route parameters — path captures become req properties
// Route: get /users/:id/posts/:postId
req.id                  // string (from :id)
req.postId              // string (from :postId)

// Body
req.body(User)          // parse body as User struct (type-checked)
req.bodyRaw             // raw bytes

// Cookies
req.cookie("session_id") // string?
```

#### 3.3.4 Response — Just Status Codes

```swift
// Response is implicit — just return a status code
return 200                           // empty body
return 401                           // short form
return 401, "custom message"         // with body
return 413                           // short form

// With headers — trailing block, no commas (Swift style)
return 429 {
    Retry-After: window
    X-RateLimit-Remaining: 0
}

// With headers and body
return 200 {
    Content-Type: application/json
    Body: json(stats())
}
```

#### 3.3.5 State Types

```swift
// Counters — built-in state, per-shard, for rate limiting and concurrency control
counter.incr(key, window: window)   // increment counter for key within time window (sliding window)
counter.get(key)                     // read current count
counter.active(key)                  // active count: +1 on request start, auto -1 on request end
                                     // used for concurrency limiting (different from incr)
```

### 3.4 Syntax

#### 3.4.1 Listening and Upstream

```swift
// Simple
listen :80, redirect: :443

// With TLS
listen :443, tls(cert: env("CERT"), key: env("KEY"))

// With connection-level security (anti-DDoS / slowloris)
listen :443, tls(cert: env("CERT"), key: env("KEY")) {
    maxConns: 100000             // global max connections
    maxConnsPerIP: 256           // per-IP connection limit
    headerTimeout: 10s           // header must arrive within this
    bodyTimeout: 30s             // body must arrive within this
    idleTimeout: 60s             // keep-alive idle timeout
    maxHeaderSize: 8kb           // max total header size
    maxHeaderCount: 100          // max number of headers
    maxUrlLength: 4kb            // max URL length
    minRecvRate: 1kb/s           // close connections slower than this
    strictParsing: true          // reject ambiguous HTTP (anti-smuggling)
}

// Internal port for metrics/admin
listen :9090, internal: true

let users = upstream {
    "10.0.0.1:8080", weight: 3
    "10.0.0.2:8080"
    balance: .leastConn
    health: .get("/ping", every: 5s)
    breaker: .consecutive(failures: 5, recover: 30s)
    retry: .on([502, 503, 504], count: 2, backoff: 100ms)
}

let orders = upstream { "10.0.1.1:8080" }
```

#### 3.4.2 Functions (Middleware and Helpers)

All functions are inlined at compile time. No runtime function call overhead.

Two types of middleware, distinguished by parameter signature:
- **Request middleware**: `func f(_ req: Request)` — runs before handler. Return status code to reject, return nothing to pass through.
- **Response middleware**: `func f(_ req: Request, _ resp: Response)` — runs after handler/proxy, before sending to client. Can modify response headers, body, status.

`guard` is the idiomatic way to express "check or reject".

```swift
func auth(_ req: Request, role: string) -> User {
    guard let token = req.authorization else { return 401 }
    guard token.hasPrefix("Bearer ") else { return 401 }

    let claims = jwtDecode(token.trimPrefix("Bearer "), secret: env("JWT_SECRET"))
    guard let claims else { return 401 }
    guard claims.exp >= now() else { return 401, "token expired" }
    guard claims.role == role else { return 403 }

    req.X-User-ID = claims.sub
    req.X-User-Role = claims.role
    return User(id: claims.sub, role: claims.role)
}

func rateLimit(_ req: Request, limit: i32, window: Duration) {
    let count = counter.incr(req.remoteAddr, window: window)
    guard count <= limit else {
        return 429 {
            Retry-After: window
        }
    }
}

func requestId(_ req: Request) {
    if req.X-Request-ID.isEmpty {
        req.X-Request-ID = uuid()
    }
}

func cors(_ req: Request, origins: [string]) {
    let origin = req.origin
    guard origins.contains(origin) else {
        if req.method == .OPTIONS { return 403 }
        return
    }

    if req.method == .OPTIONS {
        return 204 {
            Access-Control-Allow-Origin: origin
            Access-Control-Allow-Methods: "GET, POST, PUT, DELETE"
            Access-Control-Allow-Headers: "Content-Type, Authorization"
            Access-Control-Max-Age: 86400
        }
    }

    req.Access-Control-Allow-Origin = origin
    req.Vary = "Origin"
}

func verifySig(_ req: Request, secret: string) {
    guard let ts = req.X-Timestamp else { return 401 }
    guard now() - time(ts) <= 5m else { return 401, "expired" }

    let payload = "\(req.method)\n\(req.path)\n\(ts)"
    guard hmacSha256(secret, payload).hex == req.X-Signature else {
        return 401, "bad signature"
    }
}

func ipAllow(_ req: Request, cidrs: [CIDR]) {
    guard cidrs.contains(where: { req.remoteAddr.in($0) }) else {
        return 403
    }
}

func maxBody(_ req: Request, limit: ByteSize) {
    guard req.contentLength <= limit else { return 413 }
}

func concurrencyLimit(_ req: Request, key: string, limit: i32) {
    guard counter.active(key) < limit else { return 503, "overloaded" }
    // counter.active: +1 on request start, auto -1 on request end
}

// --- Response middleware (takes both Request and Response) ---

func securityHeaders(_ req: Request, _ resp: Response) {
    resp.Strict-Transport-Security = "max-age=31536000; includeSubDomains"
    resp.X-Content-Type-Options = "nosniff"
    resp.X-Frame-Options = "DENY"
    resp.X-XSS-Protection = "1; mode=block"
    resp.Referrer-Policy = "strict-origin-when-cross-origin"
    resp.Server = nil          // strip server info
}

// --- Security middleware examples ---

func antiFlood(_ req: Request) {
    guard counter.incr(req.remoteAddr, window: 1s) <= 50 else { return 429 }
    guard counter.incr("global", window: 1s) <= 10000 else { return 503 }
}

func waf(_ req: Request) {
    let path = req.path
    guard !path.contains("/../") else { return 403, "path traversal" }
    guard !urlDecode(path).contains("/../") else { return 403, "encoded traversal" }

    let dangerous = ["UNION SELECT", "DROP TABLE", "<script", "javascript:"]
    let input = "\(path) \(req.queryString ?? "")".upper()
    for pattern in dangerous {
        guard !input.contains(pattern) else {
            log.warn("waf blocked", { pattern: pattern, addr: req.remoteAddr })
            return 403
        }
    }
}

func blockBots(_ req: Request) {
    let ua = req.userAgent.lower()
    guard !ua.isEmpty else { return 403 }
    let blocked = ["bot", "spider", "crawler", "scraper"]
    for word in blocked {
        guard !ua.contains(word) else { return 403 }
    }
}
```

#### 3.4.3 Route Definition

Routes use trailing closure syntax. No commas between block items (Swift style).

```swift
route {
    get /health => 200

    // Global middleware — applied to all routes below
    use requestId
    use cors(origins: ["https://app.example.com"])

    get /users/:id { req in
        let user = auth(req, role: "user")
        rateLimit(req, limit: 100, window: 1m)
        req.X-User-ID = req.id
        proxy(users, timeout: 10s)
    }

    post /users { req in
        auth(req, role: "admin")
        maxBody(req, limit: 1mb)
        proxy(users, timeout: 30s)
    }

    post /orders { req in
        let user = auth(req, role: "user")
        let order = req.body(Order)
        guard !order.items.isEmpty else {
            return 400, "order must have items"
        }
        for item in order.items {
            guard item.qty > 0 else { return 400, "invalid quantity" }
            guard item.price >= 0 else { return 400, "invalid price" }
        }
        req.X-User-ID = user.id
        proxy(orders, timeout: 30s)
    }

    post /webhook { req in
        verifySig(req, secret: env("WEBHOOK_SECRET"))
        maxBody(req, limit: 1mb)
        proxy(webhookService)
    }

    // Route group with shared prefix and middleware
    group /admin { req in
        ipAllow(req, cidrs: [10.0.0.0/8, 172.16.0.0/12])
        auth(req, role: "admin")

        match req.path {
            /admin/stats  => 200, json(stats())
            /admin/reload => do { reload(); return 200 }
            _             => 404
        }
    }

    any ** => 404
}
```

#### 3.4.4 Response Callbacks

Proxy accepts a trailing closure to inspect and modify the upstream response before sending it to the client.

```swift
get /users/:id { req in
    auth(req, role: "user")
    proxy(users, timeout: 10s) { resp in
        // Modify response headers
        resp.Server = "gateway"
        resp.X-Powered-By = nil           // remove header (assign nil)
        resp.X-Request-ID = req.X-Request-ID

        // Log upstream errors
        if resp.status >= 500 {
            log.error("upstream error", {
                status: resp.status
                path: req.path
                upstream: "users"
            })
        }

        // Rewrite error response body
        if resp.status == 404 {
            resp.status = 200
            resp.body = json({ users: [], total: 0 })
        }
    }
}
```

#### 3.4.5 Response Caching

Cache upstream responses with declarative syntax:

```swift
get /users/:id { req in
    auth(req, role: "user")
    cache(key: "/users/\(req.id)", ttl: 5m) {
        proxy(users)
    }
}

// Cache with more options
get /products { req in
    cache(
        key: "\(req.path)?\(req.queryString)",
        ttl: 10m,
        staleWhileRevalidate: 30s,     // serve stale while refreshing
        varyBy: [req.Accept-Language],  // vary cache by header
        unless: { $0.status >= 400 }    // don't cache errors
    ) {
        proxy(productService)
    }
}

// Cache invalidation via internal endpoint
post /internal/cache/purge { req in
    ipAllow(req, cidrs: [10.0.0.0/8])
    let pattern = req.body(PurgeRequest).pattern
    cache.purge(pattern: pattern)       // purge by glob pattern
    return 200
}
```

#### 3.4.6 Traffic Mirroring

`fire` sends a request without waiting for the response (fire-and-forget). Useful for traffic shadowing, async logging, webhooks.

```swift
func mirror(_ req: Request, to: string) {
    fire post http://{to}{req.path} {
        Headers: req.headers
        Body: req.bodyRaw
        Timeout: 5s           // timeout for the fire, won't block caller
    }
}

// Usage: mirror production traffic to staging
post /api/orders { req in
    auth(req, role: "user")
    mirror(req, to: "staging-gateway:8080")   // fire-and-forget
    proxy(orders, timeout: 30s)               // actual request
}
```

#### 3.4.7 WebSocket Proxying

```swift
get /ws/chat { req in
    auth(req, role: "user")
    guard req.upgrade == .websocket else { return 400 }
    websocket(chatService)        // transparent WebSocket proxy
}

// With message inspection (optional)
get /ws/events { req in
    guard req.upgrade == .websocket else { return 400 }
    websocket(eventService) { frame in
        // Inspect/filter WebSocket frames
        if frame.isText {
            let msg = frame.text
            guard !msg.contains("forbidden") else {
                return .close(reason: "policy violation")
            }
        }
        return .forward     // pass through
    }
}
```

#### 3.4.8 Streaming Proxy

For large bodies (file uploads, downloads), stream without buffering:

```swift
post /upload { req in
    auth(req, role: "user")
    guard req.contentLength <= 100mb else { return 413 }
    proxy(storageService, streaming: true)    // body streams through, not buffered
}

get /download/:fileId { req in
    proxy(storageService, streaming: true)    // response streams to client
}
```

#### 3.4.9 Conditional Routing

Route to different upstreams based on request attributes:

```swift
let usersV1 = upstream { "10.0.0.1:8080" }
let usersV2 = upstream { "10.0.0.2:8080" }
let canary  = upstream { "10.0.0.3:8080" }
let stable  = upstream { "10.0.0.4:8080" }

// By header
get /api/users/:id { req in
    let target = match req.X-API-Version {
        "v2" => usersV2
        _    => usersV1
    }
    proxy(target)
}

// Canary release: percentage-based
get /api/** { req in
    let hash = fnv32(req.remoteAddr)
    let target = hash % 100 < 10 ? canary : stable   // 10% canary
    proxy(target)
}

// Blue-green via environment variable
get /api/** { req in
    let target = env("DEPLOY_GROUP") == "blue" ? blueUpstream : greenUpstream
    proxy(target)
}
```

#### 3.4.10 HTTP Calls (Native Syntax)

HTTP requests use native method + URL syntax. The compiler automatically handles async — no await needed. User writes sequential code.

```swift
// GET request
let res = get http://auth/verify {
    Authorization: token
}

// POST with body
let res = post http://order-service/create {
    Content-Type: application/json
    Body: order
    Timeout: 10s
}

// Using the response
guard res.status == 200 else { return 502 }
let user = res.body(User)

// Fire-and-forget (non-blocking, don't wait for response)
fire post http://audit-service/log {
    Body: json({ action: "login", userId: user.id, time: now() })
}
```

#### 3.4.11 Module System

```swift
import "middleware/auth.dispatch"
import "middleware/ratelimit.dispatch"
```

#### 3.4.12 External Functions (FFI Escape Hatch)

For rare cases where built-in HTTP calls are insufficient (e.g., direct Redis/database protocol):

```swift
// Declare external function — implementation registered in C++ runtime
extern func redisGet(key: string) -> string?
extern func redisSet(key: string, value: string)

// Usage (compiler knows the types, auto-handles async)
guard let data = redisGet(key: "sess:\(sid)") else { return 401 }
```

C++ side:
```cpp
engine.RegisterExtern("redisGet", [&redis](Str key) -> AsyncResult<Str> {
    return redis.get(key);
});
```

### 3.5 Built-in Capabilities

Everything a gateway needs, with zero external dependencies:

```swift
// --- I/O ---
get/post/put/delete url { }   // HTTP calls to external services
fire post url { }             // fire-and-forget HTTP (traffic mirroring, async logging)
proxy(upstream)                // forward to upstream, with optional response callback
proxy(upstream, streaming:)   // streaming proxy (large body, no buffering)
websocket(upstream)           // WebSocket transparent proxy
counter.incr(key, window:)   // rate limiting counter (per-shard, sliding window)
counter.active(key)          // concurrency counter (auto-decrement on request end)

// --- Traffic Control ---
cache(key:, ttl:) { }        // response caching (per-shard LRU + optional shared)
upstream { breaker: ... }     // circuit breaker (consecutive failures → open → half-open → closed)
upstream { retry: ... }       // retry policy (on specific status codes, with backoff)

// --- Data ---
String:   split, contains, replace, slice, trim, hasPrefix, hasSuffix, upper, lower, isEmpty
JSON:     req.body(Type), json(value), resp.body
Regex:    match(pattern, string)

// --- Security ---
Hash:     md5, sha256, sha1, fnv32
HMAC:     hmacSha256, hmacSha1
Encoding: base64, base64url, hex, urlEncode, urlDecode
JWT:      jwtDecode(token, secret:) -> Claims?
Crypto:   aesEncrypt, aesDecrypt

// --- Time ---
now()                         // current Time
time(string)                  // parse time string
Duration arithmetic           // now() - req.ifModifiedSince > 1h

// --- Utility ---
uuid()                        // generate UUID
env(key)                      // environment variable
log(level, msg)               // structured logging
json(value)                   // serialize to JSON string

// --- Numeric ---
i8, i16, i32, i64             // signed integers, wrapping overflow
u8, u16, u32, u64             // unsigned integers, wrapping overflow
f32, f64                      // floating point
Bit operations: & | ^ << >> ~
```

### 3.6 Compile-Time Checks

| Check | Example | Error |
|-------|---------|-------|
| Route conflict | `get /users/:id` + `get /users/:name` | "conflicting route patterns" |
| Route param | `get /users/:id { req in req.name }` | "route has no param :name" |
| Type error | `User(id: 123)` | "id expects string, got i32" |
| Domain value | `listen :70000` | "port range 1-65535" |
| Duration unit | `timeout: 30x` | "unknown Duration unit" |
| StatusCode range | `return 999` | "invalid status code" |
| Unreachable route | `any **` then `get /after` | "unreachable route" |
| Header type | `req.contentLength = "abc"` | "contentLength is ByteSize, not string" |
| MediaType | `req.contentType == text/lol` | "unknown media type" |
| CIDR format | `req.remoteAddr.in(999.0.0.0/8)` | "invalid IP in CIDR" |
| Header spell | `req.athorization` | "warning: did you mean authorization?" |
| Indirect call | `let f = auth; f(req)` | "functions cannot be assigned to variables" |
| guard exhaustive | `guard let x = opt` (no else) | "guard must have else clause" |
| Optional access | `req.authorization.hasPrefix("B")` | "value is optional, use guard let or ??" |
| Response middleware | `func f(_ req: Request, _ resp: Response)` used as request-only `use` | "this is a response middleware, will run after handler" (info) |

### 3.7 Async Transparency

The language has no `async`, `await`, `future`, or `promise` keywords. User writes sequential code. The compiler identifies I/O operations (HTTP calls, extern functions, proxy) and automatically generates state machines.

```swift
// User writes:
func auth(_ req: Request, role: string) -> User {
    guard let token = req.authorization else { return 401 }
    let res = get http://auth/verify {     // compiler knows: I/O point
        Authorization: token
    }
    guard res.status == 200 else { return 401 }
    let user = res.body(User)
    guard user.role == role else { return 403 }
    req.X-User-ID = user.id
    return user
}

// Compiler generates (after inlining into route handler):
//
//   void handle_get_users(Connection* c) {
//       switch (c->state) {
//       case 0:
//           if (!get_header(c, "Authorization")) { send(c, 401); return; }
//           prep_http_get(c, "http://auth/verify", ...);
//           c->state = 1;
//           return;  // back to event loop, zero stack usage
//       case 1:
//           if (c->upstream_resp.status != 200) { send(c, 401); return; }
//           // ... inline the rest ...
//       }
//   }
```

### 3.8 Swift Syntax Features Used

| Feature | Example | Why |
|---------|---------|-----|
| `guard ... else` | `guard let token = req.authorization else { return 401 }` | Perfect for middleware "reject or continue" pattern |
| Named parameters | `auth(req, role: "user")` | Self-documenting calls, LLMs generate correct argument order |
| `_` unlabeled param | `func auth(_ req: Request, role: string)` | First param (always req) doesn't need label |
| Trailing closure | `get /users/:id { req in ... }` | Clean route handler syntax |
| `$0` shorthand | `cidrs.contains(where: { req.remoteAddr.in($0) })` | Concise closures |
| Optional chaining | `req.authorization?.hasPrefix("Bearer ")` | Safe navigation on nullable headers |
| `let` / `var` | `let x = ...` / `var count = 0` | Immutable by default |
| String interpolation | `"\(req.method)\n\(req.path)"` | Cleaner than concatenation |
| `.enumCase` | `balance: .leastConn` | Concise enum values |
| No commas in blocks | Multi-line blocks don't need commas | Cleaner, less noise |

### 3.9 Complete Example

```swift
// production.dispatch

import "middleware/auth.dispatch"
import "middleware/security.dispatch"

listen :443, tls(cert: env("CERT"), key: env("KEY"))
listen :80, redirect: :443
listen :9090, internal: true    // metrics + admin

// ---------- Upstreams ----------

let users = upstream {
    "10.0.0.1:8080", weight: 3
    "10.0.0.2:8080"
    balance: .leastConn
    health: .get("/ping", every: 5s)
    breaker: .consecutive(failures: 5, recover: 30s)
    retry: .on([502, 503], count: 2, backoff: 100ms)
}

let orders = upstream { "10.0.1.1:8080" }
let storageService = upstream { "10.0.2.1:9000" }
let chatService = upstream { "10.0.3.1:8080" }
let stagingMirror = upstream { "staging:8080" }

// ---------- Types ----------

struct User {
    id: string
    role: string
}

struct Order {
    items: [OrderItem]
}

struct OrderItem {
    sku: string
    qty: i32
    price: f64
}

// ---------- Middleware ----------

func auth(_ req: Request, role: string) -> User {
    guard let token = req.authorization else { return 401 }
    guard token.hasPrefix("Bearer ") else { return 401 }

    let claims = jwtDecode(token.trimPrefix("Bearer "), secret: env("JWT_SECRET"))
    guard let claims else { return 401 }
    guard claims.exp >= now() else { return 401, "token expired" }
    guard claims.role == role else { return 403 }

    req.X-User-ID = claims.sub
    req.X-User-Role = claims.role
    return User(id: claims.sub, role: claims.role)
}

func rateLimit(_ req: Request, limit: i32, window: Duration) {
    let count = counter.incr(req.remoteAddr, window: window)
    guard count <= limit else {
        return 429 { Retry-After: window }
    }
}

func requestId(_ req: Request) {
    if req.X-Request-ID.isEmpty {
        req.X-Request-ID = uuid()
    }
}

func cors(_ req: Request, origins: [string]) {
    let origin = req.origin
    guard origins.contains(origin) else {
        if req.method == .OPTIONS { return 403 }
        return
    }
    if req.method == .OPTIONS {
        return 204 {
            Access-Control-Allow-Origin: origin
            Access-Control-Allow-Methods: "GET, POST, PUT, DELETE"
            Access-Control-Allow-Headers: "Content-Type, Authorization"
            Access-Control-Max-Age: 86400
        }
    }
    req.Access-Control-Allow-Origin = origin
    req.Vary = "Origin"
}

// ---------- Access Log ----------

accessLog {
    format: .json
    output: .stdout
    filter: { $0.duration > 100ms || $0.status >= 400 }
}

// ---------- Routes ----------

route {
    get /health => 200

    // Request middleware (runs before handler)
    use requestId
    use antiFlood
    use waf
    use blockBots
    use cors(origins: ["https://app.example.com"])
    use rateLimit(limit: 1000, window: 1m)

    // Response middleware (runs after handler, before sending to client)
    use securityHeaders

    // --- Users (with caching + response rewrite) ---

    get /users/:id { req in
        let user = auth(req, role: "user")
        cache(key: "/users/\(req.id)", ttl: 5m) {
            proxy(users, timeout: 10s) { resp in
                resp.X-Request-ID = req.X-Request-ID
                resp.X-Powered-By = nil
            }
        }
    }

    post /users { req in
        auth(req, role: "admin")
        guard req.contentLength <= 1mb else { return 413 }
        // mirror to staging for testing
        fire post http://staging:8080\(req.path) {
            Headers: req.headers
            Body: req.bodyRaw
        }
        proxy(users, timeout: 30s)
    }

    // --- Orders (with validation) ---

    post /orders { req in
        let user = auth(req, role: "user")
        let order = req.body(Order)
        guard !order.items.isEmpty else {
            return 400, "order must have items"
        }
        for item in order.items {
            guard item.qty > 0 else { return 400, "invalid quantity" }
            guard item.price >= 0 else { return 400, "invalid price" }
        }
        req.X-User-ID = user.id
        proxy(orders, timeout: 30s)
    }

    // --- File upload (streaming) ---

    post /files/upload { req in
        auth(req, role: "user")
        guard req.contentLength <= 100mb else { return 413 }
        proxy(storageService, streaming: true)
    }

    get /files/:fileId { req in
        auth(req, role: "user")
        proxy(storageService, streaming: true)
    }

    // --- WebSocket ---

    get /ws/chat { req in
        auth(req, role: "user")
        guard req.upgrade == .websocket else { return 400 }
        websocket(chatService)
    }

    // --- Admin (internal) ---

    group /admin { req in
        ipAllow(req, cidrs: [10.0.0.0/8, 172.16.0.0/12])
        auth(req, role: "admin")

        match req.path {
            /admin/stats     => 200, json(stats())
            /admin/reload    => do { reload(); return 200 }
            /admin/cache/purge => do {
                cache.purge(pattern: req.body(PurgeRequest).pattern)
                return 200
            }
            _                => 404
        }
    }

    any ** => 404
}
```

---

### 3.10 OpenResty/Kong/APISIX Feature Coverage

Evaluation of how our DSL covers features from the OpenResty ecosystem (Kong, APISIX, and common Lua patterns).

#### Fully Covered (built-in)

| Feature Category | DSL Capability |
|-----------------|----------------|
| JWT auth | `jwtDecode()` built-in |
| HMAC auth | `hmacSha256()` built-in |
| Basic auth | `base64` decode + string compare |
| API key auth | header/query access + compare |
| Forward auth | native HTTP calls to auth service |
| IP restriction | `req.remoteAddr.in(CIDR)` native |
| UA restriction | `req.userAgent.contains()` |
| CORS | `guard` + header assignment |
| Rate limiting | `counter.incr()` built-in (sliding window) |
| Request size limiting | `req.contentLength` comparison |
| Request ID / Correlation ID | `uuid()` built-in |
| Header add/remove/modify | `req.Header = val` / `= nil` |
| URL rewriting | `req.path` assignment |
| Response header rewrite | `proxy() { resp in resp.Header = val }` |
| Response body rewrite | `proxy() { resp in resp.body = ... }` |
| Request termination (maintenance) | `return 503` |
| Canary release / A/B testing | conditional routing + `fnv32` hash |
| Blue-green deployment | `env()` + conditional proxy |
| Traffic mirroring | `fire` keyword (fire-and-forget HTTP) |
| Circuit breaker | `upstream { breaker: ... }` |
| Retry with backoff | `upstream { retry: ... }` |
| Response caching | `cache(key:, ttl:) { }` |
| Stale-while-revalidate | `cache(staleWhileRevalidate:)` |
| Cache purge | `cache.purge(pattern:)` |
| Streaming proxy | `proxy(upstream, streaming: true)` |
| WebSocket proxy | `websocket(upstream)` |
| Access logging | `accessLog { }` built-in |
| Prometheus metrics | `metrics()` built-in |
| Distributed tracing | auto-instrumentation + `X-Debug` |
| Health checks | `upstream { health: ... }` |
| Dynamic log level | `/internal/log-level` endpoint |
| Structured logging | `log.info/warn/error()` |
| Hot reload | JIT recompile + atomic swap |

#### Covered via HTTP Calls (no special syntax needed)

| Feature | Approach |
|---------|----------|
| OAuth2 / OIDC | HTTP calls to Keycloak/Auth0/Okta |
| LDAP auth | HTTP call to LDAP-HTTP bridge |
| OPA authorization | HTTP call to OPA endpoint |
| Token introspection | HTTP call to OAuth2 introspection endpoint |
| External logging (Kafka, Splunk, etc.) | `fire` to HTTP log collector |
| Webhook delivery | `fire post` to webhook URLs |
| Feature flags | HTTP call to flag service |

#### Phase 2+ (needs HTTP/2 or protocol support)

| Feature | Requirement |
|---------|------------|
| gRPC proxy | HTTP/2 support |
| gRPC-REST transcoding | HTTP/2 + protobuf definitions |
| gRPC-Web | HTTP/2 + gRPC-Web protocol |
| HTTP/2 upstream | HTTP/2 client implementation |
| mTLS (client certs) | OpenSSL client cert validation |
| Dynamic SSL / ACME | ACME protocol + cert management |

#### Out of Scope

| Feature | Reason |
|---------|--------|
| L4 TCP/UDP proxy | This is an L7 HTTP gateway |
| MQTT/Dubbo/Redis protocol proxy | Custom protocol handling |
| Developer portal | Admin UI, separate project |
| Database-backed config | etcd/Postgres, separate concern |
| Multi-language plugin SDK | We have one language + extern FFI |

---

## 4. Runtime Architecture

### 4.1 Shard Model (Share-Nothing)

Each shard is a single OS thread pinned to a single CPU core. Shards do not share mutable state. There are no locks, mutexes, or condition variables anywhere in the hot path.

```
Shard (1 per core):
  ├── 1 OS thread, pinned via sched_setaffinity / pthread_setaffinity_np
  ├── 1 I/O backend (io_uring or epoll, selected at startup)
  ├── 1 listen socket (via SO_REUSEPORT, kernel distributes connections)
  ├── 1 set of connection slots
  ├── 1 upstream connection pool (per upstream target)
  ├── 1 route table pointer (atomically swappable)
  ├── 1 timer wheel
  └── 1 memory region (Arena + Slab + Slice pool)
```

Resource estimation (8 cores, C100K):

| Resource | Per Shard | Total (8 shards) |
|----------|-----------|-------------------|
| Connection metadata | 12,500 x 48B = 600 KB | 4.8 MB |
| SQ ring (16,384 entries) | 1 MB | 8 MB |
| CQ ring (32,768 entries) | 2 MB | 16 MB |
| Provided buffer ring (2,048 x 4KB) | 8 MB | 64 MB |
| Slice pool (1,024 x 16KB) | 16 MB | 128 MB |
| Scratch arena | 64 KB | 512 KB |
| **Total** | **~28 MB** | **~224 MB** |

### 4.2 Accept Distribution

Using `SO_REUSEPORT`: each shard binds and listens on the same port. The kernel distributes incoming connections across shards. Combined with `IORING_ACCEPT_MULTISHOT`, one SQE continuously accepts new connections.

```
┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐
│shard0│  │shard1│  │shard2│  │shard3│
│listen│  │listen│  │listen│  │listen│
│:443  │  │:443  │  │:443  │  │:443  │
└──────┘  └──────┘  └──────┘  └──────┘
      ▲       ▲       ▲       ▲
      └───────┴───────┴───────┘
            kernel distributes
            (SO_REUSEPORT)
```

No cross-shard connection migration. A connection accepted on shard N stays on shard N for its entire lifetime.

### 4.3 I/O Backend Abstraction

The runtime supports two I/O backends behind a compile-time interface (no virtual dispatch):

```cpp
// Compile-time backend selection via template or build flag
// No virtual functions, no runtime overhead

struct IoEvent {
    u32 conn_id;
    u8  type;     // ACCEPT, RECV, SEND, UPSTREAM_CONNECT, UPSTREAM_RECV, TIMEOUT
    i32 result;   // bytes transferred or error code
    u16 buf_id;   // provided buffer id (io_uring only, 0 for epoll)
};

// Backend interface (satisfied by both IoUringBackend and EpollBackend):
//   void init(u32 shard_id);
//   void add_accept(int listen_fd);
//   void add_recv(int fd, u32 conn_id, void* buf, u32 len);
//   void add_send(int fd, u32 conn_id, const void* buf, u32 len);
//   void add_connect(int fd, u32 conn_id, sockaddr* addr);
//   void cancel(int fd, u32 conn_id);
//   u32  wait(IoEvent* events, u32 max_events);  // returns event count
```

#### io_uring Backend (Linux 6.0+, preferred)

```
Characteristics:
  - Completion-based: "I/O is already done when you get the event"
  - One syscall (io_uring_enter) for submit + wait
  - multishot accept: one SQE continuously accepts
  - multishot recv + provided buffer ring: idle connections hold no buffer
  - send_zc: zero-copy send
  - Kernel-side SQ polling (SQPOLL): can reduce to zero syscalls
```

#### epoll Backend (Linux 3.9+, fallback)

```
Characteristics:
  - Readiness-based: "fd is ready, now you do the I/O"
  - Two syscalls per I/O: epoll_wait + recv/send
  - No multishot: must re-arm after each event (EPOLLONESHOT)
    or use level-triggered (default)
  - No provided buffer ring: must pre-assign buffer per connection
    or allocate on readiness notification
  - SO_REUSEPORT still works for accept distribution
```

#### Differences That Affect Upper Layers

```
                        io_uring              epoll
────────────────────────────────────────────────────────────
Event semantics         completion            readiness
Syscalls per I/O        1 (batched)           2 (epoll_wait + read/write)
Idle conn buffer        0 (provided buf ring) must allocate on EPOLLIN
Accept                  multishot (1 SQE)     epoll_wait + accept() loop
recv                    multishot + buf select epoll_wait + recv()
send                    submit + completion   write() + handle EAGAIN
Backpressure            cancel recv SQE       EPOLL_CTL_DEL / stop arming

Upper layers see IoEvent in both cases — the difference is hidden.
```

### 4.4 Event Loop

```cpp
// Generic event loop — works with both backends
// BACKEND is IoUringBackend or EpollBackend (compile-time)

template<typename BACKEND>
void shard_main(int shard_id) {
    BACKEND backend;
    backend.init(shard_id);
    backend.add_accept(listen_fd);

    IoEvent events[256];

    for (;;) {
        u32 n = backend.wait(events, 256);

        for (u32 i = 0; i < n; i++) {
            switch (events[i].type) {
            case ACCEPT:
                handle_new_connection(events[i].result);
                break;
            case RECV:
                handle_recv(events[i].conn_id, events[i]);
                break;
            case SEND:
                handle_send_complete(events[i].conn_id, events[i]);
                break;
            case UPSTREAM_CONNECT:
                handle_upstream_connected(events[i].conn_id, events[i]);
                break;
            case UPSTREAM_RECV:
                handle_upstream_data(events[i].conn_id, events[i]);
                break;
            case TIMEOUT:
                timer_wheel.tick();
                break;
            }
        }
    }
}
```

The event loop code is identical for both backends. All differences are encapsulated inside `BACKEND::wait()` and the `add_*` methods.

### 4.4 Connection Lifecycle

```
State Machine:

  Accept
    │
    ▼
  Idle ◄──────────────────────────────┐  (keep-alive)
    │  multishot recv, no buffer held  │
    │                                  │
    ▼                                  │
  ReadingHeader                        │
    │  provided buffer ring            │
    │  llhttp parse                    │
    ▼                                  │
  ExecHandler                          │
    │  JIT function call               │
    │  scratch arena for temp allocs   │
    ├──► direct response ──► Sending ──┘
    │
    ▼
  Proxying
    │  upstream connect / reuse pool
    │  bidirectional data relay
    │  watermark backpressure
    ▼
  Sending ─────────────────────────────┘
    │  drain send buffer
    │  reset scratch arena
    │  return slices to pool
    ▼
  Idle (or Close)
```

```cpp
struct Connection {
    int      fd;
    u8       state;           // Idle | ReadingHeader | ReadingBody | ExecHandler | Proxying | Sending
    u8       shard_id;
    u16      flags;           // keep_alive, tls, http2, ...
    u32      timer_slot;
    ListNode timer_node;      // intrusive list node for timer wheel
    ListNode idle_node;       // intrusive list node for idle list

    // Only non-null when active
    Slice*   read_slice;
    Slice*   write_slice;

    // Upstream (only when proxying)
    int      upstream_fd;
    u16      upstream_idx;    // index into upstream pool

    // JIT handler state (for async state machine)
    u16      handler_state;
    void*    handler_ctx;     // points into scratch arena
};
// sizeof: 48-64 bytes
```

### 4.6 Idle Connections: C100K Strategy

Most of 100K connections are idle (keep-alive waiting for next request). Idle connections must not hold buffers.

#### io_uring Backend

```
multishot recv + provided buffer ring:

  1. Submit one IORING_OP_RECV with IORING_RECV_MULTISHOT for each connection
  2. Set IOSQE_BUFFER_SELECT flag → kernel picks buffer from provided ring only when data arrives
  3. Idle connections: 1 SQE in kernel, 0 bytes of buffer
  4. Data arrives: kernel grabs a buffer from the ring, fills it, returns CQE
  5. After processing: return buffer to the ring

  Per-shard provided buffer ring:
  // Pre-allocate and register buffer pool
  struct io_uring_buf_ring* buf_ring;
  void* buffers = mmap(NULL, 2048 * 4096, ...);   // 2048 x 4KB = 8MB
  io_uring_register_buf_ring(ring, buf_ring, 2048, BUF_GROUP_ID);

  // Each buffer is 4KB — enough for most HTTP headers
  // If request needs more, allocate from slice pool

  Cost per idle connection: ~0 bytes userspace (1 SQE in kernel)
```

#### epoll Backend

```
epoll does not have provided buffer rings or multishot recv.
Two strategies for idle connections:

Strategy A: allocate on readiness (recommended)
  1. All idle connections registered in epoll (EPOLLIN, level-triggered)
  2. epoll_wait returns ready fds
  3. Only then: grab buffer from slice pool → recv() → parse
  4. After request: return buffer to pool

  Cost per idle connection: 0 bytes buffer (just an epoll entry ~64 bytes in kernel)
  Downside: extra recv() syscall after epoll_wait (vs io_uring where data is already in buffer)

Strategy B: small pre-allocated header buffer
  1. Each connection has a tiny inline buffer (128 bytes) for the first recv
  2. If request header > 128 bytes, spill to slice pool
  3. Covers ~80% of simple GETs without extra allocation

  Cost per idle connection: 128 bytes
  100K × 128 bytes = 12.8 MB — acceptable

  struct Connection {
      // ... other fields ...
      u8 inline_buf[128];    // small inline recv buffer
      Slice* overflow_slice; // nullptr unless header > 128 bytes
  };
```

#### Resource Comparison (8 cores, C100K)

```
                         io_uring          epoll (strategy A)
──────────────────────────────────────────────────────────────
Conn metadata            4.8 MB            4.8 MB
SQ/CQ rings              24 MB             0
epoll fd kernel memory   0                 ~6.4 MB
Provided buffer ring     64 MB             0
Slice pool               128 MB            128 MB
Scratch arena            512 KB            512 KB
──────────────────────────────────────────────────────────────
Total                    ~222 MB           ~140 MB
Syscalls per recv        1 (batched)       2 (epoll_wait + recv)
Zero-copy recv           yes               no
```

---

## 5. Memory Architecture

### 5.1 Design Principles

- No C++ standard library containers (`-nostdlib++`)
- No `malloc`/`free` on the hot path
- All memory pre-allocated at startup via `mmap`
- Per-shard memory isolation (share-nothing)
- Three allocator types for three lifetimes

### 5.2 Compiler and Linker Configuration

```
CXX      = clang++
CXXFLAGS = -std=c++20
           -nostdlib++
           -fno-exceptions
           -fno-rtti
           -fno-unwind-tables
           -fno-asynchronous-unwind-tables
           -ffunction-sections
           -fdata-sections
           -flto

LDFLAGS  = -nostdlib++
           -Wl,--gc-sections
           -static             # static link musl
           -lc                 # libc only (for LLVM)

LLVM_LIBS = -lLLVMOrcJIT -lLLVMX86CodeGen -lLLVMX86AsmParser
            -lLLVMCore -lLLVMSupport
```

Expected binary size: < 500KB without LLVM, ~15-20MB with LLVM statically linked.

### 5.3 Per-Shard Memory Layout

```
┌──────────────────────────────────────────┐
│             Per-Shard Arena              │
│       (single mmap at startup)           │
├──────────┬──────────┬────────────────────┤
│ Provided │ Slab     │ Scratch            │
│ Buffer   │ Pools    │ (per-request)      │
│ Ring     │          │                    │
│ + Slice  │          │                    │
│ Pool     │          │                    │
└──────────┴──────────┴────────────────────┘
```

```cpp
struct PerShardMemory {
    void*  base;
    size_t total_size;

    // 1. io_uring provided buffer ring — for idle connection recv
    //    Kernel auto-selects buffer when data arrives. Zero-copy.
    //    2048 x 4KB = 8MB
    ProvidedBufferRing io_buffers;

    // 2. Slice pool — for active connection read/write buffers
    //    1024 x 16KB = 16MB, free-list recycled
    SlicePool slices;

    // 3. Slab allocators — fixed-size objects
    SlabPool<Connection, 16384>  connections;   // 48-64 bytes each
    SlabPool<UpstreamConn, 4096> upstream_conns;

    // 4. Scratch arena — per-request temporary memory
    //    Reset to zero after each request (one pointer reset)
    Arena scratch;  // 64KB
};
```

### 5.4 Arena Allocator (Per-Request Temporaries)

```cpp
struct Arena {
    u8* base;
    u8* ptr;
    u8* end;

    void* alloc(u32 size) {
        size = (size + 7) & ~7;  // 8-byte align
        void* p = ptr;
        ptr += size;
        return p;
    }

    void reset() { ptr = base; }  // one instruction, "frees" everything
};
```

Used for: parsed request headers, route match params, JIT handler temporary variables. Entire request's temp memory freed with a single pointer reset.

### 5.5 Slice Buffer (Network Data Streams)

```
Each Slice:
┌─────────────────────────────────────┐
│  drained  │   data    │  reservable │
└─────────────────────────────────────┘
            ↑           ↑
           start       end

- Fixed 16KB per slice
- Drain: advance start pointer, O(1)
- Append: advance end pointer, O(1)
- When fully drained: return to free list (O(1))
```

```cpp
struct Slice {
    u8  buf[16384];
    u32 start;
    u32 end;

    u32 len()      { return end - start; }
    u8* data()     { return buf + start; }
    u32 writable() { return sizeof(buf) - end; }

    u32 append(const u8* src, u32 n) {
        u32 to_copy = min(n, writable());
        __builtin_memcpy(buf + end, src, to_copy);
        end += to_copy;
        return to_copy;
    }

    void drain(u32 n) { start += n; }
};

struct SliceBuffer {
    Slice* slices[64];     // max 64 slices = 1MB logical buffer
    u32    head, tail;     // ring buffer indices
    Slice* free_list;      // recycling pool

    void append(const u8* src, u32 n);  // write to tail slice(s)
    void drain(u32 n);                  // consume from head slice(s), recycle empty
};
```

Used for: network recv/send data, proxy body relay. Can be registered with io_uring for zero-copy I/O.

### 5.6 Slab Allocator (Fixed-Size Objects)

```cpp
template<typename T, u32 Capacity>
struct SlabPool {
    T      storage[Capacity];
    u32    free_stack[Capacity];
    u32    free_top;

    T* alloc() {
        u32 idx = free_stack[--free_top];
        return &storage[idx];
    }

    void free(T* obj) {
        u32 idx = obj - storage;
        free_stack[free_top++] = idx;
    }
};
```

Used for: Connection objects, upstream connection objects. Constant-time alloc/free, no fragmentation.

### 5.7 Stdlib Replacement Types

```cpp
// Fixed-capacity vector, no heap allocation
template<typename T, u32 Cap>
struct FixedVec {
    T   data[Cap];
    u32 len = 0;

    void push(T val) { data[len++] = val; }
    T&   operator[](u32 i) { return data[i]; }
    T*   begin() { return data; }
    T*   end()   { return data + len; }
};

// Non-owning string view
struct Str {
    const char* ptr;
    u32 len;

    bool eq(Str other) const {
        return len == other.len &&
               __builtin_memcmp(ptr, other.ptr, len) == 0;
    }
};

// Open-addressing flat hash map, inline storage
template<typename K, typename V, u32 Cap>
struct FlatMap {
    struct Slot { K key; V val; u8 state; };  // 0=empty 1=used 2=deleted
    Slot slots[Cap];
};

// Intrusive linked list (for connection lists, timer wheel, LRU)
struct ListNode {
    ListNode* prev;
    ListNode* next;
};
```

### 5.8 Request Lifecycle Memory Flow

```
  I/O backend recv:
    io_uring: multishot recv → provided buffer (4KB, zero-copy from kernel)
    epoll:    EPOLLIN → alloc slice from pool → recv() into slice
       │
       ▼
  llhttp parse headers → Str slices pointing into buffer (zero-copy)
       │
       ▼
  Arena alloc Request struct + header kv array + route params
       │
       ▼
  JIT handler executes (temp allocs from arena)
       │
       ├── Direct response → write to slice buffer → backend send
       │
       └── Proxy → slice buffer relay between downstream/upstream
       │
       ▼
  Request complete:
       arena.reset()                    ← one instruction
       return buffer to pool/ring       ← one pointer write
       return slices to free list       ← linked list ops

  Hot path: zero malloc, zero free
  io_uring: zero extra syscalls (batched in io_uring_enter)
  epoll:    +1 syscall per recv, +1 per send
```

---

## 6. I/O Backend Implementations

### 6.1 Backend Selection

```
Startup detection:

  1. Try io_uring_setup() syscall
     - Success + kernel >= 6.0 → use io_uring backend
     - Fail (ENOSYS) or old kernel → fall back to epoll

  2. Can be overridden via command line:
     --io-backend=io_uring    # force io_uring (fail if unavailable)
     --io-backend=epoll       # force epoll

  3. Selected once at startup, all shards use the same backend
     (binary contains both implementations, compiled via templates)
```

### 6.2 io_uring Backend (Linux 6.0+)

No liburing dependency. Direct syscall wrapper for full control (~300 lines):

```cpp
struct IoUringBackend {
    int            ring_fd;
    io_uring_sqe*  sq_entries;
    io_uring_cqe*  cq_entries;
    u32*           sq_tail;
    u32*           cq_head;
    u32            sq_mask;
    u32            cq_mask;

    void init(u32 shard_id) {
        io_uring_params params = {};
        params.flags = IORING_SETUP_SQPOLL
                     | IORING_SETUP_SINGLE_ISSUER
                     | IORING_SETUP_COOP_TASKRUN;
        ring_fd = syscall(__NR_io_uring_setup, 16384, &params);
        // mmap sq/cq rings...
        // setup provided buffer ring...
    }

    io_uring_sqe* get_sqe() {
        u32 tail = __atomic_load_n(sq_tail, __ATOMIC_RELAXED);
        io_uring_sqe* sqe = &sq_entries[tail & sq_mask];
        __atomic_store_n(sq_tail, tail + 1, __ATOMIC_RELEASE);
        return sqe;
    }

    u32 wait(IoEvent* events, u32 max_events) {
        syscall(__NR_io_uring_enter, ring_fd,
                pending_count, 1,
                IORING_ENTER_GETEVENTS, nullptr, 0);
        // harvest CQEs → convert to IoEvent array
    }

    void add_accept(int fd) {
        // IORING_ACCEPT_MULTISHOT — one SQE, continuous accept
    }

    void add_recv(int fd, u32 conn_id, void*, u32) {
        // IORING_RECV_MULTISHOT + IOSQE_BUFFER_SELECT
        // buffer arg ignored, kernel picks from provided ring
    }

    void add_send(int fd, u32 conn_id, const void* buf, u32 len) {
        // IORING_OP_SEND or IORING_OP_SEND_ZC
    }
};
```

io_uring features used:

| Feature | Purpose | Kernel Version |
|---------|---------|----------------|
| `IORING_ACCEPT_MULTISHOT` | One SQE continuously accepts connections | 5.19 |
| `IORING_RECV_MULTISHOT` | One SQE continuously receives data per connection | 6.0 |
| `IOSQE_BUFFER_SELECT` + provided buffer ring | Kernel auto-selects buffer on recv, idle connections hold no buffer | 5.19 / 6.0 |
| `IORING_SETUP_SQPOLL` | Kernel-side SQ polling, reduces syscalls | 5.11 |
| `IORING_SETUP_SINGLE_ISSUER` | Single-thread optimization | 6.0 |
| `IORING_OP_SEND_ZC` | Zero-copy send | 6.0 |
| `IORING_OP_LINK` | Chain multiple ops, single submission | 5.3 |
| `IORING_SETUP_COOP_TASKRUN` | Cooperative task running, fewer interrupts | 6.0 |
| Fixed file registration | Pre-register fds, skip kernel fd lookup | 5.1 |
| Fixed buffer registration | Pre-register buffers, skip DMA mapping | 5.1 |

### 6.3 epoll Backend (Linux 3.9+)

Fallback for older kernels. ~250 lines:

```cpp
struct EpollBackend {
    int epoll_fd;
    int timer_fd;

    void init(u32 shard_id) {
        epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        // arm timerfd for 1s ticks
    }

    u32 wait(IoEvent* events, u32 max_events) {
        epoll_event ep_events[256];
        int n = epoll_wait(epoll_fd, ep_events, 256, -1);

        u32 out = 0;
        for (int i = 0; i < n; i++) {
            auto [conn_id, type] = decode(ep_events[i].data.u64);

            if (type == LISTEN && (ep_events[i].events & EPOLLIN)) {
                // accept loop (no multishot, must loop)
                for (;;) {
                    int fd = accept4(listen_fd, nullptr, nullptr,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (fd < 0) break;
                    events[out++] = { .conn_id = 0, .type = ACCEPT, .result = fd };
                }
            }
            else if (ep_events[i].events & EPOLLIN) {
                // readiness notification — do the actual recv here
                Connection* c = &conns[conn_id];
                Slice* s = alloc_slice();       // allocate buffer NOW
                int nr = recv(c->fd, s->data(), s->writable(), 0);
                if (nr <= 0) {
                    free_slice(s);
                    events[out++] = { .conn_id = conn_id, .type = RECV, .result = nr };
                } else {
                    s->end = nr;
                    c->read_slice = s;
                    events[out++] = { .conn_id = conn_id, .type = RECV, .result = nr };
                }
            }
            else if (ep_events[i].events & EPOLLOUT) {
                // writable — do the send here
                Connection* c = &conns[conn_id];
                int nw = send(c->fd, c->write_slice->data(), c->write_slice->len(), MSG_NOSIGNAL);
                events[out++] = { .conn_id = conn_id, .type = SEND, .result = nw };
            }
        }
        return out;
    }

    void add_recv(int fd, u32 conn_id, void*, u32) {
        epoll_event ev = { .events = EPOLLIN, .data.u64 = encode(conn_id, CONN) };
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
        // actual recv happens inside wait() when EPOLLIN fires
    }

    void add_send(int fd, u32 conn_id, const void* buf, u32 len) {
        // try immediate send first (common case: socket buffer not full)
        int nw = ::send(fd, buf, len, MSG_NOSIGNAL);
        if (nw == (int)len) {
            // sent everything, queue synthetic completion event
            pending_completions.push({ .conn_id = conn_id, .type = SEND, .result = nw });
            return;
        }
        // partial send or EAGAIN: register for EPOLLOUT
        epoll_event ev = { .events = EPOLLOUT, .data.u64 = encode(conn_id, CONN) };
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
};
```

### 6.4 Backend Comparison

```
                         io_uring              epoll
──────────────────────────────────────────────────────────────
Model                    completion            readiness
Syscalls per I/O         1 (batched)           2 (epoll_wait + recv/send)
Accept                   multishot (1 SQE)     accept4() loop
Idle conn buffer         0 (provided buf ring) 0 (alloc on readiness)
Zero-copy recv           yes                   no
Zero-copy send           yes (SEND_ZC)         no (must copy to kernel)
Kernel version           6.0+                  3.9+ (SO_REUSEPORT)
Code size                ~300 lines            ~250 lines
Latency (per I/O)        lower                 higher (~1 extra syscall)
Throughput               higher                ~80-90% of io_uring
──────────────────────────────────────────────────────────────
Both produce the same IoEvent stream to the event loop.
Upper layers are completely unaware of which backend is active.
```

### 6.5 Reactor vs Proactor Model

The two backends represent two I/O models:

```
Reactor (epoll):
  "fd is ready, you do the I/O"

  epoll_wait() → "fd 5 is readable"
               → you call recv(fd5, buf, ...)     ← you do the I/O
               → "fd 8 is writable"
               → you call send(fd8, buf, ...)     ← you do the I/O

Proactor (io_uring):
  "I/O is already done, here's the result"

  you submit: "read from fd5 into buf"
  io_uring_enter() → "fd5 data is in buf, 1024 bytes read"
                   → "fd8 data sent, 512 bytes written"
```

#### Why Not Choose One?

```
Reactor advantages (pure networking):
  ├── Simpler — you fully control when recv/send happens
  ├── Speculative reads — recv in loop until EAGAIN after readiness
  ├── No SQE/CQ ring memory overhead
  └── Low kernel version requirement (3.9+)

Proactor advantages:
  ├── Fewer syscalls — submit + wait = 1 syscall (vs 2 for reactor)
  ├── Batch submission — 100 sends in one io_uring_enter
  ├── Provided buffer ring — idle connections hold zero buffer
  ├── Multishot — accept/recv without re-submitting
  └── SQPOLL — zero syscalls in extreme case
```

#### Our Design: Unified Completion Interface

```
We don't choose — we use both, behind a unified completion (proactor) API.

  - io_uring backend: native proactor, optimal performance
  - epoll backend: reactor internally, but wait() does the recv/send
    and emits IoEvent completions — "reactor disguised as proactor"

  Upper layers always see completion events (IoEvent).
  Upper layer code is written once.

  ┌──────────────────────────────┐
  │      Event Loop (generic)     │
  │      handles IoEvent[]        │
  └──────────┬───────────────────┘
             │
  ┌──────────┴───────────────────┐
  │    IoEvent abstraction        │
  ├──────────────┬────────────────┤
  │ IoUringBackend│  EpollBackend  │
  │ (proactor)    │  (reactor →    │
  │               │   proactor     │
  │               │   adapter)     │
  └──────────────┴────────────────┘
```

This approach:
- `io_uring` available → best performance, fewer syscalls, zero-copy
- `io_uring` unavailable → epoll fallback, ~80-90% throughput, still correct
- Application code unchanged between backends

---

## 7. Networking

### 7.1 HTTP Parsing

Custom HTTP/1.1 request parser (~500-800 lines C++). No external dependency.

```
Why not llhttp or picohttpparser:
  llhttp:          callback-based API (bad fit), no SIMD, 3000 lines, generated code
  picohttpparser:  better but no strict mode, can't integrate with Slice buffer

Custom parser advantages:
  - Direct integration with Slice buffer (can parse across slice boundaries)
  - Zero-copy: emits Str slices pointing into recv buffer
  - SIMD-accelerated header scanning (SSE4.2/AVX2: find \r\n and ': ' in 16/32 bytes)
  - Strict mode for anti-smuggling (reject ambiguous requests)
  - Only parses requests (not responses — we don't need response parsing on the proxy path)
  - Arena-allocated output (ParsedRequest struct + header array)
```

```cpp
struct ParsedRequest {
    Str method;
    Str path;
    Str query;
    u8  version;         // 0 = HTTP/1.0, 1 = HTTP/1.1
    struct { Str key; Str val; } headers[64];
    u16 header_count;
    u32 content_length;
    bool chunked;
    bool keep_alive;
    u32  header_end;     // offset where body starts
};

// Returns: >0 = complete (header length), 0 = need more data, <0 = error
int parse_request(const u8* buf, u32 len, ParsedRequest* out);
```

### 7.2 Routing

Compile-time built **radix trie**. The JIT compiler can compile the trie traversal into a series of switch/jump instructions.

```
/api/v1/users       → handler_1
/api/v1/users/:id   → handler_2  (captures req.params.id)
/api/v2/users       → handler_3
/health             → handler_4

Compiled to radix trie:
  /
  ├── api/v
  │   ├── 1/users
  │   │   ├── (end) → handler_1
  │   │   └── / :id → handler_2
  │   └── 2/users → handler_3
  └── health → handler_4
```

Compile-time checks:
- Route conflicts (two patterns matching same path)
- Unreachable routes (shadowed by earlier catch-all)
- Parameter name consistency

### 7.3 Upstream Connection Pool

Per-shard, per-upstream-target. No cross-shard sharing.

Connection states (borrowed from Envoy):

```
Connecting → Ready → Busy → Draining → Closed

- Ready: available for new requests
- Busy: currently serving a request
- Draining: no new requests, waiting for in-flight to complete
```

### 7.4 Backpressure (Watermark)

Borrowed from Envoy's watermark buffer design:

```
downstream recv → buffer grows
                     │
          exceeds high watermark (e.g., 64KB)
                     │
          pause downstream recv (cancel io_uring recv SQE)
                     │
          upstream consumes buffer → buffer shrinks
                     │
          drops below low watermark (e.g., 32KB)
                     │
          resume downstream recv (re-submit io_uring recv)
```

This provides end-to-end flow control without explicit coordination.

### 7.5 TLS

Two directions: **inbound** (client → gateway) and **outbound** (gateway → upstream/external service).

#### 7.5.1 Inbound TLS (TLS Termination)

```
Connection flow:

  accept()
    │
    ▼
  TLS handshake (io_uring recv/send + OpenSSL state machine)
    │  ClientHello → ServerHello → Certificate → KeyExchange → Finished
    │  2-4 RTT for full handshake, 1 RTT for session resumption
    ▼
  Handshake complete → plaintext read/write channel
    │
    ▼
  Normal HTTP parsing (identical to plaintext path)
```

Implementation strategy (phased):

```
Phase 2: User-space OpenSSL + BIO_s_mem

  ┌────────────────────────────────────────────────────┐
  │                   Connection                        │
  │                                                     │
  │  io_uring recv → [ciphertext Slice] → BIO_write()  │
  │                                        │            │
  │                        SSL_read() ◄────┘            │
  │                           │                         │
  │                           ▼                         │
  │              [plaintext Slice] → HTTP parser        │
  │                                                     │
  │  HTTP response → SSL_write() → BIO_read()          │
  │                                   │                 │
  │                                   ▼                 │
  │                      [ciphertext Slice]             │
  │                           │                         │
  │                    io_uring send                     │
  └────────────────────────────────────────────────────┘

  OpenSSL integration:
    BIO* rbio = BIO_new(BIO_s_mem());   // read BIO: write ciphertext in
    BIO* wbio = BIO_new(BIO_s_mem());   // write BIO: read ciphertext out
    SSL_set_bio(ssl, rbio, wbio);

    // On recv (ciphertext arrives):
    BIO_write(rbio, ciphertext, len);
    int n = SSL_read(ssl, plaintext, sizeof(plaintext));
    // plaintext now contains decrypted HTTP data

    // On send (plaintext to encrypt):
    SSL_write(ssl, plaintext, len);
    int n = BIO_read(wbio, ciphertext, sizeof(ciphertext));
    // submit io_uring send with ciphertext

Phase 3 optimization: kTLS (kernel TLS)

  After handshake in user-space, switch data path to kernel:
    setsockopt(fd, SOL_TLS, TLS_TX, &crypto_info, sizeof(crypto_info));
    setsockopt(fd, SOL_TLS, TLS_RX, &crypto_info, sizeof(crypto_info));

  io_uring recv/send now operate on plaintext directly
  → zero-copy, kernel handles encrypt/decrypt
  → requires Linux 4.13+ (kTLS), 5.x+ (mature), specific cipher suites
  → fallback to user-space OpenSSL if kTLS unavailable
```

SNI (Server Name Indication) — multiple certificates per port:

```swift
// Single certificate (simple)
listen :443, tls(cert: env("CERT"), key: env("KEY"))

// Multiple domains with different certificates
listen :443 {
    tls "api.example.com", cert: "/certs/api.pem", key: "/certs/api.key"
    tls "admin.example.com", cert: "/certs/admin.pem", key: "/certs/admin.key"
    tls default, cert: "/certs/default.pem", key: "/certs/default.key"
}
```

```
SNI implementation:
  - OpenSSL SNI callback (SSL_CTX_set_tlsext_servername_callback)
  - Lookup hostname from ClientHello → select SSL_CTX with matching certificate
  - Per-shard certificate cache (share-nothing)
  - Certificate reload on hot reload (new .dispatch → new cert paths → reload certs)
```

TLS Session Resumption — avoid full handshake on reconnect:

```
Two mechanisms (both enabled by default):

  Session ID cache:
    - Per-shard in-memory cache
    - Key: session ID (32 bytes) → Value: SSL session state
    - LRU eviction, configurable max size
    - Saves 1 RTT on reconnect

  Session Tickets:
    - Server encrypts session state, sends to client as ticket
    - Client presents ticket on reconnect → server decrypts → resume
    - No server-side storage needed
    - Ticket encryption key rotated periodically (configurable)
    - Key shared across shards (one of the few cross-shard shared items)
```

TLS configuration in language:

```swift
listen :443 {
    tls "api.example.com", cert: "/certs/api.pem", key: "/certs/api.key"

    // TLS security settings
    tlsMinVersion: .tls12           // minimum TLS 1.2 (reject TLS 1.0/1.1)
    tlsCiphers: [
        "TLS_AES_256_GCM_SHA384",
        "TLS_AES_128_GCM_SHA256",
        "TLS_CHACHA20_POLY1305_SHA256",
    ]
    tlsSessionTimeout: 4h          // session cache TTL
    tlsTicketRotation: 12h         // rotate ticket keys
}
```

#### 7.5.2 Outbound TLS (Gateway → External Services)

```
Already designed in Section 13.4. Summary:

  http://  → plaintext connection
  https:// → automatic TLS handshake + server cert verification

  Implementation: same OpenSSL + BIO_s_mem mechanism
  Session caching: per-shard, per-host:port → reuse TLS sessions on reconnect
  mTLS (client cert): optional, via ClientCert parameter in HTTP call syntax

  let res = get https://api.stripe.com/charges {
      Authorization: "Bearer \(env("STRIPE_KEY"))"
  }
  // TLS handled transparently, session cached for future calls to same host
```

#### 7.5.3 Dependency

```
OpenSSL 3.x (dynamic link, ~3MB .so)
  - Most mature TLS library
  - System usually has it installed
  - Security updates via system package manager
  - Alternative: BoringSSL (lighter, Google maintains for Chrome)
    but no stable API guarantee

Not considered:
  - Custom TLS: too complex, security-critical code
  - LibreSSL: less widely deployed
  - mbedTLS: missing some features (kTLS integration)
  - rustls: Rust, would need FFI bridge
```

#### 7.5.4 Memory Impact

```
Per TLS connection (additional to plaintext connection):
  SSL object:          ~2-4 KB (OpenSSL internal state)
  BIO buffers:         2 x 16 KB (read + write)
  Session cache entry: ~200 bytes

C100K with TLS:
  100K × 4KB SSL state  = 400 MB    ← significant
  100K × 32KB BIO       = 3.2 GB   ← too much

Optimization: lazy BIO allocation (same as idle connection strategy)
  Idle TLS connections: SSL object only (~4KB), no BIO buffers
  Active TLS connections: SSL + BIO buffers allocated on demand

  Typical: 100K connections, 5K active
  5K × 36KB + 95K × 4KB = 180MB + 380MB = 560MB
  Acceptable for a TLS-terminating gateway
```

#### 7.5.5 Implementation Phases

```
Phase 1: No TLS
  - Use upstream LB / HAProxy for TLS termination
  - Gateway listens on plaintext only

Phase 2: Inbound TLS + Outbound TLS
  - OpenSSL + BIO_s_mem + io_uring
  - SNI for multi-domain
  - Session ID cache + Session Tickets
  - TLS config in .dispatch files
  - Outbound HTTPS for HTTP calls

Phase 3: Optimization + Advanced
  - kTLS for data path (zero-copy after handshake)
  - mTLS (client certificate validation)
  - OCSP stapling
  - Certificate hot-reload without connection drop
  - Dynamic certificates (e.g., Let's Encrypt via extern)
```

---

## 8. Cross-Shard Communication

### 8.1 Principle

No locks. No mutexes. No condition variables. Four specific cross-shard scenarios, each with a dedicated lock-free solution.

### 8.2 Global Rate Limiting

**Default: per-shard partitioning (no communication)**

```
// Global limit: 1000 req/s, 8 shards
// Each shard: 125 req/s
// Accuracy: ±8 req/s — acceptable for rate limiting

local limit: u32 = global_limit / shard_count
```

**Optional: precise global counting (atomic)**

```cpp
alignas(64) std::atomic<u32> global_counter;  // own cache line, no false sharing

// Per shard:
u32 count = global_counter.fetch_add(1, std::memory_order_relaxed);
if (count > limit) reject();
```

### 8.3 Global Statistics

Per-shard local counters, aggregated on read:

```cpp
struct ShardStats {
    alignas(64) u64 requests;     // own cache line
    alignas(64) u64 errors;
    alignas(64) u64 bytes_in;
    alignas(64) u64 bytes_out;
};

ShardStats per_shard_stats[MAX_SHARDS];

// Write (hot path): each shard writes only its own, zero contention
per_shard_stats[my_shard].requests++;

// Read (metrics endpoint, low frequency):
u64 total = 0;
for (int i = 0; i < num_shards; i++)
    total += per_shard_stats[i].requests;
```

### 8.4 Configuration Hot Reload

RCU (Read-Copy-Update) pattern:

```cpp
struct Config {
    RouteTable* routes;
    HandlerFn*  handlers;
    u64         version;
};

std::atomic<Config*> current_config;

// Reload (infrequent):
void reload(Config* new_config) {
    Config* old = current_config.exchange(new_config, std::memory_order_release);
    wait_for_all_shards_epoch_advance();  // wait for in-flight requests
    free_jit_code(old);
}

// Per-shard request handling (hot path):
void handle(Connection* c) {
    Config* cfg = current_config.load(std::memory_order_acquire);  // ~1ns
    epoch_enter();
    // ... process request ...
    epoch_leave();
}
```

### 8.5 Health Check Results

**Option A: each shard runs independent health checks (simplest, fully share-nothing)**

8 shards x 1 check/5s = upstream receives 8 health checks per interval. Negligible overhead.

**Option B: single shard checks, broadcast via atomic bitmap**

```cpp
alignas(64) std::atomic<u64> healthy_mask;  // up to 64 upstreams

// Shard 0 writes:
healthy_mask.store(new_mask, std::memory_order_release);

// Other shards read:
u64 mask = healthy_mask.load(std::memory_order_acquire);
if (!(mask & (1 << upstream_id))) skip_upstream();
```

### 8.6 Language-Level Exposure

```
// Users don't need to know about atomics, cache lines, or RCU
// The type system guarantees safety

local counter: u32 = 0           // compiles to plain variable
shared total: u64 = 0            // compiles to per-shard counter + aggregated read
shared rate: u32 = 0             // compiles to atomic

// Compiler rejects unsafe shared patterns:
shared map: Map<string, u32> = {}  // ❌ compile error: shared does not support Map
shared counter += 1                // ✅ compiles to fetch_add
```

---

## 9. Timer Wheel

For managing 100K connection timeouts (keep-alive, request timeout, health check intervals).

```
Timer Wheel:
  Resolution: 1 second
  Slots: 60 (covers 60s keep-alive timeout)
  Each slot: intrusive linked list of connections
  Driven by: single timerfd per shard

  ┌───┬───┬───┬───┬───┬─── ... ───┬───┐
  │ 0 │ 1 │ 2 │ 3 │ 4 │           │59 │
  └───┴───┴───┴───┴───┴─── ... ───┴───┘
    ↑ cursor (advances each second)
```

```cpp
struct TimerWheel {
    ListNode slots[60];
    u32 cursor;

    // Add timeout: O(1)
    void add(Connection* c, u32 seconds) {
        u32 slot = (cursor + seconds) % 60;
        list_append(&slots[slot], &c->timer_node);
    }

    // Refresh on activity: O(1)
    void refresh(Connection* c, u32 seconds) {
        list_remove(&c->timer_node);
        add(c, seconds);
    }

    // Tick (called every second): close all timed-out connections
    void tick() {
        ListNode* node = slots[cursor].head;
        while (node) {
            Connection* c = container_of(node, Connection, timer_node);
            close_connection(c);
            node = node->next;
        }
        cursor = (cursor + 1) % 60;
    }
};
```

All operations O(1). One timerfd drives the entire wheel per shard.

---

## 10. Hot Reload

### 10.1 Overview

No process restart. No fork. No fd passing. No upstream reconnection. Just atomic pointer swap.

Comparison with nginx:

```
                     nginx reload              Our hot reload
─────────────────────────────────────────────────────────────────
Trigger              SIGHUP → master fork      file change → compiler thread
Compilation          master parses new conf     compiler thread: parse → typecheck → JIT
New code activation  fork new worker processes  atomic swap function pointers
Old code retirement  old worker process exits   ref_count → 0, release JIT memory
Connection migration none (old worker drains)   none needed (same shard, new handler)
Upstream connections rebuilt (new worker, new pool) preserved (pool untouched, only handler changes)
Memory during swap   2x (two sets of workers)   + ~100-200KB (one extra JIT code copy)
Activation latency   seconds (fork + init)      milliseconds (atomic swap)
Failure handling     parse fails → no fork      compile fails → no swap
```

### 10.2 Compilation Thread

Compilation runs on a dedicated background thread (not on any shard), so request processing is never blocked.

```cpp
struct CompilerThread {
    std::thread thread;
    int         notify_fd;       // eventfd, triggered by inotify or API call
    Duration    debounce = 500ms; // wait before compiling (coalesce rapid changes)

    void run() {
        for (;;) {
            eventfd_read(notify_fd, ...);

            // Debounce: wait 500ms, restart if file changes again
            while (wait_for_more_changes(debounce)) {}

            auto source = read_file(config_path);
            auto result = compiler.compile(source);

            if (result.has_error) {
                log_error("compile failed", { errors: result.errors });
                // Don't swap — old config keeps running
                // Error visible at /internal/health
                continue;
            }

            initiate_swap(result.config);
        }
    }
};
```

### 10.3 LLVM ORC JIT Compilation

```
Each compilation produces an isolated JITDylib (LLVM dynamic library):

  1. Parse → Typed AST → Inline expand → DIR → Optimize
  2. Generate LLVM IR from DIR
  3. Create new JITDylib: jit->createJITDylib("config_v3")
  4. Add IR module → ORC JIT compiles to native machine code
  5. Resolve function pointers: jit->lookup(dylib, "handle_get_users_id")
  6. Build route table (radix trie) pointing to new function pointers

  Result: CompiledConfig with version, route table, handler pointers, and JITDylib reference
```

```cpp
struct CompiledConfig {
    u64                     version;
    RouteTable*             routes;          // radix trie
    HandlerFn*              handlers;        // function pointer array
    u32                     handler_count;
    llvm::orc::JITDylib*    dylib;           // holds JIT memory
};
```

### 10.4 Atomic Swap

```cpp
// Global atomic pointer — the only cross-shard shared mutable state
alignas(64) std::atomic<CompiledConfig*> g_current_config;

// Swap (on compiler thread):
void initiate_swap(CompiledConfig* new_config) {
    CompiledConfig* old = g_current_config.exchange(
        new_config, std::memory_order_release
    );
    // old goes into pending release queue
    pending_release.push(old);
    log_info("config swapped", { from: old->version, to: new_config->version });
}
```

```
Timeline:

  Shard 0           Shard 1           Compiler Thread
  ─────────         ─────────         ───────────────
  req A (v1)        req C (v1)        compile v2...
  req B (v1)        req D (v1)        done!
                                      swap: g_current_config = v2
  req E (v2) ←── new requests use v2 ──→ req F (v2)
  req A (v1) ←── in-flight stays on v1
  req A done ──→ epoch advance
  req B done ──→ epoch advance
                  req C done ──→ epoch advance
                  req D done ──→ epoch advance
                                      check: all epochs advanced → release v1
```

### 10.5 In-Flight Request Handling

When config swaps mid-request (e.g., async handler yielded, waiting for upstream), the request continues using the config it started with.

```cpp
struct Connection {
    // ... other fields ...
    CompiledConfig* active_config;    // config bound at request start
    HandlerFn       active_handler;   // handler bound at request start
};

void on_request_start(Connection* c, int shard_id) {
    // Bind config for this request's entire lifetime
    shard_epochs[shard_id].epoch++;   // odd = processing (enter)
    c->active_config = g_current_config.load(std::memory_order_acquire);

    // Route match + bind handler
    auto idx = c->active_config->routes->match(c->parsed->method, c->parsed->path);
    c->active_handler = c->active_config->handlers[idx];
    c->handler_state = 0;
    c->active_handler(c);
}

void on_io_complete(Connection* c) {
    // I/O completed (upstream responded), continue same handler (same version)
    c->active_handler(c);
}

void on_request_complete(Connection* c, int shard_id) {
    c->active_config = nullptr;
    c->active_handler = nullptr;
    shard_epochs[shard_id].epoch++;   // even = idle (leave)
}
```

### 10.6 Old JIT Code Release (Epoch-Based Reclamation)

```cpp
// Per-shard epoch counter — each shard only writes its own (no contention)
struct alignas(64) ShardEpoch {
    u64 epoch;          // odd = processing request, even = idle
    u64 padding[7];     // pad to 64 bytes (own cache line, no false sharing)
};
ShardEpoch shard_epochs[MAX_SHARDS];

// Compiler thread records epoch snapshot at swap time
struct PendingRelease {
    CompiledConfig* config;
    u64 swap_epochs[MAX_SHARDS];   // epoch values at swap time
    Timestamp swap_time;
};

// Compiler thread periodically checks if old configs can be released
void try_release_old_configs() {
    for (auto& pr : pending_releases) {
        bool safe = true;
        for (int i = 0; i < num_shards; i++) {
            u64 e = shard_epochs[i].epoch;
            // Shard has advanced past the swap point?
            if (e <= pr.swap_epochs[i] && (e & 1)) {
                safe = false;  // still processing a pre-swap request
                break;
            }
        }
        if (safe) {
            release_jit_code(pr.config);
            pending_releases.remove(pr);
        }
    }
}

void release_jit_code(CompiledConfig* config) {
    // 1. Remove LLVM JITDylib → munmap executable pages → memory returned to OS
    jit->removeJITDylib(*config->dylib);

    // 2. Free route table
    delete config->routes;

    // 3. Free config struct
    delete config;

    log_info("released JIT code", { version: config->version });
}
```

```
Hot path overhead per request:
  atomic load g_current_config:    ~1ns
  local epoch write (enter):       ~1ns
  local epoch write (leave):       ~1ns
  ─────────────────────────────
  Total:                           ~3ns per request
```

### 10.7 Edge Cases

```
1. Compile failure
   → Don't swap, old config keeps running
   → Error reported at /internal/health and in logs

2. Rapid successive changes (5 changes in 10 seconds)
   → Debounce: 500ms wait after last change before compiling
   → At most ~2 compilations per second
   → Multiple old versions can coexist (each ~100-200KB JIT code, trivial)

3. Slow request holds old config (e.g., 60s upstream timeout)
   → Request timeout guarantees eventual completion
   → Old JIT code released within max(request_timeout) after swap
   → Normal case: old code freed in 1-5 seconds

4. Safety net for stuck requests
   → If pending release > 5 minutes: log.warn (but don't force release)
   → Force release = crash (handler code pulled from under running request)
   → Should never happen if timeouts are configured correctly

5. LLVM JITDylib memory actually freed?
   → Yes. removeJITDylib() calls munmap on executable memory pages
   → Symbol tables and metadata freed
   → Verifiable via /internal/shards (shows JIT memory usage)
```

### 10.8 Trigger Methods

```swift
// 1. File watch (automatic)
//    Runtime uses inotify to watch .dispatch files
//    Change detected → debounce → compile → swap

// 2. API endpoint (manual)
//    POST /internal/reload → immediate compile + swap

// 3. Signal (ops-friendly)
//    kill -HUP <pid> → same as API reload

// Status visible at:
//    GET /internal/health
//    {
//      "config_version": 3,
//      "compile_time_ms": 45,
//      "last_reload": "2026-03-21T10:30:00Z",
//      "pending_release": 0,
//      "pending_error": null
//    }
```

### 10.9 Connection Draining (Graceful Shutdown)

Separate from hot reload — this is for process shutdown/restart.

Borrowed from Envoy: probabilistic drain.

```
During shutdown:
  - New connections: accepted but responded with Connection: close header
  - Existing connections: drain probability increases over time
  - After drain period (configurable, e.g., 30s): force close remaining
  - Once all connections closed: process exits cleanly
```

---

## 11. Compiler Architecture

### 11.1 Pipeline

```
.dispatch source
    │
    ▼
  Lexer / Parser (hand-written recursive descent)
    │
    ▼
  Untyped AST
    │
    ▼
  Type Checker / Semantic Analysis
    │  - type inference
    │  - route conflict detection
    │  - domain value range checks
    │  - unreachable code detection
    │  - header spell check
    │  - optional access safety
    ▼
  Typed AST
    │
    ▼
  Inline Expansion (all func calls expanded)
    │
    ▼
  Dispatcher IR (DIR)  ◄── custom IR, backend-agnostic
    │
    ├── Optimization passes (on DIR)
    │   - dead code elimination
    │   - constant folding / propagation
    │   - state machine construction (I/O points → states)
    │   - instrumentation insertion (metrics, debug, access log)
    │
    ├── --dry-run: dump DIR and stop
    ├── --emit-dir: output DIR for inspection
    │
    ▼
  LLVM IR Generation (from DIR)
    │
    ▼
  LLVM ORC JIT
    │  - lazy compilation (only compile hit routes)
    │  - LLVM optimization passes
    ▼
  Native Function Pointers
    │
    ▼
  CompiledConfig { version, routes, handlers }
```

### 11.2 Dispatcher IR (DIR)

A lightweight, flat, typed IR between the AST and LLVM IR. Each route handler compiles to one DIR function — a linear sequence of blocks with explicit control flow.

#### 11.2.1 Why DIR

```
1. Optimization target
   AST is tree-shaped, hard to optimize
   LLVM IR is too low-level for domain-specific optimizations
   DIR is flat + typed — easy to analyze and transform

2. Backend independence
   DIR → LLVM IR  (current)
   DIR → Cranelift (future option)
   DIR → custom bytecode VM (future option)
   Switch backend without touching parser/type checker/optimizer

3. Debuggability
   --dry-run / --emit-dir dumps human-readable IR
   Users and developers can see exactly what the compiler produced
   Each instruction maps back to source location

4. Instrumentation insertion point
   Compiler inserts metrics/tracing/debug code at DIR level
   Optimization passes can then simplify (e.g., remove debug code in release mode)
```

#### 11.2.2 DIR Design

```
DIR concepts:
  Function   — one per route handler (after full inlining)
  Block      — a basic block: sequence of instructions, ends with a terminator
  Instruction — typed operation
  Terminator — branch, return, yield (I/O suspend point)
```

```
DIR types (mirrors language types):
  str, i32, i64, u32, u64, f64, bool
  ByteSize, Duration, Time, IP, CIDR, MediaType, StatusCode, Method
  Struct(name, fields)
  Optional(inner)
  Bytes
```

```
DIR instruction set:

  // --- Values ---
  %0 = const.str "Bearer "
  %1 = const.i32 401
  %2 = const.duration 5m
  %3 = const.bytesize 1mb

  // --- Request access ---
  %4 = req.header "Authorization"         // → Optional(str)
  %5 = req.param "id"                     // → str
  %6 = req.method                         // → Method
  %7 = req.path                           // → str
  %8 = req.remote_addr                    // → IP
  %9 = req.content_length                 // → ByteSize
  %10 = req.cookie "session_id"           // → Optional(str)

  // --- Request mutation ---
  req.set_header "X-User-ID", %val
  req.set_path %new_path

  // --- Operations ---
  %11 = str.has_prefix %4, %0             // → bool
  %12 = str.trim_prefix %4, %0            // → str
  %13 = str.interpolate [%6, "\n", %7]    // → str  (string interpolation)
  %14 = cmp.eq %6, Method.GET             // → bool
  %15 = cmp.gt %9, %3                     // → bool (ByteSize > ByteSize)
  %16 = time.now                          // → Time
  %17 = time.diff %16, %ts               // → Duration
  %18 = cmp.gt %17, %2                    // → bool (Duration > Duration)
  %19 = ip.in_cidr %8, 10.0.0.0/8        // → bool
  %20 = hash.hmac_sha256 %secret, %13    // → Bytes
  %21 = bytes.hex %20                     // → str
  %22 = call.jwt_decode %12, %secret      // → Optional(Claims)
  %23 = counter.incr %8, 1m              // → i32

  // --- Struct operations ---
  %24 = struct.field %user, "role"         // → str
  %25 = struct.create User { id: %s1, role: %s2 }  // → User
  %26 = body.parse Order                  // → Order (parse request body)
  %27 = array.len %items                  // → i32
  %28 = array.get %items, %idx            // → OrderItem

  // --- Control flow (terminators) ---
  br %cond, block_then, block_else         // conditional branch
  jmp block_next                           // unconditional jump
  ret.status 401                           // return HTTP status
  ret.status 429, { Retry-After: %w }      // return with headers
  ret.proxy %upstream, { timeout: 10s }    // proxy to upstream

  // --- I/O (suspend points → state machine boundaries) ---
  %29 = yield.http_get "http://auth/verify", { Authorization: %token }
  %30 = yield.http_post "http://svc/create", { Body: %data }
  %31 = yield.proxy %upstream
  %32 = yield.extern "redisGet", %key

  // --- Debug/instrumentation (inserted by compiler) ---
  trace.func_enter "auth"
  trace.func_exit "auth", %result
  trace.io_start "http://auth/verify"
  trace.io_end "http://auth/verify", %status, %duration
  metric.histogram_record %route_hist, %duration
  metric.counter_incr %route_counter
  accesslog.write %entry
```

#### 11.2.3 Example: auth function after inlining into a route

Source:
```swift
get /users/:id { req in
    let user = auth(req, role: "user")
    rateLimit(req, limit: 100, window: 1m)
    req.X-User-ID = req.id
    proxy(users, timeout: 10s)
}
```

DIR output (after inlining + state machine construction):

```
func handle_get_users_id(req: Request) {
  entry:
    // -- inlined: auth --
    %token = req.header "Authorization"
    br %token.is_nil, block_reject_401, block_check_prefix

  block_check_prefix:
    %has_pfx = str.has_prefix %token, "Bearer "
    br %has_pfx, block_decode_jwt, block_reject_401

  block_decode_jwt:
    %raw = str.trim_prefix %token, "Bearer "
    %secret = const.str env("JWT_SECRET")
    %claims = call.jwt_decode %raw, %secret
    br %claims.is_nil, block_reject_401, block_check_exp

  block_check_exp:
    %now = time.now
    %expired = cmp.lt %claims.exp, %now
    br %expired, block_reject_401_expired, block_check_role

  block_check_role:
    %role = struct.field %claims, "role"
    %role_ok = cmp.eq %role, "user"
    br %role_ok, block_auth_ok, block_reject_403

  block_auth_ok:
    req.set_header "X-User-ID", %claims.sub
    req.set_header "X-User-Role", %claims.role

    // -- inlined: rateLimit --
    %count = counter.incr %req.remote_addr, 1m
    %over = cmp.gt %count, 100
    br %over, block_reject_429, block_proxy

  block_proxy:
    // -- remaining handler --
    %id = req.param "id"
    req.set_header "X-User-ID", %id
    ret.proxy upstream("users"), { timeout: 10s }

  block_reject_401:
    ret.status 401

  block_reject_401_expired:
    ret.status 401, "token expired"

  block_reject_403:
    ret.status 403

  block_reject_429:
    ret.status 429, { Retry-After: 1m }
}
```

```
State machine (derived from yield points):

  This example has no yield (jwt_decode and counter.incr are synchronous).
  If auth called an external HTTP service instead:

    %res = yield.http_get "http://auth/verify", { Authorization: %token }

  Then the function splits into two states:
    state 0: everything before yield → submit HTTP request → suspend
    state 1: everything after yield → process response → continue or proxy

  The yield instruction is where the DIR optimizer inserts the state boundary.
```

#### 11.2.4 DIR Optimization Passes

```
Pass 1: Dead Code Elimination
  Remove blocks that are never reached
  Remove unused variable assignments

Pass 2: Constant Folding
  env("KEY") → resolved at compile time if env is known
  const comparisons: cmp.eq "admin", "user" → false → simplify branch

Pass 3: State Machine Construction
  Find all yield instructions
  Split function into states at yield boundaries
  Generate state enum and dispatch switch

Pass 4: Instrumentation Insertion
  Insert trace.* instructions at func/IO boundaries
  Insert metric.* instructions at entry/exit
  Insert accesslog.write at function exit
  (All guarded by debug_flags check — branch predicted not-taken)

Pass 5: Guard Coalescing
  Multiple sequential guards on the same rejection code:
    br %a, reject_401, next1
    br %b, reject_401, next2
  → merge into single block with combined condition
```

#### 11.2.5 DIR Dump (--emit-dir)

```
$ dispatcher --emit-dir gateway.dispatch

=== handle_get_users_id ===
  params: [:id]
  io_points: 0 (all sync)
  states: 1
  blocks: 7
  instructions: 18

  entry:
    %0 = req.header "Authorization"             // line 42
    br %0.is_nil, block_reject_401, block_1     // line 42 (guard)
  block_1:
    %1 = str.has_prefix %0, "Bearer "           // line 43
    br %1, block_2, block_reject_401            // line 43 (guard)
  ...

=== handle_post_orders ===
  params: []
  io_points: 0
  states: 1
  blocks: 10
  instructions: 26
  ...
```

### 11.2 LLVM Dependency Management

LLVM is the heaviest dependency. Strategies:

| Strategy | Binary Size | Notes |
|----------|-------------|-------|
| Dynamic link `libLLVM.so` | Runtime < 1MB | System install required |
| Static link (X86 target + ORC only) | ~15MB | Custom LLVM build |
| Replace with Cranelift (via C API) | ~3MB | Lighter, faster compile, Rust FFI |
| Custom bytecode VM | 0 external deps | Lower performance ceiling |

Recommended: start with LLVM ORC JIT (dynamic link), consider Cranelift if LLVM proves too heavy.

### 11.3 C++ Integration API

```cpp
class DispatcherEngine {
public:
    // Compile .dispatch source, returns compile result (errors or config)
    CompileResult compile(std::string_view source);

    // Atomically load new config to all shards (RCU swap)
    void reload(CompiledConfig* config);

    // Current config version
    uint64_t current_version() const;

    // Register C++ functions callable from .dispatch
    template<typename Func>
    void register_native(std::string_view name, Func&& fn);
};

// Example: register a Redis call
engine.register_native("redis.get",
    [&redis](Str key) -> AsyncResult<Str> {
        return redis.get(key);
    });
```

### 11.4 LSP Server

A Language Server Protocol implementation for editor support and LLM integration:

- Autocompletion (directives, header names, upstream references)
- Diagnostics (type errors, route conflicts)
- Hover (type information)
- Go-to-definition (upstream, middleware references)

---

## 12. Dependencies

### 12.1 Full Dependency List

| Dependency | Type | Size | Purpose |
|------------|------|------|---------|
| Linux kernel syscalls | OS | 0 | io_uring (6.0+) or epoll (3.9+), mmap, clone3, timerfd, signalfd, inotify |
| musl libc | Static link | ~600KB | C standard library (needed by LLVM) |
| Custom HTTP parser | Built-in | ~500-800 lines C++ | HTTP/1.1 request parsing (SIMD-accelerated, zero-copy, strict mode) |
| LLVM ORC JIT | Dynamic link | ~50MB .so | JIT compilation |
| Custom DNS client | Built-in | ~200 lines C++ | Async DNS resolution (UDP over io_uring, A/AAAA queries) |
| OpenSSL / BoringSSL (Phase 2) | Dynamic link | ~3MB .so | TLS for inbound + outbound |

Total external dependencies: **4-6**

### 12.2 What We Don't Depend On

```
❌ C++ standard library (std::)
❌ Boost
❌ libevent / libev / libuv
❌ liburing (own wrapper)
❌ protobuf / grpc
❌ OpenSSL (Phase 1)
❌ tcmalloc / jemalloc (own allocators)
❌ any package manager
```

---

## 13. External Service Integration

### 13.1 Overview

The DSL's HTTP calls and proxy operations depend on a set of runtime capabilities to work reliably in production. These are transparent to the user — the language syntax stays simple while the runtime handles complexity.

```swift
// User writes:
let res = get http://auth-service/verify {
    Authorization: token
    Timeout: 3s
}

// Runtime handles:
//  1. DNS: resolve "auth-service" → IP(s)
//  2. Connection pool: reuse existing connection or create new one
//  3. TLS: if https://, perform TLS handshake (or reuse session)
//  4. Timeout: enforce connect + read + total timeouts
//  5. Retry/breaker: if using a service declaration
```

### 13.2 DNS Resolution

Async DNS resolution, never blocking the event loop.

```swift
// Optional global DNS configuration
dns {
    servers: ["8.8.8.8", "8.8.4.4"]    // default: system resolv.conf
    cache: 30s                          // minimum cache TTL
    timeout: 2s                         // DNS query timeout
}
```

```
Implementation:
  - Custom async DNS client (~200 lines C++, UDP over io_uring)
  - Per-shard DNS cache (share-nothing)
  - Cache key: hostname → [IP], expires at max(DNS TTL, configured minimum)
  - On cache miss: async resolve, queue pending requests for same hostname
  - On failure: use last known good result (with warning log)
  - Supports /etc/hosts and /etc/resolv.conf
  - Phase 1: A/AAAA records (sufficient for Kubernetes)
  - Phase 2: SRV records (for service discovery)
```

### 13.3 Connection Pooling

Per-shard, per-host:port connection pools for both proxy and HTTP calls.

```swift
// Global connection pool configuration (optional, sensible defaults)
connections {
    maxIdlePerHost: 16       // keep-alive idle connections per host
    maxConnsPerHost: 128     // max total connections per host (active + idle)
    idleTimeout: 90s         // close idle connections after this
}

// Upstream-specific override
let users = upstream {
    "10.0.0.1:8080"
    maxConns: 256
    idleTimeout: 60s
}
```

```
Implementation:
  struct ConnPool {                         // per-shard
      host_port → {
          idle: ListNode[Connection]        // intrusive list of idle conns
          active_count: u32
          max_idle: u16
          max_total: u16
      }
  }

  HTTP call / proxy flow:
    1. Lookup pool[host:port]
    2. Idle connection available → reuse (remove from idle list)
    3. No idle + under max → create new connection
    4. At max → queue request, wait for a connection to become available
    5. Response complete + keep-alive → return to idle list
    6. Response complete + close → destroy connection
    7. Timer wheel checks idle connections → close if expired
```

### 13.4 TLS for Outbound Connections

```swift
// Automatic: http:// = plaintext, https:// = TLS
let res = get https://api.stripe.com/charges {
    Authorization: "Bearer \(env("STRIPE_KEY"))"
}
// TLS handshake automatic, server cert verified against system CA store

// mTLS (client certificate):
let res = get https://internal-service/data {
    ClientCert: tls(cert: env("CLIENT_CERT"), key: env("CLIENT_KEY"))
}

// Skip verification (dev only — compiler emits warning):
let res = get https://dev-service/test {
    TLSVerify: false     // ⚠️ warning: insecure TLS
}
```

```
Implementation:
  - OpenSSL / BoringSSL via BIO_s_mem (non-blocking, io_uring compatible)
  - Per-shard TLS session cache (per host:port)
    → Reuse TLS session on reconnect: saves ~5ms per connection
  - Connection pool preserves TLS state
    → Idle keep-alive connections retain TLS session
  - Phase 1: No TLS outbound (use http:// to internal services)
  - Phase 2: TLS outbound with session caching
  - Phase 3: mTLS outbound
```

### 13.5 Timeout Control

```swift
// Simple: one timeout covers everything
let res = get http://auth-service/verify {
    Timeout: 3s
}

// Fine-grained:
let res = get http://slow-service/compute {
    ConnectTimeout: 1s       // TCP handshake
    ReadTimeout: 30s         // waiting for response data
    Timeout: 60s             // total wall-clock limit
}

// Upstream-level defaults:
let users = upstream {
    "10.0.0.1:8080"
    connectTimeout: 1s
    readTimeout: 10s
    sendTimeout: 5s
}

// Override at proxy call:
proxy(users, timeout: 30s)   // overrides total timeout
```

```
Implementation:
  - Total timeout: single timer wheel entry per request
  - Connect timeout: timer set when io_uring connect SQE submitted
  - Read timeout: timer reset on each data chunk received
  - Send timeout: timer reset on each data chunk sent
  - All timers go through the per-shard timer wheel (O(1) operations)
  - Timeout hit → cancel io_uring SQE → return 504 (proxy) or error (HTTP call)
```

### 13.6 Service Declarations

For external services called frequently, `service` provides retry, circuit breaker, and connection pool configuration in one place — more than a raw HTTP call, less than an `upstream`.

```swift
// Declare a service with resilience policies
let authService = service("http://auth-service") {
    timeout: 3s
    retry: .on([502, 503], count: 2, backoff: 100ms)
    breaker: .consecutive(failures: 5, recover: 30s)
    maxConns: 64
}

// Use like a typed HTTP client — same syntax, but with retry + breaker:
func auth(_ req: Request, role: string) -> User {
    guard let token = req.authorization else { return 401 }

    let res = authService.get("/verify") {
        Authorization: token
    }
    // If auth-service returns 502 → auto-retry up to 2 times
    // If 5 consecutive failures → circuit opens → instant 503 for 30s

    guard res.status == 200 else { return 401 }
    return res.body(User)
}

// Raw HTTP calls (no retry, no breaker) still work:
let res = get http://one-off-service/check { }
```

```
Comparison:

  Raw HTTP call         service declaration       upstream
  ──────────────        ────────────────────      ────────
  No retry              Retry ✅                  Retry ✅
  No breaker            Breaker ✅                Breaker ✅
  No health check       No health check           Health check ✅
  Single target         Single base URL           Multiple targets
  For: one-off calls    For: frequently called    For: proxy destinations
                        external services
```

### 13.7 Service Discovery

```swift
// Phase 1: Static addresses + DNS A records (Kubernetes-ready)
let users = upstream { "user-service.default.svc:8080" }
// DNS resolves to Pod IPs, refreshed per DNS TTL

// Phase 2: DNS SRV records
let users = upstream {
    discover: .dnsSrv("_http._tcp.user-service")
    balance: .roundRobin
}
// SRV records provide port + weight + priority

// Phase 3: External discovery via extern FFI
extern func consulDiscover(service: string) -> [string]

// Periodic refresh using background timer (runtime feature):
let users = upstream {
    discover: .extern(consulDiscover, service: "user-service", every: 10s)
    balance: .leastConn
}
```

### 13.8 Implementation Phases

```
                        Phase 1         Phase 2         Phase 3
────────────────────────────────────────────────────────────────
DNS resolution          ✅ A/AAAA       SRV records     -
DNS caching             ✅ per-shard    -               -
Connection pooling      ✅ per-shard    -               -
Connection pool config  ✅ defaults     user-tunable    -
Timeout (total)         ✅              -               -
Timeout (fine-grained)  -              ✅               -
HTTP outbound           ✅ http://      -               -
HTTPS outbound          -              ✅ OpenSSL       mTLS
TLS session caching     -              ✅               -
Retry (upstream)        ✅              -               -
Retry (service)         -              ✅               -
Breaker (upstream)      ✅              -               -
Breaker (service)       -              ✅               -
Service discovery       DNS A           DNS SRV         extern (Consul, etc.)
fire (async calls)      ✅              -               -
```

---

## 14. Observability and Debugging

### 13.1 Design Principles

- Zero external dependencies (no OpenTelemetry SDK, no Prometheus client library)
- Zero overhead on normal requests (~10ns: one branch + histogram update)
- Debug mode is per-request, triggered by header, no global perf impact
- All metrics per-shard, lock-free, aggregated on read
- Compiler auto-instruments I/O points — user doesn't write tracing code

### 13.2 Language-Level Logging

```swift
// Structured logging — built into the language
func auth(_ req: Request, role: string) -> User {
    guard let token = req.authorization else {
        log.warn("missing auth token", {
            path: req.path
            addr: req.remoteAddr
        })
        return 401
    }

    let res = get http://auth/verify {
        Authorization: token
    }

    guard res.status == 200 else {
        log.error("auth service rejected", {
            status: res.status
            path: req.path
            upstream: "auth-service"
        })
        return 401
    }

    log.debug("auth ok", { userId: claims.sub, role: claims.role })
    return User(id: claims.sub, role: claims.role)
}

// Log levels: debug, info, warn, error
// Level switchable at runtime without reload
// debug level can be filtered by path pattern
```

### 13.3 Request-Level Tracing

```swift
// Automatic: compiler inserts trace points at every func call and I/O point
// No user code needed for basic tracing

// Manual spans for fine-grained tracing:
func processOrder(_ req: Request) {
    trace.span("validate_order") {
        let order = req.body(Order)
        guard !order.items.isEmpty else { return 400 }
    }

    trace.span("check_inventory") {
        let res = post http://inventory/check {
            Body: order.items
        }
        guard res.status == 200 else { return 503 }
    }

    proxy(orders)
}
```

### 13.4 Per-Request Debug Mode

```
Triggered by X-Debug header — only affects that single request:

  X-Debug: true     → verbose log output for this request
  X-Debug: trace    → full trace with timing in response header
  X-Debug: timing   → per-step timing breakdown

Curl example:
  curl -H "X-Debug: trace" http://gateway/users/123

Response includes X-Debug-Trace header:
  X-Debug-Trace: [
    {"fn": "requestId",  "duration_us": 2,    "result": "pass"},
    {"fn": "cors",       "duration_us": 1,    "result": "pass"},
    {"fn": "auth",       "duration_us": 1234, "result": "pass",
     "io": [{"url": "http://auth/verify", "status": 200, "duration_us": 1200}]},
    {"fn": "rateLimit",  "duration_us": 1,    "result": "pass", "count": 42},
    {"fn": "proxy",      "duration_us": 5678, "upstream": "users",
     "target": "10.0.0.1:8080", "status": 200}
  ]

Implementation:
  Compiler inserts at every func/I/O point:
    if (c->debug_flags) emit_trace_event(c, ...);
  Normal requests: one branch, predicted not-taken, ~0 cost
  Debug requests: ~1-5μs overhead to record full trace
```

### 13.5 Runtime Metrics

Per-shard, lock-free counters and histograms. Aggregated when read.

```
Request metrics:
  requests_total                 // by method + path pattern + status code
  requests_active                // currently processing
  request_duration_us            // latency histogram (log-scale buckets)

Connection metrics:
  connections_total              // total accepted
  connections_active             // currently active
  connections_idle               // keep-alive waiting

Upstream metrics:
  upstream_requests_total        // by upstream name + target + status
  upstream_duration_us           // upstream response latency histogram
  upstream_connections_active    // active connections to upstream
  upstream_health                // per-target health status

Memory metrics:
  memory_arena_used              // scratch arena current usage
  memory_slices_used             // slice pool usage
  memory_slices_free             // slice pool available
  memory_connections_used        // connection slab usage

I/O backend metrics (io_uring):
  iouring_sq_pending             // SQ entries pending submission
  iouring_cq_pending             // CQ entries pending consumption
  iouring_buf_available          // provided buffer ring available count
```

Latency histogram uses log-scale buckets:
```
  [<100μs, <500μs, <1ms, <5ms, <10ms, <50ms, <100ms,
   <500ms, <1s, <5s, ≥5s]

  Per-shard array, each bucket is a u64 counter
  Recording: one subtract + clz to find bucket, one atomic increment
  ~3ns per request
```

### 13.6 Access Log

High-performance access log — separate from application logging:

```
Architecture:
  Each shard writes to a per-shard ring buffer (64KB)
  Background thread (not on any shard) batch-flushes to file/stdout
  Zero allocation, zero syscall on the request path

Format: one JSON line per request
  {
    "ts": "2026-03-20T15:30:00.123Z",
    "method": "GET",
    "path": "/users/123",
    "status": 200,
    "duration_us": 1234,
    "req_size": 256,
    "resp_size": 1024,
    "addr": "10.0.0.5",
    "upstream": "users",
    "upstream_duration_us": 1100,
    "request_id": "abc-123",
    "shard": 3
  }
```

User-configurable in .dispatch:

```swift
accessLog {
    format: .json                  // .json | .text | .clf
    output: .stdout                // .stdout | .file("/var/log/access.log")
    fields: [.method, .path, .status, .duration, .addr, .requestId]
    filter: { $0.duration > 100ms || $0.status >= 400 }  // only slow/error
}
```

### 13.7 Internal Endpoints

Built-in HTTP endpoints for runtime inspection. Can be on a separate port or protected by middleware.

```swift
// Option A: separate internal port (recommended)
listen :9090, internal: true

// Option B: on main port with access control
route {
    group /internal { req in
        ipAllow(req, cidrs: [10.0.0.0/8])

        get /internal/metrics => metrics()    // Prometheus format
        get /internal/health  => health()     // JSON health status
        get /internal/shards  => shards()     // per-shard state
        get /internal/upstreams => upstreams() // upstream status
        get /internal/routes  => routes()     // route table + hit counts
        post /internal/log-level { req in     // dynamic log level
            setLogLevel(req.body(LogLevelConfig))
            return 200
        }
    }
}
```

Endpoint details:

```
GET /internal/metrics — Prometheus exposition format
  # TYPE requests_total counter
  requests_total{method="GET",path="/users/:id",status="200"} 12345
  # TYPE request_duration_us histogram
  request_duration_us_bucket{le="100"} 1000
  request_duration_us_bucket{le="500"} 4500

GET /internal/health — JSON
  {
    "status": "healthy",
    "uptime": "3d 4h 22m",
    "version": 3,                    // config version (reload count)
    "shards": 8,
    "connections": { "active": 1234, "idle": 98000 },
    "upstreams": {
      "users": { "healthy": 2, "unhealthy": 0 },
      "orders": { "healthy": 1, "unhealthy": 0 }
    }
  }

GET /internal/shards — per-shard breakdown
  {
    "shards": [
      {
        "id": 0, "core": 0,
        "connections": { "active": 150, "idle": 12300 },
        "memory": {
          "arena": "12KB / 64KB",
          "slices": "45 / 1024",
          "connections": "12450 / 16384",
          "provided_buffers": "1980 / 2048"
        },
        "requests": { "total": 1234567, "active": 150 },
        "iouring": { "sq_pending": 3, "cq_pending": 0 }
      }
    ]
  }

GET /internal/upstreams — upstream target status
  {
    "users": {
      "targets": [
        {"addr": "10.0.0.1:8080", "healthy": true, "active_conns": 12,
         "weight": 3, "requests": 500000, "avg_latency_us": 1200},
        {"addr": "10.0.0.2:8080", "healthy": true, "active_conns": 4,
         "weight": 1, "requests": 170000, "avg_latency_us": 1350}
      ]
    }
  }

GET /internal/routes — route table with stats
  {
    "routes": [
      {"method": "GET", "pattern": "/health", "hits": 50000, "avg_us": 5},
      {"method": "GET", "pattern": "/users/:id", "hits": 800000, "avg_us": 1500},
      {"method": "POST", "pattern": "/orders", "hits": 200000, "avg_us": 3200}
    ],
    "config_version": 3
  }

POST /internal/log-level — dynamic log level change
  { "level": "debug" }                              // global
  { "level": "debug", "path": "/orders/*" }          // per-path filter
  No reload needed. Takes effect immediately on all shards.
```

### 13.8 Dry-Run Mode

Compile-time validation without starting the server:

```
$ dispatcher --dry-run gateway.dispatch

✓ Parsed 3 files
✓ Type check passed
✓ Routes:
    GET  /health        → inline (no I/O)
    GET  /users/:id     → 2 I/O points (auth verify, proxy)
    POST /users         → 2 I/O points (auth verify, proxy)
    POST /orders        → 2 I/O points (auth verify, proxy)
✓ State machines:
    handle_get_users_id:  3 states
    handle_post_users:    3 states
    handle_post_orders:   3 states
✓ No route conflicts
✓ All upstreams referenced
✓ Estimated memory per shard: ~28MB
```

### 13.9 Compiler Auto-Instrumentation

The compiler automatically inserts instrumentation at key points. Users don't write tracing code for standard metrics.

```
What the compiler inserts for every route handler:

  Entry:
    record start timestamp
    increment requests_active
    check debug_flags (one branch, predicted not-taken)

  Each inlined func call (debug mode only):
    record func entry/exit timestamp
    record result (pass/reject)

  Each I/O point (always):
    record upstream URL, start/end timestamp, response status

  Exit:
    compute duration
    update latency histogram (one array increment, ~3ns)
    decrement requests_active
    increment requests_total[method][path][status]
    write access log entry to ring buffer

Overhead on normal (non-debug) requests:
    ~10ns total (timestamp + histogram + counter increments)
```

---

## 15. Implementation Phases

### Phase 1: Minimal Runtime (target: working proxy)

```
├── I/O backend: io_uring (~300 lines) + epoll (~250 lines) + abstraction (~100 lines)
├── Per-core shard threads (~200 lines)
├── Memory allocators: Arena + SlabPool + SlicePool (~500 lines)
├── Custom HTTP parser with SIMD (~500-800 lines)
├── Connection management + state machine (~1000 lines)
├── Static route table (hardcoded, no JIT yet) (~500 lines)
├── Proxy to upstream (~1000 lines)
├── Basic config loading (~500 lines)
└── Total: ~5050 lines C++

Deliverable: can accept HTTP requests, proxy to upstream, return responses.
```

### Phase 2: Language + JIT

```
├── Lexer + Parser for .dispatch (~2000 lines)
├── Type checker (~1500 lines)
├── LLVM IR codegen (~2000 lines)
├── JIT loading + hot reload (~1000 lines)
├── State machine transform (async → states) (~1000 lines)
├── Radix trie route compiler (~500 lines)
└── Total: ~8000 lines

Deliverable: .dispatch files compiled and loaded, hot reload works.
```

### Phase 3: Production Hardening

```
├── TLS termination (OpenSSL integration)
├── HTTP/2 support
├── Graceful shutdown + connection draining
├── Backpressure (watermark buffers)
├── Observability: access log (ring buffer + background flush)
├── Observability: per-shard metrics + Prometheus endpoint
├── Observability: per-request debug tracing (X-Debug header)
├── Observability: /internal/* inspection endpoints
├── Observability: compiler auto-instrumentation
├── Observability: dynamic log level control
├── TLS termination (OpenSSL integration)
├── HTTP/2 support
├── Graceful shutdown + connection draining
├── Backpressure (watermark buffers)
├── Resource limits (max connections, max request size)
├── NUMA awareness
└── LSP server for editor integration
```

---

## 16. Design Decisions Log

| # | Decision | Alternatives Considered | Rationale |
|---|----------|------------------------|-----------|
| 1 | Custom runtime, not Seastar | Seastar | Only ~30% of Seastar is relevant; too complex; future/promise overhead unnecessary |
| 2 | io_uring, not DPDK | DPDK | DPDK gives <1% improvement for L7 proxy; requires dedicated cores (expensive in cloud); io_uring + DPDK is broken in Seastar (issue #1890) |
| 3 | io_uring preferred, epoll fallback | epoll-only | io_uring: completion-based, fewer syscalls, zero-copy; epoll: wider kernel support (3.9+); both behind compile-time abstraction |
| 4 | Kernel TCP, not userspace | Seastar native stack | Kernel TCP has mature congestion control (BBR, CUBIC); standard tooling works (tcpdump, ss); containers just work |
| 5 | `-nostdlib++` | Full C++ stdlib | Full memory control; faster compilation; no hidden allocations |
| 6 | LLVM ORC JIT | Cranelift, custom VM | Same C++ toolchain; zero FFI overhead; lazy compilation; mature |
| 7 | State machines, not coroutines | Stackful/stackless coroutines | Zero overhead; no stack allocation per connection; 48 bytes vs 2-8KB per connection |
| 8 | Share-nothing shards | Shared-memory with locks | Eliminates all race conditions; proven by Seastar/Envoy/nginx |
| 9 | SO_REUSEPORT accept | Single-thread accept + dispatch | Zero cross-shard communication; kernel handles distribution |
| 10 | Arena + Slab + Slice | malloc/free, tcmalloc | Zero allocation on hot path; predictable latency; zero fragmentation |
| 11 | Timer wheel | Priority queue, timerfd per connection | O(1) all operations; single timerfd per shard |
| 12 | RCU for config reload | Locks, fork new process | Lock-free hot path; no process restart; atomic swap |
| 13 | Per-shard rate limit partitioning | Global lock, distributed counter | Zero communication; ±N accuracy acceptable for rate limiting |
| 14 | Radix trie routing | Linear scan (Envoy), hash map | O(path length) lookup; supports prefix matching and parameters; JIT-compilable to jumps |
| 15 | Unified completion (proactor) API | Pure reactor or pure proactor | Upper layers written once; io_uring is native proactor; epoll backend wraps reactor as proactor adapter; no code duplication |
| 16 | Swift-inspired syntax | Go, TypeScript, Rust, custom DSL | guard + named params + trailing closures fit gateway patterns perfectly; high LLM accuracy; clean aesthetics; no commas in blocks |
| 17 | All functions compile-time inlined | Runtime function calls | Each route = one flat state machine; zero call overhead; compiler complexity low (just expand + find I/O points) |
| 18 | HTTP as native objects | Strings for everything | Status codes, methods, headers, CIDR, MediaType are typed; compiler catches typos, range errors, type mismatches |
| 19 | No async/await | Explicit async | User writes sequential code; compiler auto-detects I/O points and generates state machines; simpler mental model |
| 20 | Built-in security/encoding primitives | External dependencies | md5, sha256, hmac, jwt, base64, aes cover 95% of gateway needs; zero external deps |
| 21 | Custom IR (DIR) between AST and LLVM IR | Direct AST → LLVM IR | Enables domain-specific optimizations, backend independence (swap LLVM for Cranelift), debuggability (--emit-dir), and clean instrumentation insertion point |
| 22 | Security as language-level code, not runtime features | Built-in WAF/DDoS modules | WAF rules, bot detection, flood protection are all expressible in the DSL; no hardcoded security logic in runtime; users can customize everything |
| 23 | Response middleware via parameter signature | Separate `onResponse` keyword | `func f(req, resp)` = response middleware; compiler auto-detects; no new syntax needed |
| 24 | Connection-level protection in `listen` config | Runtime-only config | headerTimeout, maxConnsPerIP, minRecvRate, strictParsing are declared in .dispatch files; compile-time validated; visible alongside routing logic |
