#ifndef TOPO_APP_READBACK_H
#define TOPO_APP_READBACK_H

// @stability provisional
// User-facing read-back utility. Depends on `topo --ast-dump`'s
// textual format, which itself is documented as `provisional` (the
// AST-dump consumption surface is allowed to change between minor
// releases without a breaking-change note). This header inherits
// that tier.

// Read .topo back into a Graph by parsing it with the real toolchain.
//
// Round-trip fidelity is the proposal's decisive constraint. To prove it
// honestly, read-back must go through the *actual* Topo parser, not a
// C++ re-implementation of the grammar (which could agree with the
// emitter by accident). We invoke `topo --ast-dump` and reconstruct the
// graph from the parser's own structured dump. This simultaneously
// proves "emitted .topo parses under the merged grammar" (the dump only
// succeeds if the parser accepts it) and yields graph' for the
// equivalence check. One-to-one with the Python reference's
// _readback.py.
//
// Subprocess use: the pure-value model headers (app_model.h, app.h,
// app_emit.h) shell out to nothing. Read-back and check *must* run the
// real binaries — that is the whole point of an honest round-trip — so
// these two headers (and only these) use the platform process facility.
// The Python reference shells out for exactly the same reason; this is
// parity, not a portability regression in the model.

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <topo/Platform/Process.h>
#include <topo/Platform/TempFile.h>
#include <topo/app_model.h>

