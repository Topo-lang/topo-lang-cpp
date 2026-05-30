// `topo-app-static-cpp` CLI — statically analyze a C++ topo-app source
// file and print the equivalent `.topo` to stdout.
//
// This is the clang-AST upgrade of the regex MVP that ships header-only
// as topo-lang-cpp/runtime/include/topo/app_static.h. It is the
// `topo-build`-side static-analysis front-end on the compile-time
// track: it executes none of the
// analyzed program — it runs `clang -ast-dump=json` and walks the AST.
//
// Structure mirrors the delivered Rust front-end
// (topo-lang-rust/topo-build/static-frontend, the `topo-app-static-rust`
// binary): analyze -> Graph -> emit. Unlike the Rust front-end, the C++
// front-end REUSES the existing emit / readback / check headers and the
// topo::app::Graph model rather than reimplementing them.
//
// Usage:
//   topo-app-static-cpp <app.cpp> [-- <extra clang args>]
//
// Exit codes: 0 success; 1 analysis failure; 2 usage error.

#include "AppStaticAnalyze.h"

#include "topo/Platform/ToolResolution.h"

#include <topo/app_emit.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef TOPO_APP_STATIC_CPP_RESOURCE_DIR
const char* kResourceDir = TOPO_APP_STATIC_CPP_RESOURCE_DIR;
#else
const char* kResourceDir = "";
#endif

void usage() {
    std::cerr
        << "usage: topo-app-static-cpp <app.cpp> [-- <extra clang args>]\n"
           "  statically scans the C++ topo-app registration surface via\n"
           "  clang -ast-dump=json and emits the equivalent .topo (no\n"
           "  execution). Extra clang args after `--` are forwarded\n"
           "  verbatim (e.g. -I<dir> for project includes).\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 2;
    }

    std::string sourcePath;
    std::vector<std::string> extraArgs;
    bool afterSep = false;
    for (const auto& a : args) {
        if (!afterSep && a == "--") {
            afterSep = true;
            continue;
        }
        if (afterSep) {
            extraArgs.push_back(a);
        } else if (sourcePath.empty()) {
            sourcePath = a;
        } else {
            std::cerr << "topo-app-static-cpp: unexpected argument '" << a
                      << "' (extra clang args go after `--`)\n";
            usage();
            return 2;
        }
    }

    if (sourcePath.empty()) {
        usage();
        return 2;
    }
    if (!fs::exists(sourcePath)) {
        std::cerr << "topo-app-static-cpp: cannot read '" << sourcePath
                  << "'\n";
        return 1;
    }

    // The clang binary is resolved exactly as CppDriver.cpp resolves
    // `clang++` — bundled topo-llvm/llvm-dev/bin/ first, PATH fallback.
    std::string clang = topo::platform::resolveLLVMTool("clang");

    // -resource-dir lets the bundled clang find its builtin headers
    // (<stddef.h>, intrinsics, ...) — wired by CMake at build time, the
    // same handling topo-extract-cpp uses for libclang.
    std::vector<std::string> clangArgs;
    if (kResourceDir && *kResourceDir &&
        fs::exists(std::string(kResourceDir))) {
        clangArgs.push_back("-resource-dir");
        clangArgs.push_back(kResourceDir);
    }
    for (const auto& e : extraArgs) clangArgs.push_back(e);

    topo::app::staticfe::Result r =
        topo::app::staticfe::analyzeFile(clang, sourcePath, clangArgs);

    if (!r.ok) {
        std::cerr << "static analysis failed: " << r.error << "\n";
        return 1;
    }

    // Out-of-surface constructs are reported, never swallowed. A degraded
    // fidelity still emits the understood part — but the caller is told.
    if (r.fidelity == topo::app::staticfe::Fidelity::Degraded) {
        std::cerr << "warning: analysis fidelity is DEGRADED — "
                  << r.unsupported.size()
                  << " out-of-surface construct(s):\n";
        for (const auto& u : r.unsupported)
            std::cerr << "  - " << u << "\n";
    }

    // Reuse the existing emitter (app_emit.h) — the same .topo producer
    // the runtime bridge and the regex MVP use, byte-identical output.
    std::cout << topo::app::emit_topo(r.graph);
    return 0;
}
