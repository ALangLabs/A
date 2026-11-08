# A language reference

Documentation for the **A programming language** itself: syntax, types,
declarations, statements, expressions, modules, ownership, builtins, and the
standard library. Every claim on these pages is verified against the
implementation (`compiler/astra/frontend/`), the examples in `examples/`, and
the golden tests in `tests/integration/cases/`.

## Pages

- [Overview](overview.md) — what A is, headline features, a first program.
- [Syntax and types](syntax-and-types.md) — comments, literals, indentation, operators, and every built-in type.
- [Declarations](declarations.md) — `fn`, `class`/`metaclass`, `enum`, `trait`, `impl`, `type`, generics, `comptime`, `where`.
- [Statements and expressions](statements-and-expressions.md) — assignments, control flow, `match`, comprehensions, calls, indexing/slicing, lambdas, string interpolation, `defer`.
- [Optionals, results, and errors](optionals-results.md) — `None`/`Some`, `??`, `!`, `?.`, `try`, `Ok`/`Err`, `assert`.
- [Modules and imports](modules.md) — quoted imports, `std:` modules, native modules, `python:` interop.
- [Ownership, move, and borrow](ownership.md) — `move`, `borrow`, `borrow_mut`, and slice views.
- [Builtins and native modules](builtins.md) — core builtin functions and the native module table (`math`, `fs`, `json`, `net`, …).
- [Standard library](stdlib.md) — `stdlib/` packages and `import std:` usage.
- [Python interop](python-interop.md) — calling Python from A, buffers, examples.
- [Testing](testing.md) — `a test`, `fn test_*()`, `assert`/`assert_eq`, `std:testing`.
- [CLI and tooling](cli.md) — `a run/build/check/test/fmt/doc/ir/repl` and useful flags.
- [Examples](examples.md) — runnable programs by category.
- [Benchmarks](benchmarks.md) — timed workloads and cross-language equivalents.
