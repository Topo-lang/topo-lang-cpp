// CppExtractorFidelityTest.cpp
//
// M4.2 — Golden fidelity tests for topo-extract-cpp.
//
// For each fixture directory under TOPO_CPP_FIDELITY_FIXTURES_DIR:
//   - read `request.json`
//   - rewrite relative `files` entries to absolute paths rooted in the
//     fixture directory so the subprocess does not depend on the caller's cwd
//   - spawn `topo-extract-cpp` (path baked in via TOPO_EXTRACT_CPP_BINARY)
//   - feed the request to the child stdin and CLOSE the write end before
//     draining stdout (topo-extract-cpp reads stdin-to-EOF then emits its
//     TranspileModule JSON on stdout)
//   - parse stdout as JSON
//   - compare to `expected.json`
//
// Why a local subprocess helper?
// ------------------------------
// topo::platform::PipedProcess was designed for bidirectional JSON-RPC
// framing (LSP/clangd) and intentionally keeps both pipes open until stop().
// That creates a deadlock for one-shot request/response children like the
// extractor: the parent cannot close stdin (to signal EOF so the child
// proceeds to serialize output) while still holding stdout open for reading.
// Rather than extending Platform, this test uses a self-contained POSIX/
// Windows subprocess helper scoped to the test binary. See `runExtractorOnce`.
//
// Golden bootstrap / update semantics
// -----------------------------------
// The task M4.2 forbids hand-writing golden JSON — goldens must be produced
// by the extractor itself. To support that workflow without requiring a
// separate bootstrap script, this driver uses snapshot-test semantics:
//
//   * If `expected.json` is missing, the driver writes the actual extractor
//     output to `expected.json` and the test PASSES. This is the first-run
//     bootstrap path: the developer inspects the newly written golden,
//     reviews it, and commits it.
//   * If `expected.json` exists, the driver performs a strict field-level
//     comparison via `nlohmann::json::operator==` (which ignores object key
//     order). On mismatch the test fails with a diff hint.
//   * When the environment variable `TOPO_FIDELITY_UPDATE=1` is set, any
//     existing `expected.json` is overwritten with the fresh output and the
//     test passes. This is the explicit "regenerate golden" path.
//
// A fixture directory containing a `SKIP.md` file is skipped with
// GTEST_SKIP() and the reason from the first line of SKIP.md is logged.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <spawn.h>
#    include <sys/wait.h>
#    include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef TOPO_CPP_FIDELITY_FIXTURES_DIR
#    error "TOPO_CPP_FIDELITY_FIXTURES_DIR must be defined at compile time"
#endif
#ifndef TOPO_EXTRACT_CPP_BINARY
#    error "TOPO_EXTRACT_CPP_BINARY must be defined at compile time"
#endif

