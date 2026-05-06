# Pipe Expressions

Pipe expressions pass the value on the left into a function call on the right.
They are an analyzer-level rewrite: after type checking, pipe expressions become
ordinary inlined function-call expressions and do not introduce a separate
MIR/RIR opcode.

## Syntax

```rut
func id(x: i32) -> i32 => x

route GET "/users" {
    let code = 200 | id(_)
    if code == 200 { return 200 } else { return 500 }
}
```

The right-hand side must be a function call and must contain exactly one
placeholder. `_` and `_1` both mean "the whole left-hand value":

```rut
let code = 200 | id(_)
let same = 200 | id(_1)
```

Pipe stages can be chained:

```rut
let code = 200 | id(_) | id(_)
```

## Placeholder Slots

When the left-hand side is a tuple, numbered placeholders select tuple slots:

```rut
func second(a: i32, b: i32) -> i32 => b

route GET "/users" {
    let code = (200, 500) | second(_2, _1)
    if code == 200 { return 200 } else { return 500 }
}
```

Tuple slots can come from tuple literals, tuple locals, tuple-returning
functions, struct fields, variant payloads, and generic functions as long as the
shape is known to the analyzer.

Placeholder indexes are 1-based. `_1` through `_10` are accepted by the parser;
the analyzer rejects indexes that are not valid for the left-hand value.

## Nil And Error Flow

Pipe expressions preserve optional/error flow from the left-hand side:

```rut
func id(x: str) -> str => x

route GET "/users" {
    let host = req.header("Host") | id(_)
    let safe = or(host, "missing")
    return 200
}
```

If the left side is a runtime optional/error value, the generated expression only
calls the stage when the value is present. Missing values flow through as
`nil`/`error` with the stage's return shape, so downstream `or(...)` can handle
them.

For fallible runtime left-hand values, only `_` / `_1` is accepted. Numbered
tuple-slot placeholders such as `_2` are rejected because the value has to be
unwrapped before tuple slots can be safely projected.

Known `nil` and known `error(...)` left-hand values are folded at analysis time
and do not call the stage.

## Code Snippets

### Single Stage

Use `_` to pass the left-hand value into a function:

```rut
func normalize_status(code: i32) -> i32 => code

route GET "/single" {
    let code = 200 | normalize_status(_)
    if code == 200 { return 200 } else { return 500 }
}
```

### Chained Stages

Each stage receives the previous stage's result:

```rut
func clamp_success(code: i32) -> i32 {
    if code == 204 { 200 } else { code }
}

func ensure_ok(code: i32) -> i32 {
    if code == 200 { 200 } else { 500 }
}

route GET "/chain" {
    let code = 204 | clamp_success(_) | ensure_ok(_)
    if code == 200 { return 200 } else { return 500 }
}
```

### Placeholder In A Later Argument

The placeholder can appear in any argument position:

```rut
func choose(flag: bool, fallback: i32, value: i32) -> i32 {
    if flag { value } else { fallback }
}

route GET "/position" {
    let code = 200 | choose(true, 500, _)
    if code == 200 { return 200 } else { return 500 }
}
```

### Tuple Slot Reordering

Use `_1`, `_2`, and later slot numbers to pull values from a tuple:

```rut
func status_from_pair(primary: i32, fallback: i32) -> i32 {
    if primary == 200 { primary } else { fallback }
}

route GET "/tuple" {
    let pair = (200, 500)
    let code = pair | status_from_pair(_1, _2)
    if code == 200 { return 200 } else { return 500 }
}
```

You can also reorder tuple slots:

```rut
func second(a: i32, b: i32) -> i32 => b

route GET "/swap" {
    let code = (500, 200) | second(_1, _2)
    if code == 200 { return 200 } else { return 500 }
}
```

### Tuple-Returning Stage

Pipes can chain through tuple-returning functions:

```rut
func pair(code: i32) { (code, 500) }
func pick_primary(primary: i32, fallback: i32) -> i32 => primary

route GET "/tuple-stage" {
    let code = 200 | pair(_) | pick_primary(_1, _2)
    if code == 200 { return 200 } else { return 500 }
}
```

### Request Header Optional Flow

`req.header(...)` is optional. A pipe stage runs only when the value is present;
`or(...)` can provide a fallback:

```rut
func lower_host(host: str) -> str => host

route GET "/host" {
    let host = req.header("Host") | lower_host(_)
    let safe = or(host, "missing")
    return 200
}
```

### Error Flow

Error values propagate through the pipe until handled:

```rut
func maybe_code(ok: bool) -> i32 {
    if ok { 200 } else { error(.timeout) }
}

func keep(code: i32) -> i32 => code

route GET "/error" {
    let code = maybe_code(false) | keep(_)
    let safe = or(code, 500)
    if safe == 500 { return 500 } else { return 200 }
}
```

### Generic Stage

Generic functions can be used as pipe stages when the type shape can be inferred:

```rut
func id<T>(x: T) -> T => x

route GET "/generic" {
    let code = 200 | id(_)
    if code == 200 { return 200 } else { return 500 }
}
```

## Supported Today

- Function-call stages: `value | fn(_, other_arg)`.
- Placeholder position anywhere in the function argument list.
- Single-stage and chained pipes.
- Tuple slot placeholders `_1` ... `_10`.
- Tuple literals, tuple locals, tuple-returning functions, struct fields,
  variant payloads, and generic tuple/struct shapes.
- Optional/error propagation through runtime values.
- JIT execution after lowering through existing call-expression machinery.

## Current Gaps

The following are future work rather than current behavior:

- Method-call stages on the right-hand side.
- Placeholder-free stages. A pipe stage must consume the left-hand value
  explicitly.
- Multiple placeholders in a single stage.
- Tuple-slot placeholders for runtime optional/error left-hand values beyond
  `_` / `_1`.
- A dedicated MIR/RIR pipe representation; current lowering intentionally
  rewrites pipe into ordinary expressions before MIR.
