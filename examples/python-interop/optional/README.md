# Optional Python-interop examples

These programs `import python:<pkg>` and require the package installed in a
Python environment `a` can discover at runtime — a project `.venv`/`venv`,
`VIRTUAL_ENV`, a `python3`/`python` on `PATH` (including uv-managed
interpreters under `~/.local/share/uv/python`), or an explicit
`A_PYTHON`/`A_PYTHON_LIB`. On the first `python:` import, `a` probes those
environments and prefers the one that actually contains the module, then adds
its `site-packages` to `sys.path`.

- `numpy_array.a` — needs `numpy` (`uv pip install numpy` in your env)
- `torch_tensor.a` — needs `torch`
- `py_buffer.a` — needs `numpy`; zero-copy buffer-protocol views via
  `py.buffer(x)` / `py_buffer(x)` over numpy arrays, `array.array`,
  `memoryview`, and `bytearray`

They live one directory deeper so `examples/*/*.a` sweeps skip them.
