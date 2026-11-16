# Python interop

A can call into CPython at run time — libpython is `dlopen`'d, so there is no
link-time dependency (verified in `src/astra/runtime/python.c` and
`examples/python-interop/`).

## Importing Python modules

```a
import python:numpy
from python:json import loads, dumps
```

`python:<pkg>` imports resolve through the `py` native module (`py.import_module`,
`py.try_import`, `py.getattr`, `py.call_method`, … — see
[builtins](builtins.md)). This works on every codegen path: the x86-64
SysV `emitAsm` emitter and the per-arch `emitNative` emitters (all
non-x86-64 `--target` builds, plus Win64/MS-ABI for `x86_64` on Windows)
both bind `PyModule`/`PyFunc` values and lower calls through the runtime
`py_call` builtin. There is no C emitter — `A_CODEGEN=c` is rejected and
an emitter decline is a hard error, so Python interop is uniform across
targets.

## Calling and attributes

Python objects behave like opaque values: attribute access, indexing, method
calls, `len`, iteration, truthiness, and calls all bridge to the interpreter.
Errors surface as A panics (or `Err(...)` results from `py.try_*`) with the
Python exception type and message, e.g. `ValueError: math domain error`.

## Value conversion

When a Python value crosses into A (`py_to_aval`), exact builtin types
convert eagerly: `None`/`bool`/`int`/`float`/`str`/`bytes`/`list`/`tuple`/
`dict`/`set`/`frozenset` become the matching A values. **Subclasses of
those types stay opaque Python objects** — `collections.Counter`,
`OrderedDict`, `defaultdict`, `UserList`, namedtuples and structseqs keep
their methods and attributes instead of being drained into plain A
containers (a `Counter` iterates as a dict, so draining would silently
turn it into a list of keys). The same applies in reverse: an A list or
dict passed to Python becomes a real `list`/`dict`.

Other iterables convert to an A `list` only when small (<= 65536 elements),
are not buffer-protocol objects, and are not their own iterator;
larger or streaming objects (generators, tensors, datasets) stay opaque so
`for` consumes them lazily and they can be passed back into Python calls.

Equality is symmetric: `pyobj == <A value>` converts the A side and uses
CPython rich comparison, so `json.loads(s) == [1, 2]` and
`counter == {"a": 2}` behave like Python `==`.

One limitation: A functions/closures are not Python callables — passing one
to CPython (e.g. as a `defaultdict` factory) falls back to its repr string
and fails with `TypeError: first argument must be callable or None`. Pass a
real Python callable instead (`py.getattr(builtins, "int")`).

## `std:py` helpers

`import std:py` exposes the runtime bridge directly: `py.buffer`,
`py.buffer_info`, `py.import_module`/`py.try_import`, `py.getattr`/
`py.try_getattr` (3-arg form returns a default), `py.setattr`/`py.delattr`/
`py.hasattr`, `py.getitem`/`py.setitem`/`py.delitem`, `py.call`/`py.try_call`
(callable + args list + optional kwargs dict), `py.call_method`/
`py.try_call_method` (object + method name + args + kwargs),
`py.is_pyobject`, `py.str`/`py.repr`/`py.len`/`py.contains`/`py.truthy`,
`py.type_name`. The `try_*` variants return `Ok(v)`/`Err("ExcType: msg")`
instead of panicking. Keyword arguments also work through the
`python:mod.member(...)` call syntax (`re.compile("a+", flags=...)`).

## Zero-copy buffers

`py.buffer(obj)` exposes a Python buffer-protocol export (NumPy arrays,
`array.array`, `memoryview`, `bytearray`, …) as a `T_PYBUF` view — element
access without copying. `py.buffer_info` reports layout.

## Examples

- `examples/python-interop/json_pipeline.astra` — JSON pipeline through Python.
- `examples/python-interop/opaque_objects.astra` — opaque object handling.
- `examples/python-interop/optional/` — `numpy_array.astra`, `py_buffer.astra`,
  `torch_tensor.astra` (require the corresponding Python packages installed).

Requires a Python installation discoverable at run time; behavior can
differ across target OSes (e.g. no interop on Windows), so validate with
`a run`.
