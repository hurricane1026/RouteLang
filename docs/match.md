# Match

Rut supports `match` in routes and source functions.

## Route Match

```rut
route GET "/status" {
    let code = 200
    match code {
    case 200:
        return 200
    case _:
        return 404
    }
}
```

Route match patterns can be integer, boolean, string, and variant cases. Non-exhaustive scalar
matches need a wildcard arm.

```rut
route GET "/path" {
    let path = "/users"
    match path {
    case "/users":
        return 200
    case _:
        return 404
    }
}
```

Boolean matches are exhaustive when both `true` and `false` are present.

```rut
route GET "/enabled" {
    let enabled = true
    match enabled {
    case true:
        return 200
    case false:
        return 503
    }
}
```

## Variant Cases

Variant cases can be matched with `.case` when the subject type is already known, or with
`Variant.case` when the variant must be named explicitly.

```rut
variant Auth { ok, denied }

route GET "/auth" {
    let auth = Auth.ok
    match auth {
    case .ok:
        return 200
    case .denied:
        return 403
    }
}
```

Payload cases can bind the payload for the arm body and arm guard.

```rut
variant Result { ok(i32), err }

route GET "/result" {
    let result = Result.ok(200)
    match result {
    case .ok(code) if code == 200:
        return 200
    case _:
        return 500
    }
}
```

## Arm Guards

An arm can add `if <bool-expr>` after the pattern. When the pattern matches but the guard is false,
control falls through to the next arm.

```rut
route GET "/guarded" {
    let code = 200
    match code {
    case 200 if false:
        return 500
    case _:
        return 404
    }
}
```

Guarded route matches require a wildcard fallback unless the guard is produced by a supported nested
match expansion.

## Const Match

`match const` selects an arm during analysis. The subject and selected pattern must be compile-time
known.

```rut
route GET "/const" {
    let path = "/users"
    match const path {
    case "/users":
        return 200
    case _:
        return 404
    }
}
```

`match const` arms do not support arm guards.

## Nested Route Match

A route match arm can end in another route match. The compiler expands this into guarded outer arms,
so the runtime path remains a flat branch chain.

```rut
variant Auth { ok, denied }

route GET "/nested" {
    let auth = Auth.ok
    let path = "/users"
    match auth {
    case .ok:
        match path {
        case "/users":
            return 200
        case _:
            return 404
        }
    case .denied:
        return 403
    }
}
```

Blocks may put `let` statements before the final nested match.

```rut
variant Auth { ok, denied }

route GET "/nested-block" {
    let auth = Auth.ok
    match auth {
    case .ok: {
        let path = "/users"
        match path {
        case "/users":
            return 200
        case _:
            return 404
        }
    }
    case .denied:
        return 403
    }
}
```

Nested route match currently rejects inner arm guards and inner payload bindings. Prefix `guard`
statements before the nested match are also unsupported.

## Source Function Match

Source functions use `=>` arms and return expressions.

```rut
func status(path: str) -> i32 {
    match path {
    case "/users" => 200
    case _ => 404
    }
}
```

Function match supports arm guards and payload bindings.

```rut
variant Result { ok(i32), err }

func pick(result: Result) -> i32 {
    match result {
    case .ok(code) if code == 200 => code
    case _ => 404
    }
}
```

If any function match arm uses an `if` guard, include a wildcard arm so unmatched or
guard-false values have an explicit result. Unguarded boolean and variant matches may omit
the wildcard only when all cases are exhaustive.
