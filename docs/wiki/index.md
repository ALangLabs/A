# A language reference

Documentation for the **A programming language** itself: syntax, types,
declarations, statements, expressions, modules, ownership, builtins, and the
standard library. The pages are checked against the implementation
(`compiler/astra/frontend/`), the examples in `examples/`, and the golden
tests in `tests/integration/cases/`, but they can lag: when a page disagrees
with `a run`/`a check`, the compiler is right — a behavior change should
update the page it contradicts, and drift should be filed as a bug.

## Pages

- [Overview](overview.md) — what A is, headline features, a first program.
- [Syntax and types](syntax-and-types.md) — comments, literals, indentation, operators, and every built-in type.
- [Grammar and syntax reference](grammar.md) — concise EBNF-style summary of lexical elements, literals, expressions, and control flow.
- [Declarations](declarations.md) — `fn`, `class`/`metaclass`, `enum`, `trait`, `impl`, `type`, generics, `comptime`, `where`.
- [Statements and expressions](statements-and-expressions.md) — assignments, control flow, `match`, comprehensions, calls, indexing/slicing, lambdas, string interpolation, `defer`.
- [Optionals, results, and errors](optionals-results.md) — `None`/`Some`, `??`, `!`, `?.`, `try`, `Ok`/`Err`, `assert`.
- [Modules and imports](modules.md) — quoted imports, `std:` modules, native modules, `python:` interop.
- [Ownership, move, and borrow](ownership.md) — `move`, `borrow`, `borrow_mut`, and slice views.
- [Builtins and native modules](builtins.md) — core builtin functions and the native module table (`math`, `fs`, `json`, `net`, …).
- [Standard library](stdlib.md) — `stdlib/` packages and `import std:` usage.
- [Package manager](apm.md) — `apm`, the registry/lockfile/manifest system for third-party packages.
- [Python interop](python-interop.md) — calling Python from A, buffers, examples.
- [Testing](testing.md) — `a test`, `fn test_*()`, `assert`/`assert_eq`, `std:testing`.
- [CLI and tooling](cli.md) — `a run/build/check/test/fmt/doc/ir/repl` and useful flags.
- [Examples](examples.md) — runnable programs by category.
- [Benchmarks](benchmarks.md) — timed workloads and cross-language equivalents.
