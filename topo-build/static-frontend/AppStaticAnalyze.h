#ifndef TOPO_APP_STATIC_FRONTEND_ANALYZE_H
#define TOPO_APP_STATIC_FRONTEND_ANALYZE_H

// topo-app C++ compile-time static-analysis front-end — the clang-AST
// upgrade of the regex MVP (runtime/include/topo/app_static.h).
//
// This is the `analyze` stage of the static-frontend pipeline modelled
// on the delivered Rust front-end
// (topo-lang-rust/topo-build/static-frontend/src/analyze.rs):
//
//   analyze  (clang AST JSON walk)  ->  topo::app::Graph
//          ->  emit / readback / check  (REUSED app_*.h, not rewritten)
//
// AST acquisition (planning doc task-1 decision): the front-end invokes
// `clang -Xclang -ast-dump=json` as a SUBPROCESS — the same "clang tools
// via subprocess" pattern the L2 containment path uses for `clangd`. No
// LibTooling / libclang link dependency. The clang binary is resolved by
// topo::platform::resolveLLVMTool("clang"). (The topo-build C++ compile/link
// driver CppDriver.cpp instead uses the host compiler path in cfg.hostCompilerPath
// — set by the backend tool's resolveBundledClangxx — and only calls
// resolveLLVMTool for llvm-ar.)
//
// Why clang-AST over the regex MVP: the AST is post-preprocessor and
// fully type-resolved, which directly eliminates the five constructs the
// regex front-end "deliberately does not understand":
//
//   1. macro-expanded registration calls — the AST sees the expansion;
//   2. `auto`-return handlers — clang deduces and reports the real type;
//   3. cross-line / indirect registration — a CallExpr is a CallExpr
//      regardless of source-line layout;
//   4. record-field type chains through `using` / `typedef` / template
//      alias — clang's canonical `qualType` desugars them;
//   5. nested / aliased-namespace handler symbols — clang's
//      qualified-name resolution is exact.
//
// Boundary (planning doc): this is NOT a general C++ analyzer. Only the
// topo-app registration surface is recognised. Constructs outside that
// surface are recorded in `Result::unsupported` with a fidelity
// downgrade — never silently swallowed — mirroring the Rust front-end's
// `unsupported` set.

#include <string>
#include <vector>

#include <topo/app_model.h>

namespace topo::app::staticfe {

// The recognised host-language surface fidelity of one analysis run.
//
//   Full     — every registered handler, its In/Out, the flow and all
//              record fields were resolved precisely from the AST.
//   Degraded — at least one out-of-surface construct was encountered
//              (recorded in `unsupported`); the Graph is still emitted
//              for the part that WAS understood, but the caller is told
//              the result is not a complete picture.
enum class Fidelity { Full, Degraded };

// The outcome of analyzing one C++ topo-app translation unit.
struct Result {
    // True when the AST walk produced a usable Graph. False means a hard
    // failure (clang could not parse, no App namespace, a registered
    // handler had no definition, etc.) — `error` then explains why and
    // `graph` is meaningless.
    bool ok = false;

    // The reconstructed logic graph (valid only when `ok`). Same model
    // the runtime registration bridge (app.h) builds, so it feeds the
    // reused app_emit.h / app_readback.h / app_check.h unchanged.
    Graph graph{std::string{}};

    Fidelity fidelity = Fidelity::Full;

    // Out-of-surface C++ constructs encountered during the walk. Each
    // entry is a human-readable note; a non-empty set forces
    // `fidelity == Degraded`. Never silently dropped — this is the
    // honest-degradation contract the Rust front-end also keeps.
    std::vector<std::string> unsupported;

    // Populated only when `!ok`.
    std::string error;
};

// Statically analyze a C++ topo-app source file and reconstruct the
// logic graph. Never compiles to an object file and never executes the
// program: it runs `clang -ast-dump=json -fsyntax-only` and walks the
// resulting AST.
//
// `clangBinary` — path to the clang executable (resolved by the caller
//                 via topo::platform::resolveLLVMTool("clang")).
// `sourcePath`  — the .cpp file to analyze.
// `extraArgs`   — additional clang arguments (e.g. -I include dirs,
//                 -resource-dir, -isysroot); forwarded verbatim.
Result analyzeFile(const std::string& clangBinary,
                   const std::string& sourcePath,
                   const std::vector<std::string>& extraArgs);

// Analyze pre-captured clang AST JSON text directly. Exposed so a test
// can drive the walk without re-spawning clang. `astJson` must be the
// stdout of `clang -Xclang -ast-dump=json`.
Result analyzeAstJson(const std::string& astJson);

}  // namespace topo::app::staticfe

#endif  // TOPO_APP_STATIC_FRONTEND_ANALYZE_H
