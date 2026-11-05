# A Language Documentation

This documentation describes the **current implementation** of the A language and toolchain (v2, KGEN/LLVM-oriented backend). It is derived from the compiler, type checker, module loader, native module tables, and integration tests in this repository.

> **Note:** Some older marketing/README claims may not reflect current semantics. Where behavior differs, trust the compiler and tests. Current A is **Python-style dynamic at runtime** in many cases (plain assignment creates/rebinds mutable variables; many checks happen dynamically), with **optional annotations** and static/type-checker support for typed signatures, structs, traits, generics, and method resolution.

## Contents

- [Language Specification](language-spec/README.md) — full syntax, semantics, and expression/statement reference
- [Standard Library & Builtins API Reference](standard-library/README.md) — prelude, native modules, `std/*` packages, builtins, and method surfaces
- [CLI Reference](cli/README.md) — `a` commands, flags, and workflows
- [Compiler Internals](compiler-internals/README.md) — pipeline, KGEN/LLVM backend, module loading, and project layout

## Examples

Runnable examples live in [`examples/`](../examples/README.md):

```sh
# Build the toolchain
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_WARNINGS_AS_ERRORS=ON
cmake --build build -j

# Run an example
./build/a run examples/basics/hello.a
./build/a test examples/advanced/testing.a
./build/a check examples/classes/point.a
```

## Quick Syntax Overview

```a
# Comments start with #

fn add(a, b):
    return a + b

struct Point:
    x
    y

    fn sum(self):
        return self.x + self.y

p = Point(3, 4)
print(p.sum())          # 7
print("sum = {add(1,2)}")  # string interpolation

items = [1, 2, 3, 4]
doubled = [n * 2 for n in items]
print(items |> join("-"))  # pipeline
```
