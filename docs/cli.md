# Command-line runner

The standalone runner is `ci.exe`.

It compiles a `.ci` source file, runs global code, and then calls `setup()` if the script defines it.

## Build

From the project root:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_ci.ps1
```

This creates:

```text
bin\ci.exe
```

## Run

```powershell
.\bin\ci.exe test.ci
```

If the `bin` directory is on your `PATH`, you can run:

```powershell
ci test.ci
```

## Options

```text
ci [options] <script.ci>

options:
  --no-setup       compile and run globals only
  --update <dt>    call update(dt) once after setup, if present
  --debug          start the VM in stepping/debug mode for globals
  -h, --help       show help
```

## Saved bytecode compatibility

Saved bytecode uses a versioned header declaring its file and opcode versions.
The current loader accepts the current opcode set and the original unheaded
container format; it rejects unknown future versions before initializing the
VM. Keep the compiler and runtime versions paired when distributing bytecode.

## Built-in IO bindings

The runner currently provides a small `IO` class:

```ci
IO.printS("text");
IO.nl();
IO.printI(123);
IO.printF(1.5);
IO.printb(true);
```

Example:

```ci
fun setup(): int {
    IO.printS("hello from CI");
    IO.nl();
    IO.printI(42);
    return 42;
}
```
