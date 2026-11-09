# Statements and expressions

Verified against `examples/` and `tests/integration/cases/`.

## Statements

```python
x = 42                 # binding (rebindable)
x: i64 = 7             # typed binding
x += 1                 # compound assignment on existing binding
name, age = pair       # destructuring (tuples/arrays); '_' discards
x = y = z = v          # chained assignment (references share, like Python)
x := expr              # walrus binding (statement level)
x = 1; y = 2           # ';' separates statements on one line
del x                  # unbind a name (sets it to None)
del xs[0]              # remove a list element / map key
del p.field            # clear a struct field (sets it to None)
```

Control flow (all use `:` + indented suite). Conditions are expressions
(typically boolean), e.g. `n < 2`, `x != None`, `flag and ready`, `true`.

```python
if cond:
    ...
elif other:
    ...
else:
    ...

while cond:
    ...

do:
    ...
while cond            # do-while: body runs first

for x in iterable:    # lists, strings, maps (keys), sets, ranges
    ...
for k, v in pairs:    # tuple unpacking: each item must be indexable
    ...

loop:                 # infinite loop; break/continue work
    ...

return expr           # return with value; a bare 'return' is only allowed
                      # in '-> void' functions
defer expr            # deferred call, runs LIFO at scope exit
break / continue
pass                  # empty body
import ... / from ... import ...
```

## Expressions

- Literals, identifiers, unary/binary ops (see [syntax](syntax-and-types.md)).
- Calls: `f(a, b)`, keyword args `Point(x=1, y=2)`, methods `s.strip()`,
  safe field access `maybe?.name` (see below).
- Indexing/slicing: `xs[i]` (negative indices allowed), `xs[1..4]`,
  `xs[0..10..2]`, `xs[-4..]`, `s[1..4]` — works on arrays, strings, and
  tuples by position and on maps by key; sets cannot be indexed (use `in`).
- Field access `p.x`, optional chaining `u?.address?.city` and method calls
  `p?.get()`: `?.` requires an optional receiver; on `None` the whole
  expression yields `None`, otherwise the field/method runs. Method calls
  need a plain-name receiver (`r = expr` first for complex receivers).
- `match` expression:

```python
return match code:
    200 => "OK"
    400..499 => "Client error"
    _ if n < 0 => "negative"   # guard
    _ => "Unknown"             # wildcard (required for exhaustiveness)
```

- Comprehensions: `[n * n for n in nums]`, `[n for n in nums if n % 2 == 0]`,
  set form `{n % 3 for n in 0..8}`, dict form `{k: v for k, v in pairs}` —
  note the dict form takes a single loop variable whose value feeds both
  key and value expressions.
- Lambdas `fn(x) => expr`; try operator `expr?` propagates errors (see
  [optionals/results](optionals-results.md)).
- Nested functions: inside a function body, `fn helper(x): return expr`
  desugars to a lambda binding — closures capture enclosing locals. Bodies
  with multiple statements or generics are rejected; write
  `helper = fn(x) => expr` or move the function to top level.
- String interpolation `"{expr}"` inside `"..."` strings; `f"..."` is an
  explicit prefix for the same thing; `'...'` strings never interpolate.
- Ternary `cond ? a : b` and inline-if `x if c else y`, coalescing `a ?? b`.
