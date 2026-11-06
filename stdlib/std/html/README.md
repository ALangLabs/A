# std.html

Port of CPython 3.14 `Lib/html/__init__.py` (+ `entities.py` data).

- `html.escape(s, quote)` — full parity (`& < > " '`).
- `html.unescape(s)` — named entities (`&amp;`), legacy semicolon-less
  forms (`&amp`, `&notit` -> `¬it`), decimal `&#65;` and hex `&#x42;`
  references; invalid refs left literal, like CPython.
- `html/entities.a` — complete generated `html5` entity table (2231 entries,
  regenerate from `/usr/lib/python3.14/html/entities.py`).

Not ported: `html.parser` (needs the `re` engine), `html.entities.html5`
dict is exposed as `html5()` function.
