# A Language Examples

Runnable examples for the current A language implementation. Each file can be executed with:

```sh
./build/a run examples/<category>/<file>.a
```

## Categories

| Category | Description |
|---|---|
| [basics](basics/) | Hello world, literals, variables, operators, strings, I/O |
| [control-flow](control-flow/) | `if`/`elif`/`else`, loops, `match`, ranges, comprehensions, `defer` |
| [functions](functions/) | Functions, lambdas, closures, pipelines, recursion |
| [data-types](data-types/) | Arrays, tuples, maps, sets, slicing, destructuring |
| [classes](classes/) | `struct`/`class`, fields, methods, `self`, inheritance/metaclass syntax |
| [enums-match](enums-match/) | Enums (integer and algebraic), pattern matching, guards, variants |
| [traits-generics](traits-generics/) | Traits, `impl`, `where`, type aliases, generics, `comptime` params |
| [optionals-result](optionals-result/) | `T?`, `Some`/`None`, `??`, `!`, `?.`, `Result`/`Ok`/`Err`, `?` propagation |
| [modules](modules/) | `import`, `from ... import`, aliases, native modules, prelude |
| [stdlib](stdlib/) | `math`, `fs`, `json`, `time`, `http`/`net`, `log`/`metrics`/`trace`, `testing` |
| [python-interop](python-interop/) | `import python:<pkg>` CPython interop (json example runnable; numpy/torch under `optional/`) |
| [advanced](advanced/) | Unsafe/arena, SIMD/GPU stubs, `comptime`, `a test` assertions |

## Notes

- Examples target the **current** dynamic/Python-style runtime semantics with optional annotations.
- Some stdlib packages (`json`, `binary`, `testing`, `log`, `metrics`, `trace`, `net`, `http`) are native-backed and primarily available through the interpreter/KGEN execution engine path.
- `net.tls_connect` / HTTPS TLS requires an OpenSSL-enabled build.
- `python:<pkg>` imports embed the CPython found at build time (`find_package(Python3)`); packages must be installed for *that* interpreter. See [python-interop/optional/README.md](python-interop/optional/README.md).
