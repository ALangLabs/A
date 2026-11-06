# std.graphlib

Port of CPython 3.14 `Lib/graphlib.py`: `TopologicalSorter` for
topologically sorting a graph of hashable nodes.

## Usage

```a
import graphlib

# From a {node: [predecessors]} map (Python: TopologicalSorter(graph)):
ts = graphlib.TopologicalSorter_from({
    "a": ["b", "c"],
    "b": ["c"],
    "c": [],
})
print(ts.static_order())   # e.g. ["c", "b", "a"]

# Or build incrementally (Python: TopologicalSorter()):
ts2 = graphlib.TopologicalSorter_new()
ts2.add("x", ["y", "z"])
ts2.add_node("z")
ts2.prepare()
ready = ts2.get_ready()    # nodes with no predecessors
ts2.done(ready)            # or ts2.done_one(node)
while ts2.is_active():
    group = ts2.get_ready()
    ts2.done(group)
```

## API coverage vs Python

| Python | A | Notes |
| --- | --- | --- |
| `TopologicalSorter(graph=None)` | `TopologicalSorter_new()`, `TopologicalSorter_from(graph)` | no optional ctor args in A; `graph` is a `{node: [predecessors]}` map |
| `add(node, *predecessors)` | `add(node, predecessors)` + `add_node(node)` | predecessors is an array |
| `prepare()` | `prepare()` | panics on cycles (see below) |
| `get_ready()` | `get_ready()` | returns `[T]` instead of tuple |
| `done(*nodes)` | `done(nodes)` + `done_one(node)` | nodes is an array |
| `is_active()` | `is_active()` | `__bool__` has no A equivalent |
| `static_order()` | `static_order()` | returns `[T]` instead of generator |
| `CycleError` | `panic("nodes are in a cycle")` | exceptions -> runtime errors |
| `_NodeInfo`, `_NODE_OUT`, `_NODE_DONE` | same names | internal bookkeeping, exported for parity |

## Semantic deviations

- **Exceptions -> panic.** `prepare()` on a cyclic graph, `get_ready()`/
  `done()`/`is_active()` before `prepare()`, and `done()` on unknown or
  already-processed nodes are runtime errors (`panic`). Unlike CPython,
  there is no way to catch `CycleError` and keep draining `get_ready()`.
- **Iterables -> arrays.** `get_ready()` returns an array; `static_order()`
  returns a fully materialized topological ordering.
- **Varargs -> arrays.** `add`/`done` take arrays of nodes.
- Node keys must be usable as A map keys (strings, ints, etc.).
