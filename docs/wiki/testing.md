# Testing

## `a test`

`a test [path]` runs tests in a file or discovers tests across a directory
(like pytest). With no `path` it scans the current directory; with a
directory it scans recursively; with a `.a` file it runs that file only.

```sh
a test                      # discover and run tests under .
a test stdlib/std/math      # scan a directory
a test path/to/file.a       # run one file
a test --filter json        # run matching test_* names across discovered files
```

**Discovery (scan mode):** A recursively scans for `*.a` files that define a
top-level `fn test_*()` and runs each as an independent test program,
aggregating results. Hidden directories and common build/vendor directories
(`.git`, `build`, `build-*`, `cmake-build*`, `.apm`, `node_modules`,
`third_party`) are skipped. Files are run in sorted path order.

**Execution model:** each test file is compiled and run in test mode: `main`
discovers and runs every top-level `fn test_*()` through the runtime
`a_test_main` harness, each in a forked child so one failure doesn't stop the
suite. `fn main` is never invoked, the file's top-level statements run once
up front, and `test_*` functions must declare no parameters (a parameterized
`test_*` is a compile error). Because test mode executes top-level statements,
scan mode only runs files that actually contain `fn test_*()` definitions.

```python
fn test_assertions():
    assert(true)
    assert_eq(2 + 2, 4)
    assert_eq("A".lower(), "a")
```

See `examples/advanced/testing.astra`.

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
