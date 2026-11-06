# `std.debug`

Debug/source intrinsics and formatting helpers.

## Status
Partially ported for A. `ic(...)` is still a compiler builtin because
litprinter-style automatic source-label capture needs compiler metadata.
This module provides the primitives A code can use to build debug output.

## API
- `file()` / `__file__()` — current source file path.
- `line()` / `__line__()` — current 1-based line.
- `func()` / `__func__()` — current function name, or `<module>`.
- `context()` / `__debug_context__()` — `file:line in fn()` or module context.
- `time()` / `__debug_time__()` — local debug timestamp string `HH:MM:SS.mmm`.
- `repr(value)` / `__debug_repr__(value)` — debug string for a value.
- `__ic_format__(context)` — breadcrumb line.
- `__ic_format__(context, label, value)` — formatted inspection line.
- `format_breadcrumb`, `format_value`, `ic_value`, `ic_breadcrumb` — A helpers.
