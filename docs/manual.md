# CI Ownership Language Manual

This manual describes the current CI Ownership prototype. It focuses on the syntax and ownership behavior implemented in this experimental copy.

The current VM is single-threaded. Do not execute one VM context concurrently
or access its objects from multiple threads.

## Program structure

CI scripts are made from global variables, class definitions, and functions.

```ci
counter: int = 0;

fun setup(): int {
    counter += 1;
    return counter;
}
```

Functions use `fun`, typed return values, and C-like blocks.

```ci
fun add(a, b: int): int {
    return a + b;
}
```

The standalone runner executes global code first, then calls `setup()` if the script defines it.

```powershell
ci test.ci
```

## Primitive types

Common primitive types include:

- `int`
- `float`
- `bool`
- `char`
- `short`

Primitive values are copied by value and do not participate in ownership cleanup.

```ci
x: int = 10;
y: int = x;
y += 5;
```

## Classes

Classes define named fields.

```ci
class Box {
    value: int;
};

class Holder {
    item: Box;
};
```

Class-typed fields own their assigned values unless declared `borrow`.

## Allocation with `new`

`new` creates an owned object.

```ci
box: Box = new Box;
box.value = 123;
```

The variable receiving the new object owns it. When that variable leaves scope, the object is destroyed.

## Scope cleanup

Owned locals are destroyed when their block exits.

```ci
fun setup(): int {
    {
        temp: Box = new Box;
        temp.value = 10;
    } // temp is destroyed here

    return 1;
}
```

Cleanup also happens across early exits such as `return`, `break`, and `continue`.

```ci
while (running) {
    item: Box = new Box;

    if (shouldSkip) {
        continue; // item is destroyed before continuing
    }

    if (shouldStop) {
        break; // item is destroyed before breaking
    }
}
```

## Copy assignment

For owned reference values, plain `=` copies.

```ci
original: Box = new Box;
original.value = 5;

copy: Box = original; // deep copy
copy.value = 9;
```

After this, `original` and `copy` are separate owned objects.

This rule intentionally avoids accidental shared ownership.

## Move assignment

Use `move` when ownership should transfer instead of copy.

```ci
first: Box = new Box;
second: Box = move first;
```

After the move, `first` is considered moved-from. Using it before assigning a new value is a compile error.

```ci
first: Box = new Box;
second: Box = move first;
// first.value = 1; // error: use of moved value
```

A moved-from local can be reinitialized.

```ci
first: Box = new Box;
second: Box = move first;
first = new Box; // first owns a new object again
```

## Returning owned values

Returning an owned object transfers it to the caller.

```ci
fun makeBox(): Box {
    box: Box = new Box;
    box.value = 10;
    return box;
}

fun setup(): int {
    result: Box = makeBox();
    return result.value;
}
```

The returned local is cleared in the callee so it is not destroyed before the caller receives it.

Owned temporaries produced while evaluating an expression are reclaimed when
that full expression finishes if the result was not adopted by an owner. This
applies to declarations, conditions, switch selectors/cases, returns, and
expression statements.

```ci
fun consume(box: Box; result: int): int {
    return result;
}

fun makeBox(): Box {
    return new Box;
}

fun setup(): int {
    answer: int = consume(makeBox(), 42); // temporary Box dies after initializer
    return answer;
}
```

The current implementation rejects reference-typed switch selectors and case
values; use primitive values there.

## Borrowed references

`borrow` creates a non-owning view.

```ci
owner: Box = new Box;
view: borrow Box = owner;
```

The borrowed variable does not destroy the object.

Borrowed values are read-only for member mutation in the current prototype.

```ci
owner: Box = new Box;
view: borrow Box = owner;

x: int = view.value; // ok
// view.value = 2;   // error
```

Mutate through the owner instead.

```ci
owner.value = 2;
```

## Borrowed fields

Fields can be declared as borrowed.

```ci
class Child {
    value: int;
};

class Parent {
    child: Child;
    selected: borrow Child;
};
```

A borrowed field aliases an existing owner and does not destroy the referenced object.

```ci
parent: Parent = new Parent;
child: Child = new Child;

parent.selected = child; // parent.selected borrows child
```

Borrowed fields cannot take ownership from `new`, function results, or `move`.

```ci
// parent.selected = new Child; // error
```

The compiler also rejects common cases where a borrowed field would outlive the local owner it points to.

## Preventing ownership cycles

Owned class fields cannot form direct or mutual cycles.