namespace {

// ---------------------------------------------------------------------------
// Subprocess helper: launch topo-extract-cpp with piped stdin/stdout.
// Writes `input` to the child's stdin and closes it, then drains stdout.
// Returns `true` on successful spawn + clean wait (even if the child exits
// non-zero); populates `outOutput` with stdout bytes and `outErr` with a
// human-readable error if spawn/wait failed.
// ---------------------------------------------------------------------------
bool runExtractorOnce(const std::string& binary,
                      const std::string& input,
                      std::string& outOutput,
                      std::string& outErr) {
    outOutput.clear();
    outErr.clear();

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;

    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
        outErr = "CreatePipe(stdin) failed";
        return false;
    }
    if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        outErr = "SetHandleInformation(stdin) failed";
        return false;
    }
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        outErr = "CreatePipe(stdout) failed";
        return false;
    }
    if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(stdinRead);
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        outErr = "SetHandleInformation(stdout) failed";
        return false;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    // Quote the binary path if it contains spaces.
    std::string cmdLine = binary.find(' ') != std::string::npos ? ("\"" + binary + "\"") : binary;
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(binary.c_str(),
                             cmdBuf.data(),
                             nullptr,
                             nullptr,
                             TRUE,  // inherit handles
                             0,
                             nullptr,
                             nullptr,
                             &si,
                             &pi);
    // Parent closes the child-side ends immediately.
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (!ok) {
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        outErr = "CreateProcess failed for " + binary;
        return false;
    }

    // Write stdin, close write end to signal EOF.
    DWORD written = 0;
    if (!input.empty()) {
        WriteFile(stdinWrite, input.data(), static_cast<DWORD>(input.size()), &written, nullptr);
    }
    CloseHandle(stdinWrite);

    // Drain stdout.
    char buf[4096];
    DWORD bytesRead = 0;
    while (ReadFile(stdoutRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        outOutput.append(buf, buf + bytesRead);
    }
    CloseHandle(stdoutRead);

    // Wait for child exit.
    WaitForSingleObject(pi.hProcess, 30000);  // 30s cap
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    // POSIX implementation
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0) {
        outErr = "pipe(stdin) failed";
        return false;
    }
    if (pipe(outPipe) != 0) {
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        outErr = "pipe(stdout) failed";
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Child: dup inPipe[0] onto STDIN, dup outPipe[1] onto STDOUT,
    // close the parent-side ends.
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);

    // argv: [binary, nullptr]
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(binary.c_str()));
    argv.push_back(nullptr);

    pid_t pid = -1;
    int err = posix_spawn(&pid, binary.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    // Parent closes the child-side ends.
    ::close(inPipe[0]);
    ::close(outPipe[1]);

    if (err != 0) {
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        outErr = "posix_spawn failed for " + binary + " (errno " + std::to_string(err) + ")";
        return false;
    }

    // Write input to stdin, close write end to signal EOF.
    const char* p = input.data();
    size_t remaining = input.size();
    while (remaining > 0) {
        ssize_t n = ::write(inPipe[1], p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    ::close(inPipe[1]);

    // Drain stdout.
    char buf[4096];
    while (true) {
        ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;  // EOF
        outOutput.append(buf, buf + n);
    }
    ::close(outPipe[0]);

    // Reap the child.
    int status = 0;
    waitpid(pid, &status, 0);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

std::string readFile(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void writeFile(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary);
    ofs << text;
}

bool envBool(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    std::string s(v);
    return !(s == "0" || s == "false" || s == "FALSE");
}

// ---------------------------------------------------------------------------
// Request rewriting: make `files` entries absolute relative to fixtureDir.
// ---------------------------------------------------------------------------
void absolutizeFiles(json& request, const fs::path& fixtureDir) {
    if (!request.contains("files") || !request["files"].is_array()) return;
    for (auto& f : request["files"]) {
        if (!f.is_string()) continue;
        fs::path p = f.get<std::string>();
        if (p.is_absolute()) continue;
        f = (fixtureDir / p).lexically_normal().string();
    }
}

// Normalize filesystem-dependent fields so golden comparison remains stable
// across checkout locations (main repo, git worktrees, CI scratch dirs).
//
// Two redactions, applied recursively to every string in the document:
//   1. Replace the absolute path of the current fixture directory with the
//      placeholder `<FIXTURE>` — handles paths the extractor emits for files
//      under the fixture root.
//   2. Replace clang's `(lambda at <abs-path>:L:C)` descriptor's path
//      component with `<FIXTURE>` — handles clang debug descriptors whose
//      absolute path will not match the fixtureDir replacement when the
//      golden was captured in a different checkout location.
void normalizeFidelityPaths(json& node, const fs::path& fixtureDir) {
    static const std::regex kLambdaAtPath(R"(\(lambda at [^:)]*:(\d+):(\d+)\))");
    const std::string fixtureAbs = fixtureDir.lexically_normal().string();
    if (node.is_object()) {
        for (auto& kv : node.items()) normalizeFidelityPaths(kv.value(), fixtureDir);
        return;
    }
    if (node.is_array()) {
        for (auto& v : node) normalizeFidelityPaths(v, fixtureDir);
        return;
    }
    if (!node.is_string()) return;
    std::string s = node.get<std::string>();
    if (!fixtureAbs.empty()) {
        std::string::size_type pos = 0;
        while ((pos = s.find(fixtureAbs, pos)) != std::string::npos) {
            s.replace(pos, fixtureAbs.size(), "<FIXTURE>");
            pos += std::strlen("<FIXTURE>");
        }
    }
    s = std::regex_replace(s, kLambdaAtPath, "(lambda at <FIXTURE>:$1:$2)");
    node = s;
}

// ---------------------------------------------------------------------------
// Diagnostics: produce a short diff hint on mismatch.
// ---------------------------------------------------------------------------
std::string diffHint(const json& expected, const json& actual) {
    std::ostringstream oss;
    auto keys = [](const json& j) -> std::string {
        if (!j.is_object()) return j.type_name();
        std::string s;
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!first) s += ", ";
            s += it.key();
            first = false;
        }
        return s;
    };
    auto fnName = [](const json& j) -> std::string {
        if (j.is_object() && j.contains("functions") && j["functions"].is_array() && !j["functions"].empty()) {
            const auto& fn = j["functions"][0];
            if (fn.is_object() && fn.contains("qualifiedName")) {
                return fn["qualifiedName"].get<std::string>();
            }
        }
        return "<none>";
    };
    oss << "\n  expected keys: " << keys(expected);
    oss << "\n  actual   keys: " << keys(actual);
    oss << "\n  expected functions[0].qualifiedName: " << fnName(expected);
    oss << "\n  actual   functions[0].qualifiedName: " << fnName(actual);
    return oss.str();
}

// ---------------------------------------------------------------------------
// Fixture directory discovery.
// ---------------------------------------------------------------------------
std::vector<fs::path> discoverFixtures() {
    std::vector<fs::path> result;
    fs::path root(TOPO_CPP_FIDELITY_FIXTURES_DIR);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return result;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// Single-fixture driver.
// ---------------------------------------------------------------------------
void runFixture(const fs::path& fixtureDir) {
    SCOPED_TRACE("fixture: " + fixtureDir.filename().string());

    // SKIP marker
    fs::path skipMarker = fixtureDir / "SKIP.md";
    if (fs::exists(skipMarker)) {
        std::string reason = readFile(skipMarker);
        size_t nl = reason.find('\n');
        if (nl != std::string::npos) reason = reason.substr(0, nl);
        GTEST_SKIP() << "SKIP.md present: " << reason;
    }

    fs::path requestPath = fixtureDir / "request.json";
    fs::path expectedPath = fixtureDir / "expected.json";
    fs::path inputPath = fixtureDir / "input.cpp";

    ASSERT_TRUE(fs::exists(requestPath)) << "missing request.json at " << requestPath;
    ASSERT_TRUE(fs::exists(inputPath)) << "missing input.cpp at " << inputPath;

    // Parse request, absolutize file paths.
    json request;
    try {
        request = json::parse(readFile(requestPath));
    } catch (const json::exception& e) {
        FAIL() << "failed to parse request.json: " << e.what();
        return;
    }
    absolutizeFiles(request, fixtureDir);
    std::string requestStr = request.dump();

    // Run the extractor subprocess.
    std::string actualRaw;
    std::string runErr;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, requestStr, actualRaw, runErr);
    ASSERT_TRUE(ok) << "failed to run extractor: " << runErr;
    ASSERT_FALSE(actualRaw.empty())
        << "extractor produced no output (binary=" << TOPO_EXTRACT_CPP_BINARY << ")";

    json actual;
    try {
        actual = json::parse(actualRaw);
    } catch (const json::exception& e) {
        FAIL() << "extractor output is not valid JSON: " << e.what() << "\n  raw: " << actualRaw;
        return;
    }
    normalizeFidelityPaths(actual, fixtureDir);

    // Bootstrap / update path. Goldens are stored with the canonical
    // `<FIXTURE>` placeholder so the on-disk file matches what compare
    // produces from any checkout location.
    const bool forceUpdate = envBool("TOPO_FIDELITY_UPDATE");
    if (forceUpdate || !fs::exists(expectedPath)) {
        writeFile(expectedPath, actual.dump(2) + "\n");
        SUCCEED() << "golden written to " << expectedPath
                  << (forceUpdate ? " (TOPO_FIDELITY_UPDATE=1)" : " (bootstrap)");
        return;
    }

    // Strict compare path. Apply the same normalization to expected so
    // pre-existing goldens that still embed an absolute path survive a
    // run from a different checkout (e.g. a worktree).
    json expected;
    try {
        expected = json::parse(readFile(expectedPath));
    } catch (const json::exception& e) {
        FAIL() << "failed to parse expected.json: " << e.what();
        return;
    }
    normalizeFidelityPaths(expected, fixtureDir);

    EXPECT_EQ(expected, actual) << "TranspileModule JSON mismatch" << diffHint(expected, actual);
}

void runNamed(const char* name) {
    fs::path root(TOPO_CPP_FIDELITY_FIXTURES_DIR);
    runFixture(root / name);
}

} // namespace

