# A language overview

A is a statically typed, indentation-sensitive systems language with
Python-flavored syntax. It compiles to native machine code through the bundled
**Astra** backend (a TinyCC fork): `a build` produces a real executable, `a run`
compiles and executes it, `a check` type-checks without codegen.

## First program

```python
entry main():
    name = "World"
    print("Hello, {name}!")   # string interpolation with {expr}
    for i in 0..5:
        print(i)
```

Top-level statements run first; `entry name():` marks the function the
compiler invokes once after imports initialize. Save as `hello.a` and run
`./build/a run hello.a`.

## Headline features

- **Static typing with inference** — declare types when you want them
  (`x: i64 = 7`), omit them when inference suffices (`x = 7`). Integers default
  to `i64`, floats to `f64`.
- **Rich type system** — fixed-size and dynamic arrays, strings, tuples, maps,
  sets, ranges, functions, `T?` optionals, `Result<T, E>`, pointers, plus
  user-defined `class`/`enum`/`trait` types with generics.
- **Ownership** — `move(x)` transfers values; `borrow(x)`/`borrow_mut(x)`
  create checked references; slices (`a[1..4]`) are tracked views.
- **Modern syntax** — `match` expressions with guards and ranges,
  comprehensions, lambdas `fn(x) => x * 2`, string interpolation, `defer`,
  destructuring, `do … while`, `loop`.
- **Interop** — native modules (`math`, `fs`, `json`, `net`, `time`, …),
  `import std:` standard library, and `python:` modules for calling CPython.
- **Tooling** — `a fmt`, `a doc`, `a ir`, a REPL, `a-lsp` language server, and
  a VS Code extension.

## Getting started

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j
./build/a run examples/basics/hello.a
```

See [cli.md](cli.md) for all commands and [examples.md](examples.md) for
runnable programs by category.
