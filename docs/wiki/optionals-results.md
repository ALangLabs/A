# Optionals, results, and errors

Verified against `examples/optionals-result/` and `tests/integration/cases/`.

## Optionals (`T?`)

```python
missing = None
present = Some(41)

print(missing ?? 0)        # coalesce: 0
print(present! + 1)        # unwrap (panics on None)
print(present == Some(41)) # equality works on Some/None

maybeUser?.name            # safe navigation: None propagates
```

- `None` is the empty optional; `Some(v)` wraps a value.
- `??` returns the right side when the left is `None`.
- `!` postfix unwraps (runtime panic on `None`).
- `?.` accesses a field/method only when non-`None`.
- `expr?` (try) propagates `Err`/`None`-style failures upward — used in
  `tests/integration/cases/result.a` as `q = divide(x, y)?`.

## Results (`Ok` / `Err`)

`Result` helpers live in the prelude (`import std:prelude`):

```python
import std:prelude

fn divide(a, b):
    if b == 0:
        return prelude.Err("division by zero")
    return prelude.Ok(a / b)

r = divide(10, 2)
if prelude.is_ok(r):
    print(prelude.unwrap(r))
else:
    print(prelude.err_value(r))

prelude.unwrap_or(divide(10, 0), -1)   # fallback
```

## Assertions and panics

- `assert(cond)`, `assert_eq(a, b)` — builtins; `panic(msg)` aborts.
- Type errors, arity errors, unwrap-of-`None`, and index-out-of-range are
  compile-time or runtime errors reported through the diagnostic system
  (`a check` / stderr + exit code).

See also [builtins](builtins.md) and [testing](testing.md).
