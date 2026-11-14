# Ownership, move, and borrow

A has flow-sensitive ownership checks (verified in
`tests/integration/cases/ownership_*.astra`, `borrow_*.astra`, and
`examples/advanced/`).

## `move(x)`

Transfers ownership of a value:

```python
xs = [1, 2, 3]
ys = move(xs)
# using xs again before reassigning it is a compile error
```

## `borrow(x)` / `borrow_mut(x)`

```python
shared = borrow(xs)        # read-only borrow
double = borrow_mut(xs)    # exclusive borrow — conflicts with other live borrows
```

- A shared borrow conflicts with a live `borrow_mut` of the same value and
  vice versa; the checker reports the conflict at compile time.
- Borrowed values must not be moved or structurally mutated while borrowed.

## Views (slices)

Unit-step slices create tracked views:

```python
view = items[1..4]     # borrows `items`
items.append(x)        # ERROR: changes length while view is alive
view[0] = 9            # OK: element writes through views are allowed
```

Structural mutators that are rejected while a view is live: `append`,
`extend`, `insert`, `pop`, `remove`, `clear`, `remove_at`. Moving the lender
is also an error.

## Scope behavior

Borrows and moves are tracked per scope and merged conservatively across
branches and loops; a value moved on any path counts as moved afterwards.

## Runtime memory management

A reclaims memory automatically with a **conservative mark-sweep
garbage collector** (`compiler/astra/runtime/gc.c`). It is enabled by
default; set `A_GC=0` to disable, `A_GC_STATS=1` for per-collection
stats, `A_GC_DEBUG=1` for stage markers.

- Heap objects (lists, dicts, sets, strings over 256 bytes, bytes,
  structs, closures, iterators, …) are tracked; unreachable ones are
  freed. Small strings (≤256 B) and boxed ints live in 2 MiB bump-arena
  chunks; whole chunks are released after they remain unreferenced for
  two collections.
- Roots are found conservatively: the C stack (including emitted-code
  frame slots), registers captured at collection time, runtime-internal
  stacks (format/equality/ordering guards, span/trace registries),
  builtin singletons, and the `__a_gc_roots` table the compiler emits
  for top-level globals.
- Collections are stop-the-world and generational-free: a collection
  runs when allocations since the last one exceed ~4× the live set
  (minimum 8 MB). Pointer-looking words in dead stack slots or inside
  string/double data can retain garbage (Boehm-style false positives) —
  bounded and safe, never a use-after-free.
- Plain C buffers that are not part of a heap A object are only unregistered
  by sweep; their owner releases them with `a_gc_free()`. Forcibly freeing
  these after a delayed mark was tested and removed because ASan proved
  that pointers can survive in locations the conservative scanner misses.
- Python interop drops owned references and exported buffers when those A
  wrappers become unreachable. `Py_Finalize()` is called through an
  atexit handler at process exit; a bounded amount of CPython module
  state remains internal to that interpreter by design.
- TinyCC-built runtimes with `A_RUNTIME_CC=off` are executable evidence for
  the host path. ARM, AArch64, and RISC-V executable verdicts still require
  matching target C-library sysroots.
- `copy()` is a deep copy with cycle preservation; cyclic structures
  are collected normally once unreachable.
- Parallel regions (`A_PAR`) and GC worker threads are coordinated so
  collections never run while pool workers allocate.