// ---------------------------------------------------------------------------
// Per-fixture TEST declarations (one test per fixture for granular pass/fail).
// ---------------------------------------------------------------------------

TEST(CppExtractorFidelity, BasicFunction) { runNamed("01_basic_function"); }
TEST(CppExtractorFidelity, Namespace) { runNamed("02_namespace"); }
TEST(CppExtractorFidelity, ClassMethod) { runNamed("03_class_method"); }
TEST(CppExtractorFidelity, DefaultParams) { runNamed("04_default_params"); }
TEST(CppExtractorFidelity, VirtualMethod) { runNamed("05_virtual_method"); }
TEST(CppExtractorFidelity, LambdaCapture) { runNamed("06_lambda_capture"); }
TEST(CppExtractorFidelity, UsingAlias) { runNamed("07_using_alias"); }
TEST(CppExtractorFidelity, MacroExpansion) { runNamed("08_macro_expansion"); }
TEST(CppExtractorFidelity, FriendFunction) { runNamed("09_friend_function"); }
TEST(CppExtractorFidelity, ControlFlow) { runNamed("10_control_flow"); }
TEST(CppExtractorFidelity, OperatorOverload) { runNamed("11_operator_overload"); }
TEST(CppExtractorFidelity, ReturnStruct) { runNamed("12_return_struct"); }
TEST(CppExtractorFidelity, TemplateFunction) { runNamed("13_template_function"); }
TEST(CppExtractorFidelity, RequiresMultiBound) { runNamed("14_requires_multi_bound"); }
TEST(CppExtractorFidelity, NonTypeParamDefault) { runNamed("15_nontype_param_default"); }
TEST(CppExtractorFidelity, VariadicParam) { runNamed("16_variadic_param"); }
TEST(CppExtractorFidelity, TemplateTemplateParam) { runNamed("17_template_template_param"); }

