# Ownership Model

CI Ownership uses deterministic ownership instead of tracing garbage collection.

The model is intentionally strict:

- one owner destroys an object,
- plain assignment copies,
- `move` transfers ownership,
- `borrow` aliases without owning,
- ownership cycles are rejected.

The goal is to avoid both C/C++-style lifetime bugs and GC-style unpredictable pauses.

## Execution and threads

The current VM is single-threaded. A `VMContext` and its objects must be used
from one execution thread at a time; concurrent calls into one context, sharing
its mutable VM state between threads, and cross-thread access to its objects are
unsupported. This is an explicit current limitation, not a guarantee provided
by `move` or `borrow`. A future multithreading design would need to specify
thread confinement or explicit synchronized transfer separately.

## Owners

An owner is responsible for destroying a value.

These places can own values:

- local variables,
- class fields,
- array elements,
- function return values while being transferred.

```ci
box: Box = new Box;
```

Here, `box` owns the object.

## Destruction

Owned locals are destroyed at the end of their scope.

```ci
{
    box: Box = new Box;
} // box is destroyed
```

If an owned object contains owned fields, those fields are destroyed too.

If an owned array contains owned object elements, those elements are destroyed too.

## Definite initialization

Owned references have a compile-time state: initialized, uninitialized, moved,
or unavailable on some control-flow paths. A value may be read or moved only
when it is definitely initialized.

```ci
box: Box;
if (condition) {
    box = new Box;
}
box.print(); // rejected: box is not initialized on every path
```

Initializing the value in both the `if` and `else` branches is valid. Loops are
treated as possibly executing zero times unless their condition is proven
constant-false (in which case the body has no effect), so initialization only
inside a loop is not sufficient for a use after the loop.

Branches that definitely return are excluded from the state joined after an
`if`. A non-void function must return a value on every reachable path; CI does
not silently manufacture zero or null for a missing return.

## Borrowed returns

A borrowed return must explicitly name the parameter and optional subobject
path that provide its lifetime. The function may return that parameter itself,
a field beneath it, or an array element:

```ci
fun identity(box: Box): borrow(box) Box {
    return box;
}

fun itemOf(holder: Holder): borrow(holder.item) Box {
    return holder.item;
}

fun first(items: Box[]): borrow(items[]) Box {
    return items[0];
}
```

Paths use dot-separated field names; `[]` marks an arbitrary array element
(indices are intentionally treated as potentially aliasing). Methods may use
`borrow(this.field)` to tie the result to a receiver subobject. The compiler
checks every return path and propagates the caller-side owner and path through
calls, including forwarding calls. Thus, replacing a field or array element
while the returned borrow remains live is rejected. Assigning a borrowed result
to an owning variable makes a copy rather than taking ownership. Returning a
borrow from a global, or using a borrowed return without naming its source, is
still rejected.

## Copy by default

Plain assignment does not share ownership.

```ci
a: Box = new Box;
b: Box = a;
```

`b` receives a copy. Both variables own different objects.

This rule makes accidental cycles and double-free bugs much harder to create.

## Move when sharing would otherwise be needed

`move` transfers the object instead of copying it.

```ci
a: Box = new Box;
b: Box = move a;
```

Now `b` owns the object and `a` cannot be used until reinitialized.

Temporary sharing can be modeled by moving ownership through a function and returning it.

```ci
fun useAndReturn(box: Box): Box {
    // box is owned here
    return box;
}

fun setup(): int {
    owner: Box = new Box;
    owner = useAndReturn(move owner);
    return 1;
}
```

## Borrow instead of share

Borrowing gives temporary access without ownership.

```ci
owner: Box = new Box;
view: borrow Box = owner;
```

The owner must outlive the borrow.

Borrowed values do not destroy anything.

Reference parameters are treated as borrows from the caller. You may create
local borrowed views from them and use those views during the call, but the
compiler rejects storing a parameter-derived borrow in a borrowed object field
or global borrowed variable. The caller's owner lifetime is not yet represented
in function signatures, so those escapes cannot currently be proven safe.

### Borrows of nested fields

The compiler tracks the complete storage path behind a local borrow:

```ci
view: borrow Leaf = tree.left.leaf;
```

While `view` is live, replacing either `tree.left.leaf`, `tree.left`, or the
whole `tree` is rejected because each operation would destroy the borrowed
value. Replacing an unrelated sibling such as `tree.right` remains valid.

Member paths are currently limited to eight nested fields. Exceeding that
limit is a compile error rather than falling back to an unsafe borrow.

Borrowing a reference from an array element also keeps the owning array (and
any containing field path) as its origin. The compiler rejects letting that
borrow outlive the array owner, storing it in a borrowed field that may outlive
the array, or replacing the borrowed element while the borrow is live. Literal
integer indices are distinguished, so a borrow of `items[0]` does not prevent
replacing `items[1]`. Dynamic indices may alias any element and stay
conservative. A borrowed-return annotation using `items[]` is also a wildcard,
so it may alias any element at the call site.

The compiler also checks loop back-edges. If a borrowed local can retain an
owner across iterations, replacing or moving that owner in the loop is rejected
when it could invalidate the borrow. A mutation before the borrow assignment
in the source order can still be unsafe on the second iteration. Conditions
proven constant-false do not propagate body borrow state after the loop; other
loop iteration counts remain conservative. Branches that assign different
borrow origins merge to an unknown owner, which conservatively blocks mutation
until the borrow is reassigned.

## Why borrowed values are restricted

The current prototype treats borrowed object references as read-only for member mutation:

```ci
view: borrow Box = owner;
// view.value = 10; // rejected
```

This avoids a common confusion: a non-owning reference should not be able to mutate ownership state or replace owned children through an alias.

The owner can still mutate:

```ci
owner.value = 10;
```

## Class fields

Class fields own by default.

```ci
class Holder {
    item: Box;
};
```

Assigning to `item` makes the holder own that value. Replacing the field destroys the previous owned value.

Borrowed fields are non-owning:

```ci
class Holder {
    item: Box;
    selected: borrow Box;
};
```

`selected` can point at `item` or another owner, but it will not destroy what it points at.

## Cycles

Owned cycles are rejected.

```ci
class Node {
    next: Node; // rejected
};
```

Use `borrow` where ownership should not flow.

```ci
class Node {
    next: borrow Node;
};
```

This rule is the main alternative to reference counting. Instead of counting references and then needing cycle detection, the compiler prevents owned cycles from being expressed.

## Comparison with GC

A tracing GC allows arbitrary graphs:

```text
A -> B -> C -> A
```

Then the runtime periodically scans reachable objects and frees unreachable ones.

CI Ownership chooses a stricter tree-like ownership shape:

```text
owner
  child
    grandchild
borrowed links point across the tree but do not own
```

That restriction lets cleanup happen at known program points.

## Comparison with reference counting

Reference counting allows shared ownership, but each retain/release has runtime cost. Cycles also need special handling.

CI Ownership avoids shared ownership by default:

- copy with `=`,
- transfer with `move`,
- alias with `borrow`.

This can be faster and more predictable, but it requires stricter code.

## Practical rule of thumb

Use:

- `=` when you want a separate copy,
- `move` when ownership should transfer,
- `borrow` when you only need temporary access,
- owned fields for parent-to-child relationships,
- borrowed fields for back-links, selected/current pointers, caches, and peer references.
