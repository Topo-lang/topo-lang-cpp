# topo-debug-cpp

Real-lldb Extract adapter for C++ host binaries.

Compiled as `topo-debug-cpp`, this executable links liblldb (from
`topo-llvm/llvm-dev/lib/liblldb.dylib` / `.so`) and drives a host binary on
behalf of `topo-debug query`. Given `--site <file:line>` and `--target <bin>`
it sets a breakpoint, launches the process, reads the selected in-scope
variables from the stopped frame, and emits the bytes + layout descriptor
over the Extract wire protocol on stdout.

The adapter is wire-compatible with the mock adapter
(`topo-debug-mock-adapter`); pick between them at the `topo-debug --adapter`
flag. Variable selection comes from `--var` (comma-separated); the driving
CLI extracts the bound names from the query AST and passes them through. When
no variable is given the adapter defaults to `matrix`.

Supported variable types: primitive integer / float scalars, N-dimensional
arrays of those, and structs of primitive leaves. Anything else exits 4 with
a diagnostic.
