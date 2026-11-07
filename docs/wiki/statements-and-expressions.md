# Statements and expressions

Verified against `examples/` and `tests/integration/cases/`.

## Statements

```python
x = 42                 # binding (rebindable)
x: i64 = 7             # typed binding
x += 1                 # compound assignment on existing binding
name, age = pair       # destructuring (tuples/arrays); '_' discards
```

Control flow (all use `:` + indented suite):

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

loop:                 # infinite loop; break/continue work
    ...

return expr           # return with optional value
defer expr            # deferred call, runs LIFO at scope exit
break / continue
pass                  # empty body
import ... / from ... import ...
```

## Expressions

- Literals, identifiers, unary/binary ops (see [syntax](syntax-and-types.md)).
- Calls: `f(a, b)`, keyword args `Point(x=1, y=2)`, methods `s.strip()`,
  safe calls `maybe?.name`.
- Indexing/slicing: `xs[i]`, `xs[1..4]`, `xs[0..10..2]`, `xs[-4..]`,
  `s[1..4]` — works on arrays, strings, tuples, maps, sets.
- Field access `p.x`, optional chaining `u?.address?.city`.
- `match` expression:

```python
return match code:
    200 => "OK"
    400..499 => "Client error"
    _ if n < 0 => "negative"   # guard
    _ => "Unknown"             # wildcard (required for exhaustiveness)
```

- Comprehensions: `[n * n for n in nums]`, `[n for n in nums if n % 2 == 0]`,
  set form `{n % 3 for n in 0..8}`.
- Lambdas `fn(x) => expr`; try operator `expr?` propagates errors (see
  [optionals/results](optionals-results.md)).
- String interpolation `"{expr}"` inside `"..."` strings.
- Ternary `cond ? a : b`, coalescing `a ?? b`.
