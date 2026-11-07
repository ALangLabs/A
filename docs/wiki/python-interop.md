# Python interop

A can call into CPython at run time — libpython is `dlopen`'d, so there is no
link-time dependency (verified in `compiler/astra/backend/python/python.c` and
`examples/python-interop/`).

## Importing Python modules

```python
import python:numpy
from python:json import loads, dumps
```

`python:<pkg>` imports resolve through the `py` native module (`py.import_module`,
`py.try_import`, `py.getattr`, `py.call_method`, … — see
[builtins](builtins.md)).

## Calling and attributes

Python objects behave like opaque values: attribute access, indexing, method
calls, `len`, iteration, truthiness, and calls all bridge to the interpreter.
Errors surface as A panics with the Python error text.

## Zero-copy buffers

`py.buffer(obj)` exposes a Python buffer-protocol export (NumPy arrays,
`array.array`, `memoryview`, `bytearray`, …) as a `T_PYBUF` view — element
access without copying. `py.buffer_info` reports layout.

## Examples

- `examples/python-interop/json_pipeline.a` — JSON pipeline through Python.
- `examples/python-interop/opaque_objects.a` — opaque object handling.
- `examples/python-interop/optional/` — `numpy_array.a`, `py_buffer.a`,
  `torch_tensor.a` (require the corresponding Python packages installed).

Requires a Python installation discoverable at run time; behavior differs
between backends, so validate with `a run`.
