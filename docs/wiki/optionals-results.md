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
- `?.` accesses a *field* of an optional class instance only when non-`None`
  (methods are not supported); the result is itself optional (`T?`) and
  `None` short-circuits the chain.
- `expr?` (try) propagates failures upward — `Err` for builtin `Result`
  values and `None` for optionals (the enclosing function must then return
  a matching optional/`Result` type). Used in
  `tests/integration/cases/result.astra` as `q = divide(x, y)?`.

## Results (`Ok` / `Err`)

There are two distinct "result" mechanisms:

1. **Builtin `Result`** — `Ok(v)` and `Err(e)` are builtins (no import) that
   produce a `Result` value; `expr?` unwraps the `Ok` payload or propagates
   the `Err`. There is no `Result` type annotation — leave return types
   unannotated or let them infer:

   ```python
   fn divide(a, b):
       if b == 0:
           return Err("division by zero")
       return Ok(a / b)

   fn run():
       q = divide(10, 2)?    # q == 5; Err propagates out of run()
       return Ok(q + 1)

   print(run())              # Ok(6)
   ```
2. **`std:prelude` `Result` class** — a plain struct with `ok`/`value`/
   `error` fields plus helpers (`is_ok`, `unwrap`, `err_value`,
   `unwrap_or`, …). It does **not** work with `?` and its `Ok`/`Err` are
   ordinary functions, unrelated to the builtins:

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

A runtime panic (uncaught `panic`/`assert`, index out of range, division
by zero, …) prints the message, an `--> file:line` pointer, and a
Python-style traceback of the live A frames:

```text
error: index out of range
  --> app.a:1
Traceback (most recent call last):
  File "app.a", line 7, in main
  File "app.a", line 1, in last
```

`<module>` frames show the line of the statement being executed (the
compiler stores it before each statement); function frames show the
function's declaration line, so the arrow points at the `fn` for a panic
raised inside a function. Frames come from `a_frame_push`/
`a_frame_pop` and the `a_stmt_line` slot in
`compiler/astra/runtime/core_util.c`.

See also [builtins](builtins.md) and [testing](testing.md).
