# `std.json`

JSON parsing, serialization, validation, and access to A's opaque `Json` values.

`json` is native-backed. Parse text into `Json`, inspect it with `kind`/`get`/`at`/accessors, and convert/serialize A values with `stringify`/`pretty`.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** JSON parsing, serialization, and opaque Json access.
- **Source path:** `stdlib/std/json/`

## Import

```a
import json
```

## What It Provides

- **Functions:** `parse`, `stringify`, `pretty`, `is_valid`, `get`, `at`, `len`, `has`, `keys`, `kind`, `as_str`, `as_int`, `as_float`, `as_bool`, `as_array`, `as_map`

## API Reference (Practical)

- `parse(text)` — parse JSON text into `Json` (runtime error on malformed input).
- `is_valid(text)` — check whether text is valid JSON.
- `stringify(value)`, `pretty(value)` — compact or indented JSON output.
- `kind(value)` — returns type tag such as `object`, `array`, `string`, `number`, `bool`, `null`.
- `has(obj, key)`, `get(obj, key)`, `at(arr, index)`, `len(value)`, `keys(obj)` — structural access.
- `as_str/as_int/as_float/as_bool/as_array/as_map` — typed extraction helpers (often `T?`/optional-style results).

## Examples

```a
import json

doc = json.parse("{\"name\": \"Ada\", \"scores\": [1, 2, 3], \"active\": true}")
print(json.kind(doc))
print(json.has(doc, "name"))
print(json.as_str((json.get(doc, "name"))!)!)
print(json.as_int((json.at((json.get(doc, "scores"))!, 1))!)!)
print(json.stringify(doc))
print(json.pretty([1, 2, 3]))
print(json.is_valid("[1, 2"))
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (Astra backend paths) and host environment.
- Native-backed/runtime behavior may differ by backend path; this module is primarily exercised through interpreter execution.
- Accessor results may be optional-like values; use the project’s established unwrapping patterns (as in existing tests/examples).

## Source Files

- `__init__.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