// ---------------------------------------------------------------------------
// Inheritance extraction: the C++ extractor must now
// populate module.types with qualifiedName + baseClasses (+ all-Class
// baseClassKinds, since C++ has no interfaces) from CXXBaseSpecifier nodes.
// Pre-change behavior emitted `"types": []` unconditionally — this test
// pins the new path end-to-end through the real topo-extract-cpp subprocess.
// ---------------------------------------------------------------------------
TEST(CppExtractorFidelity, InheritanceExtractedIntoTypes) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_inherit_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "struct B { int b; };\n"
                   "struct M { int m; };\n"
                   "struct D : public B, public M {\n"
                   "    int d;\n"
                   "    void run() {}\n"
                   "};\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array(); // empty ⇒ no specific function needed
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;
    ASSERT_TRUE(out["types"].is_array());

    // Find D.
    const json* d = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "D") {
            d = &t;
            break;
        }
    }
    ASSERT_NE(d, nullptr) << "type D not extracted; types=" << out["types"].dump();

    ASSERT_TRUE(d->contains("baseClasses")) << d->dump();
    ASSERT_EQ((*d)["baseClasses"].size(), 2u) << d->dump();
    EXPECT_EQ((*d)["baseClasses"][0]["nameParts"][0], "B");
    EXPECT_EQ((*d)["baseClasses"][1]["nameParts"][0], "M");

    ASSERT_TRUE(d->contains("baseClassKinds")) << d->dump();
    ASSERT_EQ((*d)["baseClassKinds"].size(), 2u);
    EXPECT_EQ((*d)["baseClassKinds"][0], "class") << "C++ has no interfaces";
    EXPECT_EQ((*d)["baseClassKinds"][1], "class");
}

