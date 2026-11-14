# Examples

`examples/` contains runnable A programs grouped by feature category. There is
no per-directory README in this checkout; the categories themselves are the
organization.

## Categories

| Directory | Contents |
| --- | --- |
| `basics/` | `hello.astra` (incl. `import std:prelude`), `literals_and_variables.astra`, `operators.astra`, `strings.astra`. |
| `control-flow/` | `if_while_for.astra`, `match_and_ranges.astra`, `comprehensions_defer.astra`. |
| `data-types/` | `arrays_tuples.astra`, `maps_sets.astra`. |
| `functions/` | `functions_and_lambdas.astra`, `recursion.astra`, `pipeline.astra`. |
| `classes/` | `point.astra`, `inheritance.astra`. |
| `enums-match/` | `enums.astra`, `match.astra`. |
| `modules/` | `imports.astra` + fixture `lib/mathx.astra`. |
| `optionals-result/` | `optionals.astra`, `result.astra`. |
| `traits-generics/` | `traits.astra`, `generics_and_aliases.astra`. |
| `stdlib/` | `core_math_fs_time.astra`, `json_binary.astra`, `ml_kernels.astra`, `observability_net.astra`. |
| `python-interop/` | `json_pipeline.astra`, `opaque_objects.astra`, `optional/` (numpy_array, py_buffer, torch_tensor — need Python libs). |
| `advanced/` | `testing.astra`, `implicit_parallel.astra`, `unsafe_arena_comptime.astra`. |
| `apm/` | `app.astra` + `apm.json` — demo of apm-managed packages; run with `apm/apm install -p examples/apm` then `apm/apm run -p examples/apm examples/apm/app.a`. |

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
