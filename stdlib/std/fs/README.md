# `std.fs`

Filesystem operations for reading, writing, listing, and managing files/directories.

Native-backed module for common file and directory tasks. Paths and permissions follow the host environment and runtime implementation.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Filesystem operations.
- **Source path:** `stdlib/std/fs/`

## Import

```a
import fs
```

## What It Provides

- **Functions:** `read_file`, `write_file`, `append_file`, `file_exists`, `list_dir`, `remove_file`, `create_dir`, `remove_dir`, `rename`, `current_dir`

## API Reference (Practical)

- `read_file(path)`, `write_file(path, text)`, `append_file(path, text)`
- `file_exists(path)`, `list_dir(path)`
- `remove_file(path)`, `create_dir(path)`, `remove_dir(path)`
- `rename(old_path, new_path)`, `current_dir()`

## Examples

```a
import fs

path = "example_notes.txt"
fs.write_file(path, "hello")
print(fs.file_exists(path))
print(fs.read_file(path))
fs.append_file(path, " world")
print(fs.read_file(path))
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (interpreter/KGEN vs LLVM paths) and host environment.
- Filesystem errors are surfaced by the runtime (behavior may differ from Python exceptions/APIs).
- Prefer workspace-local paths for tests and examples.

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
