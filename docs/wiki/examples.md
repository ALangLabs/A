# Examples

`examples/` contains runnable A programs grouped by feature category. There is
no per-directory README in this checkout; the categories themselves are the
organization.

## Categories

| Directory | Contents |
| --- | --- |
| `basics/` | `hello.a` (incl. `import std:prelude`), `literals_and_variables.a`, `operators.a`, `strings.a`. |
| `control-flow/` | `if_while_for.a`, `match_and_ranges.a`, `comprehensions_defer.a`. |
| `data-types/` | `arrays_tuples.a`, `maps_sets.a`. |
| `functions/` | `functions_and_lambdas.a`, `recursion.a`, `pipeline.a`. |
| `classes/` | `point.a`, `inheritance.a`. |
| `enums-match/` | `enums.a`, `match.a`. |
| `modules/` | `imports.a` + fixture `lib/mathx.a`. |
| `optionals-result/` | `optionals.a`, `result.a`. |
| `traits-generics/` | `traits.a`, `generics_and_aliases.a`. |
| `stdlib/` | `core_math_fs_time.a`, `json_binary.a`, `observability_net.a`. |
| `python-interop/` | `json_pipeline.a`, `opaque_objects.a`, `optional/` (numpy_array, py_buffer, torch_tensor — need Python libs). |
| `advanced/` | `testing.a`, `unsafe_arena_comptime.a`. |

## Running

```sh
./build/a run examples/basics/hello.a
./build/a check examples/classes/point.a
./build/a test examples/advanced/testing.a
```

Python-interop examples require a Python installation with the relevant
packages (the runtime `dlopen`s libpython at run time).
Related: [Stdlib](stdlib.md),
[overview](overview.md).
