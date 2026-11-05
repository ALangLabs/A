# Optional Python-interop examples

These programs `import python:<pkg>` and require the package installed in the
**Python interpreter embedded in `a`** (the one CMake's `find_package(Python3)`
selected at build time — check `a`'s linked `libpython`, e.g. `ldd build/a | grep python`).

- `numpy_array.a` — needs `numpy` (`python3 -m pip install numpy` for that interpreter)
- `torch_tensor.a` — needs `torch`

They live one directory deeper so `examples/*/*.a` sweeps skip them.
