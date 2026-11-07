# Testing

## `a test`

`a test file.a` compiles the program in test mode: `main` discovers and runs
every top-level `fn test_*()` through the runtime `a_test_main` harness, each
in a forked child so one failure doesn't stop the suite.

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

## `std:testing` / `std:test`

Property-style helpers exist in the stdlib (`testing_seed`,
`for_all_int`, `for_all_float`, `for_all_bool`, `for_all_string`,
`for_all_elem`) for randomized/property tests.

## Golden tests

The repo's own suite lives in `tests/integration/cases/` — each `*.a` file may
have `.out`/`.err`/`.exit`/`.in`/`.flags` siblings; `flags` = `check` runs
`a check`, `test` runs `a test`. Run with
`tests/integration/run_tests.sh build/a`.
