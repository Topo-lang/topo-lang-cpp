# topo-lang-cpp -- C++ Language Support

C++ language analysis, extraction, compilation driver, LSP bridge, and user-facing runtime headers for the Topo toolchain.

## Structure

Second-level directories are named after the `topo-<tool>` they serve, so the mapping from code to top-level component is explicit.

| Directory | Serves | Purpose |
|-----------|--------|---------|
| runtime/ | user code | Headers: tuple, array, span, slot, table, view, pipeline, parallel, jit, adaptive, arena, observe |
| topo-check/analysis/ | topo-check | CppAnalysisProvider + extractors (extract/) + safety catalog (catalog/) + stub generator (stub/) |
| topo-check/runner/ | topo-check | CppCheckRunner -- language-specific check orchestration |
| topo-check/extractor/ | topo-check | Standalone libclang-based symbol/body extractor (`topo-extract-cpp`) |
| topo-build/ | topo-build | CppDriver (clang++ -emit-llvm) + NinjaGen build file generator |
| topo-init/ | topo-init | C++ project template provider |
| topo-lsp/ | topo-lsp | ClangdBridge -- proxies clangd for IDE integration |
| topo-transpile/ | topo-transpile | CppEmitter -- AST → C++ source |
| topo-lang/ | topo-lang | CppPlugin -- registers all components with the language-plugin framework |
| test/ | — | Unit tests (RuntimeTypes, StubGenerator, SymbolExtractor) and integration tests (NinjaGen) |
| examples/ | — | quickstart and showcase projects |

## Build

Part of the Topo project. Build with:
```bash
cmake -B build && cmake --build build
```

Integration tests (NinjaGen) require `TOPO_ENABLE_LLVM=ON` for clang resolution.

## Tests

```bash
ctest --test-dir build -R "topo-lang-cpp"
```
