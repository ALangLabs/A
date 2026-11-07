# Ownership, move, and borrow

A has flow-sensitive ownership checks (verified in
`tests/integration/cases/ownership_*.a`, `borrow_*.a`, and
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