// ---------------------------------------------------------------------------
// Declaration-site generic type parameters: a `template<typename T>` class
// template must surface its bare type-param names in
// module.types[].templateParams (in source order). A non-type template
// param (`template<int N>`) is NOT representable yet -- the extractor must
// drop it from templateParams and downgrade the type's fidelity, never
// emit a malformed/half-recovered generic clause. An in-TU definition is
// used because the extractor has no system include paths.
// ---------------------------------------------------------------------------
TEST(CppExtractorFidelity, TypeParamRecoveredIntoTemplateParams) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_tparam_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "template<typename T> struct Box { T x; };\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array();
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;

    const json* box = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "Box") {
            box = &t;
            break;
        }
    }
    ASSERT_NE(box, nullptr) << "type Box not extracted; types=" << out["types"].dump();

    ASSERT_TRUE(box->contains("templateParams")) << box->dump();
    ASSERT_EQ((*box)["templateParams"].size(), 1u) << box->dump();
    EXPECT_EQ((*box)["templateParams"][0]["kind"], "type");
    EXPECT_EQ((*box)["templateParams"][0]["name"], "T");
    // A faithfully recovered bare type param keeps fidelity at source.
    EXPECT_EQ(box->value("fidelity", "source"), "source") << box->dump();
}

// C++17 default type-param `template <typename T = int> struct Box`: the
// libclang AST hangs a single TypeRef child off the TemplateTypeParameter
// pointing at the default type. The extractor captures it as the wire
// `default: TypeNode`, mirroring the Rust struct-site / TS / Python PEP 696
// pipeline. Built-in scalars resolve via the cursor type spelling (their
// referenced-cursor walk is empty); name parts arrive as ["int"].
TEST(CppExtractorFidelity, DefaultTypeParamRecoveredIntoTemplateParams) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_tparam_default_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "template<typename T = int> struct Box { T x; };\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array();
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;

    const json* box = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "Box") {
            box = &t;
            break;
        }
    }
    ASSERT_NE(box, nullptr) << "type Box not extracted; types=" << out["types"].dump();

    ASSERT_TRUE(box->contains("templateParams")) << box->dump();
    ASSERT_EQ((*box)["templateParams"].size(), 1u) << box->dump();
    const auto& tp0 = (*box)["templateParams"][0];
    EXPECT_EQ(tp0["kind"], "type");
    EXPECT_EQ(tp0["name"], "T");
    ASSERT_TRUE(tp0.contains("default")) << "default key must be present: " << tp0.dump();
    const auto& def = tp0["default"];
    ASSERT_TRUE(def.contains("nameParts")) << def.dump();
    ASSERT_EQ(def["nameParts"].size(), 1u) << def.dump();
    EXPECT_EQ(def["nameParts"][0], "int")
        << "default type must round-trip via the cursor type spelling; got "
        << def.dump();
    // A bare-default capture preserves source fidelity — nothing dropped.
    EXPECT_EQ(box->value("fidelity", "source"), "source") << box->dump();
}

TEST(CppExtractorFidelity, NonTypeTemplateParamRecoveredIntoTemplateParams) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_ntparam_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "template<int N> struct A { int v; };\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array();
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;

    const json* a = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "A") {
            a = &t;
            break;
        }
    }
    ASSERT_NE(a, nullptr) << "type A not extracted; types=" << out["types"].dump();

    // Non-type param surfaces as kind="nontype" with `bound` carrying the
    // value type (the cursor's CXType spelling). The struct stays at source
    // fidelity since nothing was lost.
    ASSERT_TRUE(a->contains("templateParams")) << a->dump();
    ASSERT_EQ((*a)["templateParams"].size(), 1u) << a->dump();
    const auto& tp0 = (*a)["templateParams"][0];
    EXPECT_EQ(tp0["kind"], "nontype");
    EXPECT_EQ(tp0["name"], "N");
    ASSERT_TRUE(tp0.contains("bound")) << tp0.dump();
    ASSERT_TRUE(tp0["bound"].contains("nameParts")) << tp0["bound"].dump();
    ASSERT_EQ(tp0["bound"]["nameParts"].size(), 1u) << tp0["bound"].dump();
    EXPECT_EQ(tp0["bound"]["nameParts"][0], "int")
        << "non-type param value type must round-trip via the cursor type "
           "spelling; got " << tp0["bound"].dump();
    EXPECT_EQ(a->value("fidelity", "source"), "source") << a->dump();
}