```ci
class Node {
    // next: Node; // error: owned cycle
};
```

Use `borrow` for back-links or peer links.

```ci
class Node {
    next: borrow Node;
};
```

For parent/child graphs:

```ci
class Parent {
    child: Child;
};

class Child {
    parent: borrow Parent;
};
```

## Arrays

Arrays are allocated with `new Type[count]`.

```ci
values: int[] = new int[4];
values[0] = 10;
values[1] = 20;
```

Reference arrays follow ownership rules for elements:

```ci
boxes: Box[] = new Box[2];

boxes[0] = new Box; // transfer from new

source: Box = new Box;
boxes[1] = source; // copy
boxes[1] = new Box; // replaces and destroys previous owned element
```

Borrowed arrays can read existing arrays, but cannot assign through the borrowed view.

```ci
ownerArray: Box[] = new Box[2];
view: borrow Box[] = ownerArray;

// view[0] = new Box; // error
```

## Parameters

Reference parameters are treated as borrowed by default. This means functions can inspect passed objects without taking ownership.

```ci
fun readBox(box: Box): int {
    return box.value;
}
```

This avoids accidental ownership transfer during normal calls.

Use `move` at the call site when ownership transfer is intended by an API that returns or stores ownership.

## Control flow

The compiler merges move and borrow state conservatively across branches.

```ci
value: Box = new Box;

if (condition) {
    other: Box = move value;
}

// value may have been moved, so using it here is rejected
```

Reassign the value to reinitialize it.

```ci
value = new Box;
```

Loops are also conservative. If a loop body may create a borrowed alias to an outer owner, later moving or replacing that owner can be rejected.

## Native handles

Native handles are represented as owned VM objects with a native cleanup callback. They are deliberately non-copyable so cleanup runs exactly once.

Use `move` to transfer a native handle if an API is designed to accept ownership.

## Native ABI and contiguous arrays

CI's native scalar ABI uses fixed-width integer storage and 32-bit `float` /
64-bit `double` values. `vm_host_supports_native_scalar` reports whether the
host C implementation has the same scalar size and floating-point format.
Native struct layouts can be checked with `VMNativeLayoutField` metadata and
`vm_native_layout_matches`; this accepts ordinary natural C layout and rejects
unexpected size, alignment, or field offsets rather than silently reinterpreting
the bytes.

Primitive arrays such as `float[]` are stored contiguously. A native callback
can request a checked zero-copy view:

```c
VMNativeArrayView view;
if (!vm_get_native_array_view(frame->ref_args[0], VM_SCALAR_F32, &view)) {
    /* Wrong element type/layout; report an interop error. */
}
/* view.data points at the CI array's float bytes; view.byte_length is in bytes. */
```

The typed fast-method binding macro `BIND_FAST_METHOD1_REF` describes a
one-reference native parameter. `BIND_METHOD` remains available for
`ApiCallFrame` callbacks; both can call `vm_get_native_array_view`. The runtime
regression suite exercises both callback paths and checks that the returned
pointer is the original array storage, not a converted copy.

The fast callback path can use the same helper on its reference argument. The
view is borrowed: do not retain its pointer after the native call or after the
CI array's owner is destroyed. For a `glBufferData`-style call, pass
`view.byte_length` and `view.data` directly while the array is alive.

Ordinary CI classes are not automatically C structs. In particular, an array
of class instances currently stores object references, not inline `Vec3` bytes.
For direct vertex upload today, use a contiguous `float[]` (for example, three
floats per vertex). A true `Vec3[]` with inline, validated C layout requires a
separate value-type/array feature and is not yet implemented. Pointer-bearing
classes and managed ownership metadata must never be exposed as raw C structs.

## Performance model

The current runtime uses:

- a primitive stack for primitive locals and arguments,
- a reference stack for object/array/string references,
- primitive registers,
- reference registers,
- deterministic destruction for owned references.

The split stack keeps primitive code from checking whether every value is an object.

This design is intended to make ownership mostly a compile-time concern, with runtime cleanup emitted only where owned references actually leave scope.

## Current limitations

This is still a prototype. Known areas that need more design work:

- more precise borrow analysis across complex control flow,
- mutable-borrow/exclusive-borrow design,
- deeper escape analysis for member and array aliases,
- better public embedding API names,
- more VM/codegen optimization,
- a formal grammar/specification.

It is not production-ready. In particular, one VM context must only be used by
one thread at a time, the embedding API is still evolving, and broad fuzzing,
platform coverage, and release packaging are not complete.
