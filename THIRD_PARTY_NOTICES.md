# Third-party notices

The standalone CI source package contains no third-party source code or
vendored runtime dependencies. Its Windows build script uses the Microsoft
Visual C/C++ toolchain installed on the build machine; that toolchain is not
included in the package.

The development-only benchmark tree contains Lua 5.5.1. The source-package
script intentionally excludes the benchmark tree, including Lua source and
benchmark harnesses.