// Variadic template parameter (`template <typename... Ts>`): the extractor
// must surface the pack as a kind="type" param carrying `isVariadic=true`.
// libclang has no pack API — the extractor token-scans the param extent for
// the `...` ellipsis. The pack flag is orthogonal to `kind`, and nothing is
// lost so the type stays at source fidelity.
TEST(CppExtractorFidelity, VariadicTypeParamRecoveredIntoTemplateParams) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_variadic_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "template <typename... Ts> struct Tuple {};\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array();
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;

    const json* tuple = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "Tuple") {
            tuple = &t;
            break;
        }
    }
    ASSERT_NE(tuple, nullptr) << "type Tuple not extracted; types=" << out["types"].dump();

    ASSERT_TRUE(tuple->contains("templateParams")) << tuple->dump();
    ASSERT_EQ((*tuple)["templateParams"].size(), 1u) << tuple->dump();
    const auto& tp0 = (*tuple)["templateParams"][0];
    EXPECT_EQ(tp0["kind"], "type") << "a variadic pack stays kind=type";
    EXPECT_EQ(tp0["name"], "Ts");
    ASSERT_TRUE(tp0.contains("isVariadic")) << "isVariadic key must be present: " << tp0.dump();
    EXPECT_EQ(tp0["isVariadic"], true);
    EXPECT_EQ(tuple->value("fidelity", "source"), "source") << tuple->dump();
}

// Template-template parameter (`template <template<typename> class C>`): the
// extractor must surface kind="template" and record the single inner type
// param into `innerParams`. The MVP only recognises the canonical
// `template<typename> class X` shape.
TEST(CppExtractorFidelity, TemplateTemplateParamRecoveredIntoTemplateParams) {
#ifdef _WIN32
    const long pid = static_cast<long>(GetCurrentProcessId());
#else
    const long pid = static_cast<long>(::getpid());
#endif
    fs::path tmp = fs::temp_directory_path() / ("topo_extract_ttparam_" + std::to_string(pid));
    fs::create_directories(tmp);
    fs::path src = tmp / "input.cpp";
    writeFile(src, "template <template<typename> class C> struct Holder {};\n");

    json request;
    request["files"] = json::array({src.string()});
    request["functions"] = json::array();
    request["symbolTable"] = json::object();

    std::string raw, err;
    bool ok = runExtractorOnce(TOPO_EXTRACT_CPP_BINARY, request.dump(), raw, err);
    fs::remove_all(tmp);
    ASSERT_TRUE(ok) << "failed to run extractor: " << err;
    ASSERT_FALSE(raw.empty()) << "extractor produced no output";

    json out = json::parse(raw);
    ASSERT_TRUE(out.contains("types")) << raw;

    const json* holder = nullptr;
    for (const auto& t : out["types"]) {
        if (t.value("qualifiedName", "") == "Holder") {
            holder = &t;
            break;
        }
    }
    ASSERT_NE(holder, nullptr) << "type Holder not extracted; types=" << out["types"].dump();

    ASSERT_TRUE(holder->contains("templateParams")) << holder->dump();
    ASSERT_EQ((*holder)["templateParams"].size(), 1u) << holder->dump();
    const auto& tp0 = (*holder)["templateParams"][0];
    EXPECT_EQ(tp0["kind"], "template")
        << "template-template param must surface kind=template; got " << tp0.dump();
    EXPECT_EQ(tp0["name"], "C");
    ASSERT_TRUE(tp0.contains("innerParams")) << "innerParams key must be present: " << tp0.dump();
    ASSERT_EQ(tp0["innerParams"].size(), 1u) << tp0["innerParams"].dump();
    EXPECT_EQ(tp0["innerParams"][0]["kind"], "type")
        << "inner param of `template<typename> class C` is a plain type param";
    EXPECT_EQ(holder->value("fidelity", "source"), "source") << holder->dump();
}

// Sanity check: the fixture root must contain at least 10 entries (M4.2).
TEST(CppExtractorFidelity, FixtureRootHasAtLeastTenEntries) {
    auto dirs = discoverFixtures();
    EXPECT_GE(dirs.size(), 10u)
        << "M4.2 acceptance: at least 10 extractor fidelity fixtures required;"
           " found "
        << dirs.size() << " in " << TOPO_CPP_FIDELITY_FIXTURES_DIR;
}