namespace topo::app {

// Resolve the fresh `topo` binary. An explicit path wins (tests/CI set
// TOPO_BIN so the *fresh* tree is used, never a stale build-no-llvm —
// that staleness is a tracked issue); otherwise a clear error, because
// silently degrading a correctness tool defeats the purpose.
inline std::string topo_bin() {
    if (const char* p = std::getenv("TOPO_BIN")) return p;
    throw std::runtime_error(
        "TOPO_BIN unset: point it at the FRESH "
        "build/topo-core/tools/topo/topo binary");
}

inline std::string topo_check_bin() {
    if (const char* p = std::getenv("TOPO_CHECK_BIN")) return p;
    throw std::runtime_error(
        "TOPO_CHECK_BIN unset: point it at the FRESH "
        "build/topo-cli/tools/topo-check/topo-check binary");
}

// Captured result of a child process: combined behaviour is enough for
// this bridge (stdout carries the AST dump / verdict; non-zero exit or a
// rejected parse is itself the conformance signal).
struct ProcResult {
    int exit_code;
    std::string stdout_text;
};

// Run a binary with the given argv vector, capturing stdout (stderr
// folded in so a parse rejection's diagnostic is visible to the caller).
//
// Audit issue topo-lang-cpp-app-readback-shell-popen: previously this
// header called ``popen()`` with a shell-string concatenation, which
// (a) failed to compile on Windows (no POSIX <sys/wait.h>) and (b)
// opened a shell-injection surface because ``TOPO_BIN`` /
// ``TOPO_CHECK_BIN`` env vars flowed through ``/bin/sh -c`` with no
// quoting on the env-var side. Routing through
// ``topo::platform::runProcessCapture`` uses argv-array exec (no
// shell, no quoting needed) and inherits the cross-platform impl in
// TopoPlatform. Per principle ``input-validation-at-system-boundary``
// the user-controlled env-var paths must never reach a shell.
inline ProcResult run_capture(const std::string& executable,
                              const std::vector<std::string>& args) {
    auto r = topo::platform::runProcessCapture(executable, args);
    std::string combined = r.stdoutOutput;
    combined.append(r.stderrOutput);
    return ProcResult{r.exitCode, std::move(combined)};
}

// Map a scalar's .topo spelling back to a TypeRef. The aliases the C++
// host emits are int/float/bool/str (app_emit.h preamble); reading them
// back is the inverse of TypeRef::topo().
inline TypeRef scalar_from_spelling(const std::string& s) {
    if (s == "int") return TypeRef::of_scalar(Scalar::Int);
    if (s == "float") return TypeRef::of_scalar(Scalar::Float);
    if (s == "bool") return TypeRef::of_scalar(Scalar::Bool);
    if (s == "str") return TypeRef::of_scalar(Scalar::Str);
    throw std::runtime_error("unknown scalar spelling in AST dump: " + s);
}

namespace detail {

// "record<id: int, amount: float>" -> TypeRef. Record fields in this
// bridge are scalar-typed (one nesting level, matching the proposal's
// order example), so a top-level comma split is sufficient — identical
// assumption to the Python reference's _parse_type.
inline TypeRef parse_type(std::string spec) {
    auto trim = [](std::string s) {
        std::size_t a = s.find_first_not_of(" \t");
        std::size_t b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string()
                                      : s.substr(a, b - a + 1);
    };
    spec = trim(spec);
    std::smatch m;
    if (std::regex_match(spec, m, std::regex(R"(record<(.+)>)"))) {
        std::vector<RecordField> fields;
        std::string body = m[1].str();
        std::size_t pos = 0;
        while (pos <= body.size()) {
            std::size_t comma = body.find(',', pos);
            std::string part = body.substr(
                pos, comma == std::string::npos ? std::string::npos
                                                : comma - pos);
            std::size_t colon = part.find(':');
            std::string fname = trim(part.substr(0, colon));
            std::string ftype = trim(part.substr(colon + 1));
            fields.push_back({fname, scalar_from_spelling(ftype)});
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return TypeRef::of_record(std::move(fields));
    }
    return scalar_from_spelling(spec);
}

}  // namespace detail

// Parse .topo source text into a Graph via `topo --ast-dump`. Throws if
// the toolchain rejects the source — that rejection is itself the
// grammar-conformance signal (same contract as the Python reference).
//
// Audit fix: the temp file is created via ``topo::platform::TempFile``
// (RAII, unique-name probe with O_CREAT|O_EXCL on POSIX / CREATE_NEW
// on Windows); a predictable ``temp_path`` was a symlink-attack and
// concurrent-corruption surface (sister issue
// ``topo-lang-cpp-app-static-wrapped-temp-collision``). The subprocess
// is dispatched argv-style via ``topo::platform::runProcessCapture``
// with no shell — the user-controlled ``TOPO_BIN`` env var is therefore
// never expanded by ``/bin/sh -c``.
inline Graph read_topo(const std::string& text) {
    topo::platform::TempFile tmp("topo-roundtrip", ".topo");
    {
        std::ofstream f(tmp.path());
        f << text;
    }
    ProcResult r = run_capture(topo_bin(),
                               {"--ast-dump", tmp.path().string()});
    if (r.exit_code != 0)
        throw std::runtime_error("topo --ast-dump rejected emitted .topo:\n" +
                                 r.stdout_text);

    // The AST-dump grammar these regexes match is pinned by a
    // round-trip contract test:
    //   * Producer: topo-core/lib/AST/ASTPrinter.cpp
    //   * Contract test: topo-lang-cpp-ast-dump-pin-tests
    //     (topo-lang-cpp/runtime/test/test_app_readback_ast_dump_pin.cpp)
    // Any drift in the four line forms below fails the byte-equality
    // assertion in the contract test before reaching downstream users.
    // The same lines the Python reference's _readback.py keys off.
    std::regex ns_re(R"(NamespaceDecl '(\w+)')");
    std::regex h_re(R"(HandlerDecl '(\w+)\((.*?)\)\s*->\s*(.+)')");
    std::regex flow_re(R"(FlowBlock '(\w+)')");
    std::regex edge_re(R"(Edge (\w+) -> (\w+)(?:\s*\[terminal\])?)");

    std::string namespace_name;
    std::vector<Handler> handlers;
    std::optional<Flow> flow;

    std::istringstream lines(r.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch m;
        if (std::regex_search(line, m, ns_re)) {
            namespace_name = m[1].str();
            continue;
        }
        if (std::regex_search(line, m, h_re)) {
            std::string name = m[1].str();
            std::string params = m[2].str();
            std::string ret = m[3].str();
            // trim params
            std::size_t a = params.find_first_not_of(" \t");
            std::size_t b = params.find_last_not_of(" \t");
            params = a == std::string::npos ? std::string()
                                            : params.substr(a, b - a + 1);
            std::optional<TypeRef> in_type;
            if (!params.empty()) {
                // "Type in" — strip the conventional parameter name.
                std::size_t sp = params.rfind(' ');
                in_type = detail::parse_type(params.substr(0, sp));
            }
            handlers.push_back(
                Handler{name, in_type, detail::parse_type(ret)});
            continue;
        }
        if (std::regex_search(line, m, flow_re)) {
            Flow f;
            f.name = m[1].str();
            flow = f;
            continue;
        }
        if (flow.has_value() && std::regex_search(line, m, edge_re)) {
            std::string src = m[1].str();
            std::string tgt = m[2].str();
            flow->edges.push_back(
                Edge{src, tgt == "void" ? std::optional<std::string>()
                                        : std::optional<std::string>(tgt)});
        }
    }

    Graph g(namespace_name);
    g.handlers() = std::move(handlers);
    if (flow.has_value()) g.set_flow(std::move(*flow));
    return g;
}

}  // namespace topo::app

#endif  // TOPO_APP_READBACK_H
