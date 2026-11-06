# `std.repl`

Standard library package `std.repl` for A.

## Status

- **Kind:** Native-backed module (registered in the A toolchain/runtime)
- **Backend:** Persistent REPL evaluation hook.
- **Source path:** `stdlib/std/repl/`

## Import

```a
import repl
```

## What It Provides

- **Functions:** `eval`

## The interactive shell

`main.a` is the self-hosted IPython-style shell loaded by `a repl` on a TTY
(piped/non-TTY `a repl` keeps the simple `>>>` line mode). It provides:

- `In [n]:` prompts with `...:` continuation lines, persistent `~/.a_history`
  (multiline-safe), up/down + PgUp/PgDn history, Ctrl-R reverse-i-search.
- Live syntax highlighting, ghost-text autosuggest (accept with End/Right),
  Tab completion with a selection menu (Shift-Tab cycles backwards),
  bracketed-paste multiline input.
- readline/emacs editing: Ctrl-A/E/K/U/W/Y, Ctrl-F/B, Ctrl-T transpose,
  Ctrl-_ undo, Alt-f/b word motion, Alt-d/Alt-Backspace/Ctrl-Delete word
  kills, Alt-u/l word case, Alt-. yank-last-arg, Alt+Enter newline,
  Ctrl-Left/Right word motion, Ctrl-L clear.
- IPython-style non-code lines (stored in history, do not consume `In [n]`):
  - `name?` / `name??` — inspect a keyword, builtin, module member, or
    session-defined name.
  - `%help`/`%magic`/`%quickref` — magic list; `%history [n]`; `%time <stmt>`;
    `%pwd`; `%ls [path]`; `%who`/`%whos`; `%env NAME`; `%save FILE`;
    `%load`/`%run FILE`; `%reset` (sessions cannot be reset).
  - `!cmd` — reported as unsupported (no shell builtin exists).

## Example

```a
import repl

# Inspect the available functions/constants in this build.
# Then call `repl.<member>(...)` using the API overview above.
```

## Usage Notes

- Native-backed/runtime behavior may vary by backend (Astra backend paths) and host environment.
- A’s current semantics are dynamic/Python-style in many paths; verify edge cases with tests when porting Python code.
- API names may be familiar, but behavior can differ where A lacks Python features (exceptions model differences, decorators, dynamic attributes/protocol hooks, regex engine, etc.).

## Source Files

- `complete.a`, `editor.a`, `highlight.a`, `history.a`, `main.a`

## Related Docs / Tests

- Main stdlib index: `stdlib/std/README.md`
- Extended reference: `docs/standard-library/README.md`
- Integration tests: `tests/integration/cases/stdlib/`
