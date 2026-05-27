#ifndef TOPO_APP_CHECK_H
#define TOPO_APP_CHECK_H

// @stability provisional
// User-facing helper that drives the existing topo-check binary
// against an emitted .topo + emitted host source. API may evolve
// alongside topo-check itself.

// Zero-declaration check: hand the existing topo-check the emitted .topo.
//
// The third-scenario value is "use the framework, automatically get
// topo check" — the user
// writes no .topo by hand. We materialise a throwaway project (Topo.toml
// + emitted .topo + the user's C++ sources), run the *existing*
// topo-check binary against it, and surface the verdict. No checking
// logic is reimplemented here; this is pure orchestration, one-to-one
// with the Python reference's check.py.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <topo/Platform/TempFile.h>
#include <topo/app.h>
#include <topo/app_emit.h>
#include <topo/app_readback.h>  // run_capture (argv) / *_bin

namespace topo::app {

struct CheckResult {
    bool passed;
    int exit_code;
    std::string output;
};

namespace detail {

inline void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

inline std::string basename_of(const std::string& p) {
    std::size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace detail

// Run topo-check on the framework-emitted .topo against the given C++
// source files. No hand-written .topo anywhere in the flow. The
// [purity] force mode mirrors the Python reference so an impure parallel
// handler is flagged by core PurityCheck.
inline CheckResult check(const App& app,
                         const std::vector<std::string>& cpp_sources) {
    std::string name = app.graph().namespace_name();
    if (name.empty()) name = "topo_app";

    // Audit fix (sister of topo-lang-cpp-app-readback-shell-popen):
    // pick a per-process unique scratch dir under the platform temp
    // directory; the previous ``temp_path`` hardcoded ``/tmp`` (broken
    // on Windows) and used a predictable filename (TOCTOU surface).
    static std::atomic<unsigned long> g_counter{0};
    unsigned long nonce = g_counter.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path rootPath = topo::platform::tempDirectory() /
        ("topo_app_project-" + std::to_string(nonce));
    std::filesystem::create_directories(rootPath);
    std::filesystem::create_directories(rootPath / "topo");
    std::filesystem::create_directories(rootPath / "src");
    std::string root = rootPath.string();

    std::string toml =
        "[project]\n"
        "name = \"" + name + "\"\n\n"
        "[topo]\n"
        "root = \"topo/app.topo\"\n\n"
        "[build]\n"
        "language = \"cpp\"\n"
        "sources = [\"src/*.cpp\"]\n\n"
        "[purity]\n"
        "mode = \"force\"\n\n"
        "[completeness]\n"
        "ignore_constructors = true\n"
        "ignore_main = true\n";
    detail::write_file(root + "/Topo.toml", toml);
    detail::write_file(root + "/topo/app.topo", emit_topo(app.graph()));

    for (const auto& src : cpp_sources) {
        std::ifstream in(src, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        detail::write_file(root + "/src/" + detail::basename_of(src), body);
    }

    // Argv-style exec via topo::platform::runProcessCapture — no
    // shell, no quoting hazard; ``TOPO_CHECK_BIN`` and ``root`` flow
    // verbatim into execve / CreateProcess regardless of metacharacters.
    ProcResult r = run_capture(topo_check_bin(), {"--project", root});

    // topo-check's textual verdict is the source of truth (exit codes are
    // not always non-zero on a logical FAIL) — same contract as the
    // Python reference.
    bool passed =
        r.stdout_text.find("Result: PASS") != std::string::npos;
    return CheckResult{passed, r.exit_code, r.stdout_text};
}

}  // namespace topo::app

#endif  // TOPO_APP_CHECK_H
