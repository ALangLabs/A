# std.bytes

A analogue of Python's `bytes`-type helpers (there is no `bytes` module in
CPython; this package provides the conversions byte-oriented ports need).

| A | Python |
|---|---|
| `bytes.chr(cp)` | `chr(cp)` -> 1-char string, `None` off-range |
| `bytes.ord(c)` | `ord(c)` -> code point, `None` off-char |
| `bytes.utf8_encode(s)` | `s.encode("utf-8")` -> `[i64]` |
| `bytes.utf8_decode(b)` | `bytes(b).decode("utf-8")` -> `String?` |
| `bytes.b2s(b)` / `s2b(s)` | Latin-1 byte<->string view |

Bytes are `[i64]` arrays of values 0-255 (same convention as `binary.decode`).
`chr`/`ord` ride the `binary` codec's char tag so all Unicode scalar values
work. Verified against CPython behaviour in `pybytes_basic`.
