# Testing

## `a test`

`a test file.a` compiles the program in test mode: `main` discovers and runs
every top-level `fn test_*()` through the runtime `a_test_main` harness, each
in a forked child so one failure doesn't stop the suite. `fn main` is never
invoked, the file's top-level statements run once up front, and `test_*`
functions must declare no parameters (a parameterized `test_*` is a compile
error).

```python
fn test_assertions():
    assert(true)
    assert_eq(2 + 2, 4)
    assert_eq("A".lower(), "a")
```

See `examples/advanced/testing.a`.

## Assertions

- `assert(cond)` — panics on false.
- `assert_eq(a, b)` — panics unless `a == b`.
- `panic(msg)` — unconditional abort.

## Property helpers (`testing` module)

Randomized/property helpers exist as functions of the native `testing`
module: `import testing` (or `import std:testing`) then
`testing.seed(value)` to fix the generator, plus `testing.for_all_int`,
`testing.for_all_float`, `testing.for_all_bool`, `testing.for_all_string`,
and `testing.for_all_elem`. The module members are named `seed`,
`for_all_int`, …, so import that form: `from std:testing import seed`.
(`testing_seed`, `testing_for_all_int`, … also exist unqualified as
builtins that the type checker accepts with the same signatures — they are
not `std:testing` members, so `from std:testing import testing_seed` is an
unknown-member error.) **Caveat:** the default asm
backend does not implement them yet — calls type-check but fail at code
generation ("native module function not supported by asm backend").

## Golden tests

The repo's own suite lives in `tests/integration/cases/` — each `*.a` file may
have `.out`/`.err`/`.exit`/`.in`/`.flags` siblings; `flags` = `check` runs
`a check`, `test` runs `a test`. Run with
`tests/integration/run_tests.sh build/a`.
