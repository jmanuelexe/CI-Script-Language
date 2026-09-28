# CI Ownership

CI Ownership is an experimental C-like scripting language and virtual machine
with deterministic object ownership. This source package contains the
compiler, VM, standalone command-line runner, documentation, and a small CI
example. 

## Build on Windows

Install Visual Studio with the C++ desktop development workload, then run
PowerShell from this directory:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_ci.ps1
```

This creates `bin\ci.exe` locally. The build uses only the compiler/runtime
sources in this package and the installed Microsoft toolchain.

## Try the example

```powershell
.\bin\ci.exe .\examples\hello.ci
```

Expected output:

```text
Hello from CI!
```

## Documentation

- [Language manual](docs/manual.md)
- [Ownership model](docs/ownership.md)
- [Command-line runner](docs/cli.md)

## Status

This is an experimental, single-threaded language/runtime. See the manual and
ownership document for current behavior and limitations. The project is
licensed under the MIT License; see `LICENSE`.
