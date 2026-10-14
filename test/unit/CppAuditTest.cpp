#include "analysis/CppCompileModel.h"
#include "analysis/CppAnalysisProvider.h"
#include "analysis/CppCMakeModel.h"
#include "topo/Check/Audit.h"
#include "analysis/catalog/CppPreprocessorScanner.h"
#ifdef TOPO_CPP_AUDIT_LIBCLANG
#include "CppAuditExtractor.h"
#include "CppAuditDriver.h"
#include <clang-c/Index.h>
#endif
#include "topo/Platform/TempFile.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/ToolResolution.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdlib>
#include <optional>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace topo::check;
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
void traceWindowsStage(const std::string& stage) {
#ifdef _WIN32
    std::cerr << "[windows-audit-fixture] " << stage << std::endl;
#else
    (void)stage;
#endif
}
fs::path nativeCanonical(const fs::path& path) {
    std::error_code error;
    auto result = topo::platform::canonicalPath(path, error);
    if (error) throw fs::filesystem_error("resolve native fixture target", path, error);
    return result;
}
std::string nativeKey(fs::path path) { return path.make_preferred().string(); }
void createAuditSymlink(fs::path target, fs::path link, bool directory = false) {
#ifdef _WIN32
    // MinGW can leave std::filesystem link creation unimplemented even when
    // the host supports it. Keep these fixtures real, using the native API.
    target.make_preferred(); link.make_preferred();
    traceWindowsStage("create symlink " + link.string());
    const DWORD kind = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), kind | 0x2)) {
        traceWindowsStage("created symlink " + link.string()); return;
    }
    auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER || error == ERROR_PRIVILEGE_NOT_HELD) {
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), kind)) {
            traceWindowsStage("created privileged symlink " + link.string()); return;
        }
        error = GetLastError();
    }
    throw fs::filesystem_error("create audit symlink", target, link,
                               std::error_code(static_cast<int>(error), std::system_category()));
#else
    if (directory) fs::create_directory_symlink(target, link);
    else fs::create_symlink(target, link);
#endif
}
bool removeAuditSymlink(fs::path link) {
#ifdef _WIN32
    link.make_preferred();
    const DWORD attributes = GetFileAttributesW(link.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        throw fs::filesystem_error("inspect audit symlink for removal", link,
                                   std::error_code(static_cast<int>(error), std::system_category()));
    }
    if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("recorded audit symlink was replaced: " + link.string());
    const bool removed = attributes & FILE_ATTRIBUTE_DIRECTORY ?
        RemoveDirectoryW(link.c_str()) != 0 : DeleteFileW(link.c_str()) != 0;
    if (!removed)
        throw fs::filesystem_error("remove audit symlink", link,
                                   std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    return true;
#else
    return fs::remove(link);
#endif
}
bool containsInput(const json& identity, const fs::path& path) {
    const auto expected = nativeKey(path);
    for (const auto& [spelling, value] : identity.at("inputs").items()) {
        (void)value;
        if (nativeKey(spelling) == expected) return true;
    }
    return false;
}
std::string modelErrors(const CppCompileModel& model) {
    std::string result;
    for (const auto& error : model.errors) result += error + "\n";
    for (const auto& unit : model.units)
        for (const auto& error : unit.errors) result += unit.file + ": " + error + "\n";
    return result;
}
struct Fixture {
    topo::platform::TempFile anchor{"cpp-audit-test", ".tmp"};
    fs::path root{anchor.path().string() + ".dir"};
    std::vector<fs::path> symlinks;
    Fixture() { fs::create_directories(root); root = fs::canonical(root); }
    ~Fixture() {
        traceWindowsStage("cleanup begin " + root.string());
#ifdef _WIN32
        // MinGW remove_all can stall on dangling native reparse points.
        // Remove only links created by this fixture before recursive cleanup.
        bool linksRemoved = true;
        for (auto it = symlinks.rbegin(); it != symlinks.rend(); ++it) {
            try { removeAuditSymlink(*it); }
            catch (const std::exception& error) {
                ADD_FAILURE() << error.what(); linksRemoved = false;
            }
        }
        if (!linksRemoved) return;
#endif
        std::error_code ec; fs::remove_all(root, ec);
        traceWindowsStage("cleanup end " + root.string() + " error=" + ec.message());
    }
    void symlink(const fs::path& target, const fs::path& link, bool directory = false) {
        symlinks.push_back(link);
        createAuditSymlink(target, link, directory);
    }
    std::string write(const std::string& path, const std::string& data) {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << data; return (root / path).string();
    }
    std::string database(const std::vector<std::string>& args, const std::string& file = "main.cpp") {
        auto explicitArgs = args;
        if (!explicitArgs.empty() && explicitArgs.front() == "clang++") explicitArgs.insert(explicitArgs.begin() + 1, "--no-default-config");
        return write("compile_commands.json", json::array({{{"directory", root.string()}, {"file", file}, {"arguments", explicitArgs}}}).dump());
    }
};
}
TEST(CppCompileModel, PreservesOrderDirectoryAndMultipleVariants) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    json entries = json::array();
    for (const auto* define : {"-DA=1", "-DA=2"})
        entries.push_back({{"directory", f.root.string()}, {"file", "main.cpp"}, {"arguments", {"clang++", "-Ione", "-I", "two", define, "-UOLD", "-std=c++20", "-c", "main.cpp", "-o", "main.o", "--no-default-config"}}});
    auto model = loadCppCompileModel(f.write("compile_commands.json", entries.dump()), f.root.string());
    ASSERT_TRUE(model.complete()) << modelErrors(model); ASSERT_EQ(model.units.size(), 2u);
    EXPECT_NE(model.units[0].variant, model.units[1].variant);
    EXPECT_EQ(model.units[0].arguments[0], "-I" + (f.root / "one").string());
    EXPECT_EQ(model.units[0].arguments[1], "-I");
    EXPECT_EQ(model.units[0].arguments[2], (f.root / "two").string());
    EXPECT_NE(model.units[0].arguments, model.units[1].arguments);
}
TEST(CppCompileModel, CommandFormAndWindowsQuotingAreShellFree) {
    std::string error;
    auto posix = splitCppCommand("clang++ -I'path with spaces' -DVALUE=\\\"test\\\" main.cpp", false, error);
    ASSERT_TRUE(error.empty()); ASSERT_EQ(posix.size(), 4u); EXPECT_EQ(posix[1], "-Ipath with spaces");
    auto windows = splitCppCommand("\"C:\\Program Files\\LLVM\\clang++.exe\" -I\"C:\\path with spaces\" file.cpp", true, error);
    ASSERT_TRUE(error.empty()); ASSERT_EQ(windows.size(), 3u); EXPECT_EQ(windows[0], "C:\\Program Files\\LLVM\\clang++.exe");
    splitCppCommand("clang++ $(touch sentinel) main.cpp", false, error); EXPECT_FALSE(error.empty());
}
TEST(CppCompileModel, ResponseFilesExpandButCyclesAndPluginsFail) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    f.write("second.rsp", "-DFLAG=1"); f.write("first.rsp", "@second.rsp -I include");
    auto path = f.database({"clang++", "@first.rsp", "-c", "main.cpp"});
    auto model = loadCppCompileModel(path); ASSERT_TRUE(model.complete()) << modelErrors(model); EXPECT_GE(model.units[0].inputs.size(), 2u);
    f.write("second.rsp", "@first.rsp"); EXPECT_FALSE(loadCppCompileModel(path).complete());
    f.write("second.rsp", "-Xclang -load -Xclang /tmp/evil.dylib"); EXPECT_FALSE(loadCppCompileModel(path).complete());
}
TEST(CppCompileModel, PreservesSymlinkParentTraversalForModelSourceAndResponse) {
    Fixture f;
    f.write("left/main.cpp", "void wrong() {}\n");
    f.write("right/main.cpp", "void correct() {}\n");
    f.write("left/options.rsp", "-DSELECTED=11");
    f.write("right/options.rsp", "-DSELECTED=22");
    fs::create_directories(f.root / "right/nested");
    f.symlink("../right/nested", f.root / "left/link", true);
    const auto spelling = (f.root / "left/link/..").string();
    const auto actualMain = nativeCanonical(fs::path(spelling) / "main.cpp");
    const auto selectedDirectory = actualMain.parent_path();
    const bool selectsLeft = selectedDirectory == nativeCanonical(f.root / "left");
    auto path = f.database({"clang++", "@left/link/../options.rsp", "-Ileft/link/..", "-c", "left/link/../main.cpp"}, "left/link/../main.cpp");
    auto model = loadCppCompileModel(path, f.root.string());
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    EXPECT_EQ(model.units[0].file, spelling + "/main.cpp");
    EXPECT_NE(std::find(model.units[0].arguments.begin(), model.units[0].arguments.end(), selectsLeft ? "-DSELECTED=11" : "-DSELECTED=22"), model.units[0].arguments.end());
    EXPECT_NE(std::find(model.units[0].arguments.begin(), model.units[0].arguments.end(), "-I" + spelling), model.units[0].arguments.end());
    const auto selectedRelative = selectsLeft ? "left" : "right";
    const auto selectedSource = std::string(selectedRelative) + "/main.cpp";
    const auto selectedModel = f.write(std::string(selectedRelative) + "/compile_commands.json",
        json::array({{{"directory", f.root.string()}, {"file", selectedSource},
                     {"arguments", {"clang++", "--no-default-config", "-c", selectedSource}}}}).dump());
    f.write(std::string(selectsLeft ? "right" : "left") + "/compile_commands.json", "not the selected model");
    auto selected = loadCppCompileModel(spelling + "/compile_commands.json", f.root.string());
    ASSERT_TRUE(selected.complete());
    EXPECT_EQ(nativeCanonical(selected.path), nativeCanonical(selectedModel));
}
TEST(CppCompileModel, AuthorizesPhysicalTargetBeforeSymlinkParentTraversalReads) {
    Fixture f, outside;
    f.write("main.cpp", "void run() {}\n");
    f.write("inside/options.rsp", "-DWRONG=1");
    outside.write("options.rsp", "-DOUTSIDE=1");
    outside.write("nested/options.rsp", "-DOUTSIDE=1");
    f.symlink(outside.root / "nested", f.root / "inside/link", true);
    const auto compiler = topo::platform::findOnPath("clang++");
    ASSERT_FALSE(compiler.empty());
    CppCompileReadPolicy policy{{f.root.string()}, {cppCanonicalPath(compiler)}, true};
    // Win32 resolves '..' before traversing a link; POSIX traverses the link
    // first. Verify the OS-selected target, and retain direct escaping-link
    // cases so both platforms must enforce the external-input boundary.
    for (const auto& spelling : {std::string("inside/link/.."), std::string("inside/link")}) {
        for (const bool response : {false, true}) {
            const auto selectedInput = spelling + (response ? "/options.rsp" : "");
            const auto relativeTarget = nativeCanonical(f.root / selectedInput).lexically_relative(nativeCanonical(f.root));
            const bool needsGrant = relativeTarget.empty() || relativeTarget.is_absolute() || *relativeTarget.begin() == "..";
            const auto option = (response ? "@" : "-I") + selectedInput;
            SCOPED_TRACE(option);
            auto path = f.database({compiler, "--no-default-config", option, "-c", "main.cpp"});
            traceWindowsStage("read-policy model begin " + option);
            auto checked = loadCppCompileModel(path, f.root.string(), &policy);
            traceWindowsStage("read-policy model end " + option);
            EXPECT_EQ(checked.complete(), !needsGrant) << modelErrors(checked);
            ASSERT_EQ(checked.units.size(), 1u);
            if (needsGrant)
                EXPECT_FALSE(checked.units[0].inputDigests.count((f.root / selectedInput).string()));
            EXPECT_EQ(std::find(checked.units[0].arguments.begin(), checked.units[0].arguments.end(), "-DOUTSIDE=1"),
                      checked.units[0].arguments.end());
            policy.inputRoots.push_back(outside.root.string());
            traceWindowsStage("granted model begin " + option);
            auto granted = loadCppCompileModel(path, f.root.string(), &policy);
            traceWindowsStage("granted model end " + option);
            EXPECT_TRUE(granted.complete()) << modelErrors(granted);
            if (response) EXPECT_NE(std::find(granted.units[0].arguments.begin(), granted.units[0].arguments.end(),
                                             needsGrant ? "-DOUTSIDE=1" : "-DWRONG=1"), granted.units[0].arguments.end());
            policy.inputRoots.pop_back();
        }
    }
}
TEST(CppCompileModel, CompilerDriverSpellingFollowsHostFilenameCase) {
    Fixture f;
    f.write("main.cpp", "void run() {}\n");
    const auto compiler = f.write("toolchain/bin/clang++.EXE", "model input only; never executed");
    const auto path = f.database({compiler, "--no-default-config", "-c", "main.cpp"});
    const auto model = loadCppCompileModel(path, f.root.string());
#ifdef _WIN32
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    EXPECT_EQ(model.units[0].originalArguments.front(), compiler);
#else
    EXPECT_FALSE(model.complete());
    EXPECT_NE(modelErrors(model).find("unsupported compiler driver"), std::string::npos);
#endif
}
TEST(CppCompileModel, UnknownCompilerAndFlagsDoNotLoseSemanticsSilently) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    EXPECT_FALSE(loadCppCompileModel(f.database({"g++", "-c", "main.cpp"})).complete());
    EXPECT_FALSE(loadCppCompileModel(f.database({"clang++", "-fplugin=evil.so", "main.cpp"})).complete());
    EXPECT_FALSE(loadCppCompileModel(f.database({"clang++", "-include"})).complete());
}
TEST(CppCompileModel, BackendCodegenFlagsAreAcceptedWithoutAllowingFrontendPlugins) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    auto model = loadCppCompileModel(f.database({"clang++", "-emit-llvm", "-Xclang", "-disable-llvm-passes", "-fno-autolink", "-c", "main.cpp"}));
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    EXPECT_NE(std::find(model.units[0].arguments.begin(), model.units[0].arguments.end(), "-fno-autolink"), model.units[0].arguments.end());
    EXPECT_FALSE(loadCppCompileModel(f.database({"clang++", "-Xclang", "-load", "-Xclang", "plugin.dylib", "main.cpp"})).complete());
    EXPECT_FALSE(loadCppCompileModel(f.database({"clang++", "-Xclang", "-disable-llvm-passes=1", "main.cpp"})).complete());
    EXPECT_FALSE(loadCppCompileModel(f.database({"clang++", "-Xclang", "main.cpp"})).complete());
}
TEST(CppCompileModel, MissingCompilerAndImplicitConfigurationRemainUnverified) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    auto compiler = (f.root / "toolchain/bin/clang++").string();
    EXPECT_FALSE(loadCppCompileModel(f.database({compiler, "-c", "main.cpp"})).complete());
    f.write("toolchain/bin/clang++", "not executed");
    f.write("toolchain/etc/clang/unknown.cfg", "-DFLAG=1");
    EXPECT_FALSE(loadCppCompileModel(f.database({compiler, "-c", "main.cpp"})).complete());
    EXPECT_TRUE(loadCppCompileModel(f.database({compiler, "--no-default-config", "-c", "main.cpp"})).complete());
}

TEST(CppCompileModel, AuditReadPolicyRejectsModelSelectedCompilerAndExternalInputs) {
    Fixture f, external;
    f.write("main.cpp", "#include <value.h>\nvoid run() {}\n");
    auto compiler = topo::platform::findOnPath("clang++");
    ASSERT_FALSE(compiler.empty());
    CppCompileReadPolicy policy{{f.root.string()}, {cppCanonicalPath(compiler)}, true};
    auto path = f.database({compiler, "--no-default-config", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iheaders", "-c", "main.cpp"});
    external.write("value.h", "#pragma once\n");
    f.symlink(external.root, f.root / "headers", true);
    traceWindowsStage("external search-root model begin");
    EXPECT_FALSE(loadCppCompileModel(path, f.root.string(), &policy).complete());
    traceWindowsStage("external search-root model end");
    policy.inputRoots.push_back(external.root.string());
    traceWindowsStage("granted search-root model begin");
    auto model = loadCppCompileModel(path, f.root.string(), &policy);
    traceWindowsStage("granted search-root model end");
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    traceWindowsStage("granted search-root identity begin");
    EXPECT_TRUE(cppAuditInputIdentity(model, &policy)["supported"].get<bool>());
    traceWindowsStage("granted search-root identity end");
    const auto fake = external.write("clang++", "#!/bin/sh\nexit 0\n");
    path = f.database({fake, "--no-default-config", "-c", "main.cpp"});
    model = loadCppCompileModel(path, f.root.string(), &policy);
    ASSERT_FALSE(model.complete()); EXPECT_FALSE(model.units[0].inputDigests.count(fake));
    const auto response = external.write("outside.rsp", "-c main.cpp");
    path = f.database({compiler, "--no-default-config", "@" + response});
    policy.inputRoots = {f.root.string()};
    model = loadCppCompileModel(path, f.root.string(), &policy);
    ASSERT_FALSE(model.complete()); EXPECT_FALSE(model.units[0].inputDigests.count(response));
}

TEST(CppCompileModel, FixedAuditEnvironmentIgnoresAmbientHomeConfiguration) {
    Fixture f, ambient;
    f.write("main.cpp", "void run() {}\n");
    auto compiler = f.write("toolchain/bin/clang++", "model identity only; never executed");
    ambient.write(".config/clang/default.cfg", "-DAMBIENT=1\n");
    struct HomeRestore {
        std::optional<std::string> old;
        HomeRestore(const std::string& value) {
            if (const auto* current = std::getenv("HOME")) old = current;
            set(value.c_str());
        }
        static void set(const char* value) {
#ifdef _WIN32
            _putenv_s("HOME", value ? value : "");
#else
            if (value) setenv("HOME", value, 1); else unsetenv("HOME");
#endif
        }
        ~HomeRestore() { set(old ? old->c_str() : nullptr); }
    } home(ambient.root.string());
    const auto path = f.database({compiler, "-c", "main.cpp"});
    EXPECT_FALSE(loadCppCompileModel(path, f.root.string()).complete());
    CppCompileReadPolicy policy{{f.root.string()}, {cppCanonicalPath(compiler)}, true};
    EXPECT_TRUE(loadCppCompileModel(path, f.root.string(), &policy).complete());
}

TEST(CppAudit, UnconfiguredAuditDoesNotFallBackToFilesystemDiscovery) {
    Fixture project, external;
    project.write("main.cpp", "void run() {}\n");
    external.write("private.cpp", "void private_data() {}\n");
    auto provider = createCppAnalysisProvider();
    std::string diagnostic;
    provider->configureAuditExecution({}, diagnostic);
    provider->configureAnalysis(project.root.string(), "");
    EXPECT_TRUE(provider->collectSourceFiles(project.root.string(), {external.root.string()}).empty());
    EXPECT_TRUE(provider->auditFacts({}).modelPath.empty());
}

TEST(CppAudit, FailedAuditModelDoesNotPublishInputsOrSelectInvalidRows) {
    Fixture project, external;
    const auto source = external.write("private.cpp", "void private_data() {}\n");
    const auto model = external.database({"g++", "-c", source}, source);
    auto provider = createCppAnalysisProvider();
    std::string diagnostic;
    provider->configureAuditExecution({}, diagnostic);
    provider->configureAnalysis(project.root.string(), model);
    EXPECT_TRUE(provider->auditFacts({}).modelPath.empty());
    EXPECT_TRUE(provider->collectSourceFiles(project.root.string(), {external.root.string()}).empty());
    const auto local = project.write("compile_commands.json", json::array({{
        {"directory", project.root.string()}, {"file", source}, {"arguments", {"g++", "-c", source}}}}).dump());
    provider->configureAnalysis(project.root.string(), local);
    EXPECT_TRUE(provider->auditFacts({}).modelPath.empty());
    EXPECT_TRUE(provider->collectSourceFiles(project.root.string(), {}).empty());
}

TEST(CppAuditHeaderClosure, AuthorizedSearchRootsDoNotAuthorizeEscapingHeaderSymlinks) {
    Fixture f, external;
    f.write("main.cpp", "#include \"value.h\"\nvoid run() {}\n");
    const auto file = external.write("value.h", "#pragma once\n");
    f.symlink(file, f.root / "value.h");
    auto compiler = topo::platform::findOnPath("clang++");
    ASSERT_FALSE(compiler.empty());
    CppCompileReadPolicy policy{{f.root.string()}, {cppCanonicalPath(compiler)}, true};
    auto path = f.database({compiler, "--no-default-config", "-std=c++17", "-nostdinc", "-nostdinc++", "-c", "main.cpp"});
    traceWindowsStage("policy model begin " + path);
    const auto model = loadCppCompileModel(path, f.root.string(), &policy);
    traceWindowsStage("policy model end " + path);
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    traceWindowsStage("denied identity begin");
    EXPECT_FALSE(cppAuditInputIdentity(model, &policy)["supported"].get<bool>());
    traceWindowsStage("denied identity end");
    policy.inputRoots.push_back(external.root.string());
    traceWindowsStage("allowed identity begin");
    EXPECT_TRUE(cppAuditInputIdentity(model, &policy)["supported"].get<bool>());
    traceWindowsStage("allowed identity end");
}

TEST(CppAuditHeaderClosure, TracksTransitiveAndPreviouslyMissingHigherPriorityHeaders) {
    Fixture f;
    f.write("main.cpp", "#include <value.h>\nint run() { return VALUE; }\n");
    f.write("later/value.h", "#pragma once\n#include \"detail.h\"\n");
    f.write("later/detail.h", "#define VALUE 7\n");
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iearly", "-Ilater", "-c", "main.cpp"});
    auto identity = [&] {
        traceWindowsStage("identity begin " + path);
        auto result = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        traceWindowsStage("identity end " + path);
        return result;
    };
    auto first = identity(); ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    EXPECT_EQ(first.at("searchCandidates").at(nativeKey(f.root / "early/value.h")).at("kind"), "missing");
    EXPECT_TRUE(first["inputs"].contains(nativeKey(f.root / "later/detail.h")));
    f.write("later/detail.h", "#define VALUE 8\n"); EXPECT_NE(identity(), first);
    f.write("later/detail.h", "#define VALUE 7\n"); EXPECT_EQ(identity(), first);
    f.write("early/value.h", "#define VALUE 9\n"); EXPECT_NE(identity(), first);
    fs::remove(f.root / "early/value.h"); fs::remove(f.root / "early"); EXPECT_EQ(identity(), first);
    f.write("report.json", "unrelated output\n"); EXPECT_EQ(identity(), first);
}

TEST(CppAuditHeaderClosure, IncludesResponsesForcedHeadersAndQuoteOnlyRoots) {
    Fixture f;
    f.write("main.cpp", "#include \"quote.h\"\nint run() { return VALUE; }\n");
    f.write("quote/quote.h", "#pragma once\n");
    f.write("forced.h", "#include \"macro.h\"\n");
    f.write("macro.h", "#define VALUE 3\n");
    f.write("macros.h", "#define EXTRA 1\n");
    f.write("inner.rsp", "-std=c++17 -nostdinc -nostdinc++ -iquote quote -include forced.h -imacros macros.h");
    f.write("outer.rsp", "@inner.rsp -c main.cpp");
    auto path = f.database({"clang++", "@outer.rsp"});
    auto identity = [&] {
        traceWindowsStage("identity begin " + path);
        auto result = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        traceWindowsStage("identity end " + path);
        return result;
    };
    auto first = identity(); ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    for (const auto* name : {"outer.rsp", "inner.rsp", "forced.h", "macro.h", "macros.h", "quote/quote.h"})
        EXPECT_TRUE(containsInput(first, f.root / name)) << name;
    f.write("macros.h", "#define EXTRA 2\n"); EXPECT_NE(identity(), first);
    f.write("macros.h", "#define EXTRA 1\n"); EXPECT_EQ(identity(), first);
    f.write("inner.rsp", "-std=c++17 -nostdinc -nostdinc++ -iquote quote -include forced.h -imacros macros.h -DOTHER=1");
    EXPECT_NE(identity(), first);
}

TEST(CppAuditHeaderClosure, TracksIncludeNextAndLiteralNegativeProbes) {
    Fixture f;
    f.write("main.cpp", "#include <value.h>\n#if __has_include(<optional.h>)\n#include <optional.h>\n#endif\n");
    f.write("first/value.h", "#if __has_include_next(<value.h>)\n#include_next <value.h>\n#endif\n");
    f.write("second/value.h", "#define VALUE 1\n");
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Ifirst", "-Isecond", "-c", "main.cpp"});
    auto identity = [&] {
        traceWindowsStage("identity begin " + path);
        auto result = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        traceWindowsStage("identity end " + path);
        return result;
    };
    auto first = identity(); ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    EXPECT_TRUE(first["inputs"].contains(nativeKey(f.root / "second/value.h")));
    f.write("first/optional.h", "#define OPTIONAL 1\n"); EXPECT_NE(identity(), first);
}

TEST(CppAuditHeaderClosure, TracksSymlinkRetargetingAndMissingIntermediateParents) {
    Fixture f;
    f.write("main.cpp", "#include <value.h>\n#if __has_include(<nested/new.h>)\n#endif\n#if __has_include(<dangling/new.h>)\n#endif\n");
    f.write("a/value.h", "#define VALUE 1\n"); f.write("b/value.h", "#define VALUE 1\n");
    f.symlink(f.root / "a", f.root / "selected", true);
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iselected", "-c", "main.cpp"});
    auto identity = [&] {
        traceWindowsStage("identity begin " + path);
        auto result = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        traceWindowsStage("identity end " + path);
        return result;
    };
    auto first = identity(); ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    removeAuditSymlink(f.root / "selected"); f.symlink(f.root / "b", f.root / "selected", true);
    EXPECT_NE(identity(), first);
    auto second = identity();
    fs::create_directories(f.root / "empty"); f.symlink(f.root / "empty", f.root / "b/nested", true);
    EXPECT_NE(identity(), second);
    auto third = identity(); f.write("empty/new.h", "#pragma once\n"); EXPECT_NE(identity(), third);
    f.symlink(f.root / "absent-a", f.root / "b/dangling", true);
    const auto dangling = identity();
#ifdef _WIN32
    // Existing native reparse destinations participate in the identity;
    // dangling destinations stay explicitly uncached until their link-text
    // identity can be captured through a native Windows descriptor.
    ASSERT_FALSE(dangling["supported"].get<bool>()) << dangling.dump();
    EXPECT_NE(dangling.value("reason", "").find("unresolved reparse-point"), std::string::npos);
#else
    ASSERT_TRUE(dangling["supported"].get<bool>()) << dangling.dump();
#endif
    traceWindowsStage("remove dangling link begin");
    removeAuditSymlink(f.root / "b/dangling"); f.symlink(f.root / "absent-b", f.root / "b/dangling", true);
    traceWindowsStage("remove dangling link end");
#ifdef _WIN32
    const auto retargeted = identity();
    EXPECT_FALSE(retargeted["supported"].get<bool>()) << retargeted.dump();
    EXPECT_NE(retargeted.value("reason", "").find("unresolved reparse-point"), std::string::npos);
#else
    EXPECT_NE(identity(), dangling);
#endif
}
TEST(CppAuditHeaderClosure, PreservesSymlinkParentTraversalInLookupIdentity) {
    Fixture f;
    f.write("main.cpp", "#include <value.h>\nvoid run() {}\n");
    f.write("left/value.h", "#define SELECTED 11\n");
    f.write("right/value.h", "#define SELECTED 22\n");
    fs::create_directories(f.root / "right/nested");
    f.symlink("../right/nested", f.root / "left/link", true);
    const auto actual = nativeCanonical(f.root / "left/link/../value.h");
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Ileft/link/..", "-c", "main.cpp"});
    auto model = loadCppCompileModel(path, f.root.string());
    auto before = cppAuditInputIdentity(model);
    ASSERT_TRUE(before.value("supported", false)) << before.dump();
    bool found = false;
    for (const auto& [name, input] : before.at("inputs").items())
        if (input.value("resolvedPath", "") == actual.generic_string()) found = true;
    EXPECT_TRUE(found);
    std::ofstream(actual) << "#define SELECTED 33\n";
    EXPECT_NE(before, cppAuditInputIdentity(model));
}

TEST(CppAuditHeaderClosure, BypassesDynamicAndUnclosedPreprocessorInputs) {
    Fixture f;
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iinclude", "-c", "main.cpp"});
    for (const auto& source : std::vector<std::string>{
             "#define JOIN(a,b) a ## b\nconst char* t = JOIN(__TI, ME__);\n",
             "const char* t = __TI\\\nME__;\n", "#include HEADER\n", "#include \"../escape.h\"\n",
             "#if __has_include(HEADER)\n#endif\n", "#define FOUND(x) __has_include(<x>)\n",
             "#pragma clang module load Whatever\n", "%:include \"a.h\"\n", std::string("?") + "?=include \"a.h\"\n",
             "#include <a/*x*/b.h>\n", std::string("\xef\xbb\xbf") + "#include <value.h>\n",
             "#if __has_embed(\"data\")\n#endif\n", "#__include_macros \"hidden.h\"\n", "#module Whatever\n"}) {
        f.write("main.cpp", source);
        EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>()) << source;
    }
    f.write("main.cpp", "#include <bad.h>\n"); f.write("include/bad.h", "const char* t = __DATE__;\n");
    EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>());
    f.write("include/bad.h", "#pragma once\n");
    EXPECT_TRUE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>());
    fs::create_hard_link(f.root / "include/bad.h", f.root / "same-header.h");
#ifdef _WIN32
    const auto nativeHeader = (f.root / "include/bad.h").make_preferred();
    const HANDLE handle = CreateFileW(nativeHeader.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    ASSERT_TRUE(inspected);
    EXPECT_EQ(information.nNumberOfLinks, 2u);
    traceWindowsStage("hardlink counts native=" + std::to_string(information.nNumberOfLinks) +
                      " stdlib=" + std::to_string(fs::hard_link_count(nativeHeader)));
#endif
    EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>());
}
TEST(CppAuditMacroHeaderClosure, CapturesAllBranchAliasesAndUseSiteSearchCandidates) {
    Fixture f; fs::create_directories(f.root / "early");
    f.write("main.cpp", "#include \"defs/headers.h\"\n#include HEADER\n#if __has_include(OPTIONAL)\n#include OPTIONAL\n#endif\n");
    f.write("defs/headers.h", "#define HEADER ALIAS\n#if CHOICE\n#define ALIAS LEFT\n#else\n#define ALIAS RIGHT\n#endif\n#define LEFT \"value.h\"\n#define RIGHT \"other.h\"\n#define OPTIONAL \"optional.h\"\n");
    f.write("late/value.h", "#define TRANSITIVE \"detail.h\"\n#include TRANSITIVE\n");
    f.write("late/detail.h", "#define VALUE 1\n"); f.write("late/other.h", "#define VALUE 2\n");
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iearly", "-Ilate", "-c", "main.cpp"});
    auto identity = [&] {
        traceWindowsStage("identity begin " + path);
        auto result = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        traceWindowsStage("identity end " + path);
        return result;
    };
    const auto first = identity(); ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    for (const auto* name : {"defs/headers.h", "late/value.h", "late/detail.h", "late/other.h"})
        EXPECT_TRUE(containsInput(first, f.root / name)) << name;
    for (const auto* name : {"value.h", "early/value.h", "early/optional.h", "late/optional.h"})
        EXPECT_EQ(first.at("searchCandidates").at(nativeKey(f.root / name)).at("kind"), "missing") << name;
    EXPECT_FALSE(first["searchCandidates"].contains(nativeKey(f.root / "defs/value.h")));
    f.write("early/optional.h", "#pragma once\n"); EXPECT_NE(identity(), first);
    fs::remove(f.root / "early/optional.h"); EXPECT_EQ(identity(), first);
    f.write("early/value.h", "#define VALUE 3\n"); EXPECT_NE(identity(), first);
    fs::remove(f.root / "early/value.h"); EXPECT_EQ(identity(), first);
    f.write("late/detail.h", "#define VALUE 4\n"); EXPECT_NE(identity(), first);
}
TEST(CppAuditMacroHeaderClosure, ResolvesForcedHeadersAfterOrderedCommandLineDefinitions) {
    Fixture f;
    f.write("main.cpp", "#include HEADER\n");
    f.write("macros.h", "#define SELECTED \"selected.h\"\n");
    f.write("forced.h", "#define HEADER SELECTED\n");
    f.write("selected.h", "#pragma once\n"); f.write("unused.h", "#pragma once\n");
    auto flags = std::vector<std::string>{"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-DSELECTED=\"unused.h\"", "-U", "SELECTED", "-include", "forced.h", "-imacros", "macros.h", "-c", "main.cpp"};
    auto path = f.database(flags);
    auto first = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
    ASSERT_TRUE(first["supported"].get<bool>()) << first.dump();
    EXPECT_TRUE(first["inputs"].contains((f.root / "selected.h").string()));
    EXPECT_FALSE(first["inputs"].contains((f.root / "unused.h").string()));
    f.write("main.cpp", "#include COMMAND\n");
    flags.insert(flags.end(), {"-D", "COMMAND=\"unused.h\"", "-UCOMMAND", "-DCOMMAND=\"selected.h\""});
    auto final = cppAuditInputIdentity(loadCppCompileModel(f.database(flags), f.root.string()));
    ASSERT_TRUE(final["supported"].get<bool>()) << final.dump();
    EXPECT_TRUE(final["inputs"].contains((f.root / "selected.h").string()));
    EXPECT_FALSE(final["inputs"].contains((f.root / "unused.h").string()));
    flags.push_back("-UCOMMAND");
    EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(f.database(flags), f.root.string()))["supported"].get<bool>());
}
TEST(CppAuditMacroHeaderClosure, KeepsUnclosedMacroFormsUncached) {
    Fixture f; f.write("value.h", "#pragma once\n");
    const auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-c", "main.cpp"});
    for (const auto& source : std::vector<std::string>{
        "#include UNKNOWN\n", "#define A B\n#define B A\n#include A\n",
        "#define H(x) x\n#include H(\"value.h\")\n",
        "#define H <value.h>\n#include H\n",
        "#if PICK\n#define H \"value.h\"\n#else\n#define H <value.h>\n#endif\n#include H\n",
        "#define H \"value.h\"\n#undef H\n#define H \"value.h\"\n#include H\n",
        "#define H \"value\" \".h\"\n#include H\n",
        "#define H __FILE__\n#include H\n",
        "#undef __has_include\n#if 0\n#endif\n",
        "void run(){\n#undef __has_include\n(\"value.h\");\n}\n",
        "void run(){\n#undef __has_include_next\n(\"value.h\");\n}\n",
        "#define __has_include(x) 1\n#if __has_include(\"value.h\")\n#endif\n",
        "#define P __has_include\n#if P(\"value.h\")\n#endif\n",
        "#define H \"value.h\"\n#define JOIN(a,b) a ## b\n#include H\n"}) {
        f.write("main.cpp", source);
        EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>()) << source;
    }
    f.write("main.cpp", "#define HEADER \"value.h\"\n#define SUM(a,b) a+b\n#include HEADER\n");
    EXPECT_TRUE(cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()))["supported"].get<bool>());
    for (const auto& flag : {"-D__has_include(x)=0", "-U__has_include", "-D__has_include_next(x)=0", "-U__has_include_next"}) {
        auto command = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", flag, "-c", "main.cpp"});
        EXPECT_FALSE(cppAuditInputIdentity(loadCppCompileModel(command, f.root.string()))["supported"].get<bool>()) << flag;
    }
}
TEST(CppAuditMacroHeaderClosure, EnforcesAuthorizedRootsForExpandedCandidates) {
    Fixture f, external;
    f.write("main.cpp", "#define HEADER \"selected.h\"\n#include HEADER\n");
    auto target = external.write("private.h", "#pragma once\n");
    f.symlink(target, f.root / "selected.h");
    const auto compiler = topo::platform::findOnPath("clang++"); ASSERT_FALSE(compiler.empty());
    CppCompileReadPolicy policy{{f.root.string()}, {cppCanonicalPath(compiler)}, true};
    auto path = f.database({compiler, "--no-default-config", "-std=c++17", "-nostdinc", "-nostdinc++", "-c", "main.cpp"});
    traceWindowsStage("macro model begin");
    const auto model = loadCppCompileModel(path, f.root.string(), &policy); ASSERT_TRUE(model.complete()) << modelErrors(model);
    traceWindowsStage("macro model end");
    traceWindowsStage("macro denied identity begin");
    auto denied = cppAuditInputIdentity(model, &policy);
    traceWindowsStage("macro denied identity end");
    EXPECT_FALSE(denied["supported"].get<bool>());
    EXPECT_NE(denied.value("reason", "").find("outside"), std::string::npos);
    policy.inputRoots.push_back(external.root.string());
    traceWindowsStage("allowed identity begin");
    EXPECT_TRUE(cppAuditInputIdentity(model, &policy)["supported"].get<bool>());
    traceWindowsStage("allowed identity end");
}
TEST(CppAuditMacroHeaderClosure, BoundsMacroNamesAlternativesAliasDepthAndClosureRounds) {
    Fixture f;
    const auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-c", "main.cpp"});
    auto exceeds = [&](const std::string& source, const std::string& reason) {
        f.write("main.cpp", source);
        auto identity = cppAuditInputIdentity(loadCppCompileModel(path, f.root.string()));
        EXPECT_FALSE(identity["supported"].get<bool>()) << identity.dump();
        EXPECT_NE(identity.value("reason", "").find(reason), std::string::npos) << identity.dump();
    };
    std::string names;
    for (int i = 0; i < 4097; ++i) names += "#define H" + std::to_string(i) + " 1\n";
    exceeds(names, "definition limit");
    std::string alternatives;
    for (int i = 0; i < 65; ++i) alternatives += "#define H \"" + std::to_string(i) + ".h\"\n";
    exceeds(alternatives + "#include H\n", "definition limit");
    std::string aliases;
    for (int i = 0; i < 65; ++i) aliases += "#define H" + std::to_string(i) + " H" + std::to_string(i + 1) + "\n";
    exceeds(aliases + "#define H65 \"leaf.h\"\n#include H0\n", "expansion limit");
    std::string expanded = "#define H \"leaf.h\"\n";
    for (int i = 0; i < 18; ++i) expanded += "#define A" + std::to_string(i) + " " + (i == 17 ? "H" : "A" + std::to_string(i + 1)) + "\n";
    for (int i = 0; i < 14000; ++i) expanded += "#if __has_include(A0)\n#endif\n";
    exceeds(expanded, "expansion limit");
    // A small alias DAG can repeat a long token many times without reaching
    // the node-count limit. Bound expanded text before materializing it all.
    std::string amplified;
    for (int i = 0; i < 17; ++i) {
        for (const auto* name : {"A", "B"}) {
            const auto macro = std::string(name) + std::to_string(i);
            amplified += "#define " + macro + " A" + std::to_string(i + 1) + "\n";
            amplified += "#define " + macro + " B" + std::to_string(i + 1) + "\n";
        }
    }
    const auto longHeader = std::string(1024, 'h') + ".h";
    amplified += "#define A17 \"" + longHeader + "\"\n#define B17 \"" + longHeader + "\"\n#include A0\n";
    exceeds(amplified, "expansion text limit");
    for (int i = 0; i < 130; ++i)
        f.write(std::to_string(i) + ".h", i == 129 ? "#pragma once\n" : "#include \"" + std::to_string(i + 1) + ".h\"\n");
    exceeds("#include \"0.h\"\n", "round limit");
}

TEST(CppAuditHeaderClosure, NormalizesPhysicalLineEndingsBeforeSplicing) {
    Fixture f;
    f.write("value.h", "#pragma once\nint selected;\n");
    const auto database = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-c", "main.cpp"});
    unsigned revision = 0;
    for (const std::string newline : {"\n", "\r\n", "\r"}) {
        SCOPED_TRACE(json(newline).dump());
        const auto text = "#define HEADER \\" + newline + "\"value.h\"" + newline +
                          "#include HEADER" + newline + "int value;" + newline;
        { std::ofstream file(f.root / "main.cpp", std::ios::binary); file << text; }
        const auto before = cppAuditInputIdentity(loadCppCompileModel(database, f.root.string()));
        ASSERT_TRUE(before["supported"].get<bool>()) << before.dump();
        f.write("value.h", "#pragma once\nint selected;\n// changed " + std::to_string(++revision));
        const auto after = cppAuditInputIdentity(loadCppCompileModel(database, f.root.string()));
        ASSERT_TRUE(after["supported"].get<bool>()) << after.dump();
        EXPECT_NE(before, after);
    }
}

TEST(CppPreprocessor, DecodesEscapedLineMarkerFilenamesFromRealClang) {
    const auto clang = topo::platform::findOnPath("clang++");
    ASSERT_FALSE(clang.empty());
    Fixture f;
    for (const std::string& newline : {std::string("\n"), std::string("\r\n")}) {
        SCOPED_TRACE("source newline " + json(newline).dump());
        for (const std::string& expected : {std::string(R"(C:\audit\quoted"unit.cpp)"), std::string("tab\tfile.cpp")}) {
            SCOPED_TRACE(json(expected).dump());
            const auto source = (f.root / "main.cpp").string();
            {
                std::ofstream file(source, std::ios::binary);
                file << "#line 42 " << json(expected).dump() << newline
                     << "void run() { system(\"x\"); }" << newline;
            }
#ifdef _WIN32
            // Independently verify that the native compiler supplies the CRLF
            // stream this scanner must map, including the escaped #line name.
            const auto raw = topo::platform::runProcessCaptureWithTimeout(
                clang, {"-E", "-x", "c++", "-std=c++17", source}, 20000);
            ASSERT_EQ(raw.exitCode, 0) << raw.stderrOutput;
            ASSERT_NE(raw.stdoutOutput.find("\r\n"), std::string::npos);
#endif
            CppPreprocessorScanner scanner; scanner.setClangPath(clang);
            const auto result = scanner.scanDetailed(source);
            ASSERT_TRUE(result.complete) << (result.errors.empty() ? "" : result.errors.front());
            ASSERT_FALSE(result.sites.empty());
            EXPECT_EQ(result.sites.front().file, expected);
            EXPECT_EQ(result.sites.front().line, 42);
        }
    }
}

TEST(CppPreprocessor, UsesCompileDatabaseMacrosAndReportsRealFailure) {
    auto clang = topo::platform::findOnPath("clang++");
    if (clang.empty()) GTEST_SKIP() << "clang++ unavailable";
    Fixture f;
    auto source = f.write("main.cpp", "void run() { DANGER(\"x\"); }\n");
    f.database({"clang++", "-DDANGER=system", "-c", "main.cpp"});
    CppPreprocessorScanner scanner; scanner.setBuildDir(f.root.string()); scanner.setClangPath(clang);
    auto result = scanner.scanDetailed(source);
    ASSERT_TRUE(result.complete) << (result.errors.empty() ? "" : result.errors.front());
    ASSERT_FALSE(result.sites.empty()); EXPECT_EQ(result.sites.front().calleePattern, "system");
    EXPECT_EQ(nativeCanonical(result.sites.front().file), nativeCanonical(source));
    EXPECT_EQ(result.sites.front().callerQualifiedName.find("[macro-expanded]"), std::string::npos);
    f.write("main.cpp", "#include \"missing.h\"\nvoid run() {}\n");
    result = scanner.scanDetailed(source); EXPECT_FALSE(result.complete); EXPECT_FALSE(result.errors.empty());
    EXPECT_THROW(scanner.scanExpanded(source), std::runtime_error);
}
#ifdef TOPO_CPP_AUDIT_LIBCLANG
#ifndef _WIN32
TEST(CppAudit, PreventsActualNativeGpuHelperExecution) {
    Fixture f; f.write("main.cpp", "int run() { return 7; }\n");
    const auto compiler = f.write("toolchain/clang++", "driver identity only; never executed\n");
    const auto marker = f.root / "helper-executed";
    // Use a sibling marker, avoiding shell interpolation of the temporary path.
    const auto helper = f.write("toolchain/amdgpu-arch",
        "#!/bin/sh\ndir=${0%/*}\n: > \"$dir/../helper-executed\"\nprintf 'gfx906\\n'\n");
    fs::permissions(helper, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
    const auto path = f.database({compiler, "--no-default-config", "--target=amdgcn-amd-amdhsa", "-mcpu=native", "main.cpp"});
    const auto model = loadCppCompileModel(path, f.root.string());
    ASSERT_TRUE(model.complete()) << modelErrors(model);
    const auto& unit = model.units.front();
    auto args = unit.arguments; args.insert(args.begin(), compiler);
    std::vector<const char*> argv; for (const auto& arg : args) argv.push_back(arg.c_str());
    CXIndex index = clang_createIndex(0, 0); CXTranslationUnit tu = nullptr;
    clang_parseTranslationUnit2FullArgv(index, unit.file.c_str(), argv.data(),
        static_cast<int>(argv.size()), nullptr, 0, 0, &tu);
    if (tu) clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    ASSERT_TRUE(fs::exists(marker)) << "raw Clang control must execute the discovery helper";
    fs::remove(marker);
    const auto facts = auditCppCompileModel(path, f.root.string());
    EXPECT_FALSE(fs::exists(marker));
    EXPECT_FALSE(facts["complete"].get<bool>());
    EXPECT_TRUE(facts["identities"].empty());
    EXPECT_NE(facts["unresolved"].dump().find("before frontend invocation"), std::string::npos);
}
#endif

TEST(CppAudit, RejectsGpuDiscoveryBeforeParsingAndExpandsResponseOrder) {
    Fixture f; f.write("main.cpp", "int run() { return 7; }\n");
    f.write("gpu.rsp", "--target=amdgcn-amd-amdhsa -mcpu=gfx906 -mcpu=native");
    const auto path = f.database({"clang++", "@gpu.rsp", "main.cpp"});
    const auto model = loadCppCompileModel(path, f.root.string());
    ASSERT_TRUE(model.complete()) << modelErrors(model); ASSERT_EQ(model.units.size(), 1u);
    EXPECT_FALSE(cppAuditDriverRejection(model.units[0]).empty());
    const auto facts = auditCppCompileModel(path, f.root.string());
    EXPECT_FALSE(facts["complete"].get<bool>());
    EXPECT_TRUE(facts["identities"].empty());
    EXPECT_NE(facts["unresolved"].dump().find("before frontend invocation"), std::string::npos);
    EXPECT_EQ(facts["unresolved"].dump().find("cannot parse translation unit"), std::string::npos);
    EXPECT_EQ(facts["unresolved"].dump().find("posix_spawn"), std::string::npos);
    f.write("gpu.rsp", "--target=amdgcn-amd-amdhsa -mcpu=native -mcpu=gfx906");
    const auto explicitModel = loadCppCompileModel(path, f.root.string());
    ASSERT_TRUE(explicitModel.complete());
    EXPECT_TRUE(cppAuditDriverRejection(explicitModel.units[0]).empty());
}

TEST(CppAudit, SearchAndParsedInputPathsFollowPhysicalSymlinkTraversal) {
    Fixture f;
    f.write("left/value.h", "inline int selected_left(){return 11;}\n#define selected selected_left\n");
    auto actual = f.write("right/value.h", "inline int selected_right(){return 22;}\n#define selected selected_right\n");
    fs::create_directories(f.root / "right/nested");
    f.symlink("../right/nested", f.root / "left/link", true);
    for (const auto& include : {std::string("<value.h>"), std::string("\"left/link/../value.h\"")}) {
        f.write("main.cpp", "#include " + include + "\nint run(){return selected();}\n");
        const auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Ileft/link/..", "-c", "main.cpp"});
        auto facts = auditCppCompileModel(path, f.root.string());
        EXPECT_TRUE(facts.value("complete", false)) << facts.at("unresolved").dump();
        bool correct = false, wrong = false;
        for (const auto& item : facts.at("identities")) {
            if (item.at("name") == "selected_right") { correct = true; EXPECT_EQ(item.at("file"), actual); }
            if (item.at("name") == "selected_left") wrong = true;
        }
        EXPECT_TRUE(correct); EXPECT_FALSE(wrong);
        for (const auto& file : facts.at("inputs")) {
            if (fs::path(file.get<std::string>()).extension() == ".h")
                EXPECT_EQ(cppCanonicalPath(file.get<std::string>()), actual);
        }
    }
}
namespace {
long long parsedObservedValue(const CppCompileModel& model) {
    auto index = clang_createIndex(0, 0);
    auto args = model.units.at(0).arguments;
    args.insert(args.begin(), model.units.at(0).compiler);
    std::vector<const char*> argv;
    for (const auto& arg : args) argv.push_back(arg.c_str());
    CXTranslationUnit tu = nullptr;
    const auto status = clang_parseTranslationUnit2FullArgv(index, model.units[0].file.c_str(), argv.data(),
        static_cast<int>(argv.size()), nullptr, 0, 0, &tu);
    EXPECT_EQ(status, CXError_Success);
    long long value = -1;
    if (tu) {
        for (unsigned i = 0; i < clang_getNumDiagnostics(tu); ++i) {
            auto diag = clang_getDiagnostic(tu, i);
            EXPECT_LT(clang_getDiagnosticSeverity(diag), CXDiagnostic_Error);
            clang_disposeDiagnostic(diag);
        }
        clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor cursor, CXCursor, CXClientData data) {
            auto name = clang_getCursorSpelling(cursor);
            const bool selected = std::string(clang_getCString(name)) == "observed";
            clang_disposeString(name);
            if (selected) {
                auto evaluated = clang_Cursor_Evaluate(cursor);
                if (evaluated) {
                    *static_cast<long long*>(data) = clang_EvalResult_getAsLongLong(evaluated);
                    clang_EvalResult_dispose(evaluated);
                }
            }
            return CXChildVisit_Recurse;
        }, &value);
        clang_disposeTranslationUnit(tu);
    }
    clang_disposeIndex(index);
    return value;
}
}
TEST(CppAuditMacroHeaderClosure, RealClangInputsAndValuesFollowMacroChanges) {
    Fixture f;
    f.write("main.cpp", "#include HEADER\n#ifndef BONUS\n#define BONUS 0\n#endif\n#if __has_include(OPTIONAL)\n#include OPTIONAL\n#endif\nconstexpr int observed = VALUE + BONUS;\n");
    f.write("defs.h", "#define HEADER ALIAS\n#if CHOICE\n#define ALIAS \"value.h\"\n#else\n#define ALIAS \"other.h\"\n#endif\n#define OPTIONAL \"optional.h\"\n");
    f.write("late/value.h", "#define DETAIL \"detail.h\"\n#include DETAIL\n");
    f.write("late/detail.h", "#define VALUE 7\n"); f.write("late/other.h", "#define VALUE 20\n");
    auto flags = std::vector<std::string>{"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-Iearly", "-Ilate", "-imacros", "defs.h", "-DCHOICE=1", "-c", "main.cpp"};
    auto path = f.database(flags); json previous;
    auto observe = [&](long long expected) {
        const auto model = loadCppCompileModel(path, f.root.string());
        const auto identity = cppAuditInputIdentity(model);
        ASSERT_TRUE(identity["supported"].get<bool>()) << identity.dump();
        EXPECT_NE(identity, previous); previous = identity;
        EXPECT_EQ(parsedObservedValue(model), expected);
        const auto facts = auditCppCompileModel(path, f.root.string());
        EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
        for (const auto& [input, digest] : facts["inputDigests"].items()) {
            ASSERT_TRUE(identity["inputs"].contains(input)) << input;
            EXPECT_EQ(identity["inputs"][input]["sha256"], digest);
        }
    };
    observe(7);
    f.write("late/detail.h", "#define VALUE 8\n"); observe(8);
    f.write("early/value.h", "#define VALUE 9\n"); observe(9);
    f.write("early/optional.h", "#undef BONUS\n#define BONUS 100\n"); observe(109);
    flags.insert(flags.end(), {"-UCHOICE", "-D", "CHOICE=0"}); path = f.database(flags); observe(120);
    f.write("defs.h", "#define HEADER ALIAS\n#define ALIAS \"value.h\"\n#define OPTIONAL \"optional.h\"\n"); observe(109);
    fs::remove(f.root / "early/value.h"); observe(108);
}
TEST(CppAuditMacroHeaderClosure, RealClangCommandAndForcedMacroOrderMatchesClosedInputs) {
    Fixture f;
    f.write("main.cpp", "#include HEADER\nconstexpr int observed = VALUE;\n");
    f.write("macros.h", "#define SELECTED \"value.h\"\n");
    f.write("forced.h", "#define HEADER SELECTED\n");
    f.write("value.h", "#define VALUE 42\n");
    auto path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-include", "forced.h", "-DSELECTED=\"wrong.h\"", "-USELECTED", "-imacros", "macros.h", "-c", "main.cpp"});
    auto model = loadCppCompileModel(path, f.root.string()); auto identity = cppAuditInputIdentity(model);
    ASSERT_TRUE(identity["supported"].get<bool>()) << identity.dump();
    EXPECT_EQ(parsedObservedValue(model), 42);
    EXPECT_TRUE(identity["inputs"].contains((f.root / "value.h").string()));
    path = f.database({"clang++", "-std=c++17", "-nostdinc", "-nostdinc++", "-D", "HEADER=\"wrong.h\"", "-UHEADER", "-DHEADER=\"value.h\"", "-c", "main.cpp"});
    model = loadCppCompileModel(path, f.root.string()); identity = cppAuditInputIdentity(model);
    ASSERT_TRUE(identity["supported"].get<bool>()) << identity.dump();
    EXPECT_EQ(parsedObservedValue(model), 42);
    EXPECT_FALSE(identity["searchCandidates"].contains((f.root / "wrong.h").string()));
}

TEST(CppAuditHeaderClosure, RealClangValuesFollowNegativeNextForcedResponseAndSymlinkChanges) {
    Fixture f;
    f.write("main.cpp", "#include <value.h>\n#ifndef BONUS\n#define BONUS 0\n#endif\n#if __has_include(<optional.h>)\n#include <optional.h>\n#endif\nconstexpr int observed = VALUE + BONUS + FORCED;\n");
    f.write("first/value.h", "#if __has_include_next(<value.h>)\n#include_next <value.h>\n#endif\n");
    f.write("second/value.h", "#include \"detail.h\"\n"); f.write("second/detail.h", "#define VALUE 7\n");
    f.write("forced.h", "#define FORCED 1\n");
    f.symlink(f.root / "second", f.root / "selected", true);
    f.write("args.rsp", "-std=c++17 -nostdinc -nostdinc++ -Iearly -Ifirst -Iselected -include forced.h");
    auto path = f.database({"clang++", "@args.rsp", "-c", "main.cpp"});
    json previous;
    auto observe = [&](long long expected) {
        auto model = loadCppCompileModel(path, f.root.string());
        auto identity = cppAuditInputIdentity(model);
        ASSERT_TRUE(identity["supported"].get<bool>()) << identity.dump();
        EXPECT_NE(identity, previous); previous = identity;
        EXPECT_EQ(parsedObservedValue(model), expected);
        auto facts = auditCppCompileModel(path, f.root.string());
        EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
        for (const auto& [input, digest] : facts["inputDigests"].items()) {
            ASSERT_TRUE(identity["inputs"].contains(input)) << input;
            EXPECT_EQ(identity["inputs"][input]["sha256"], digest);
        }
    };
    observe(8);
    f.write("second/detail.h", "#define VALUE 8\n"); observe(9);
    f.write("early/value.h", "#define VALUE 20\n"); observe(21);
    f.write("early/optional.h", "#undef BONUS\n#define BONUS 100\n"); observe(121);
    f.write("forced.h", "#define FORCED 2\n"); observe(122);
    f.write("args.rsp", "-std=c++17 -nostdinc -nostdinc++ -Iselected -Iearly -Ifirst -include forced.h"); observe(110);
    f.write("third/value.h", "#define VALUE 30\n");
    removeAuditSymlink(f.root / "selected"); f.symlink(f.root / "third", f.root / "selected", true); observe(132);
}

TEST(CppAudit, BuiltinsDoNotCreatePhantomDependenciesAndSystemInputsHaveIdentity) {
    Fixture f;
    f.write("main.cpp", "namespace app { int value() { return 42; } }\n");
    auto path = f.database({"clang++", "-std=c++17", "-g", "-c", "main.cpp"});
    auto facts = auditCppCompileModel(path, f.root.string());
    EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
    EXPECT_TRUE(facts["dependencies"].empty());
    const auto headers = fs::path(f.root.string() + "-system");
    fs::create_directories(headers);
    std::ofstream(headers / "value.h") << "inline int system_value() { return 7; }\n";
    f.write("main.cpp", "#include <value.h>\nnamespace app { int value() { return system_value(); } }\n");
    path = f.database({"clang++", "-std=c++17", "-isystem", headers.string(), "-c", "main.cpp"});
    facts = auditCppCompileModel(path, f.root.string());
    EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
    ASSERT_EQ(facts["dependencies"].size(), 1u);
    const auto digest = facts["dependencies"][0]["digest"].get<std::string>();
    EXPECT_EQ(digest.size(), 64u);
    std::ofstream(headers / "value.h") << "inline int system_value() { return 8; }\n";
    EXPECT_NE(auditCppCompileModel(path, f.root.string())["dependencies"][0]["digest"], digest);
    fs::remove_all(headers);
}
TEST(CppAudit, DistinguishesSameNamedClassesAndBindsOutOfLineDefinition) {
    Fixture f;
    f.write("log.h", "namespace real { class Log { public: void write(); }; } namespace fake { class Log { public: void write() {} }; }\n");
    f.write("main.cpp", "#include \"log.h\"\nvoid real::Log::write() {}\nvoid run() { real::Log value; value.write(); }\n");
    auto model = f.database({"clang++", "-std=c++17", "-c", "main.cpp"});
    auto facts = auditCppCompileModel(model, f.root.string());
    EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
    std::string target;
    for (const auto& id : facts["identities"]) if (id["name"] == "real::Log::write") {
        target = id["id"]; EXPECT_EQ(id["file"], (f.root / "main.cpp").string()); EXPECT_FALSE(id["owner"].get<std::string>().empty());
    }
    ASSERT_FALSE(target.empty());
    bool found = false; for (const auto& ref : facts["references"]) if (ref["target"] == target) { found = true; EXPECT_TRUE(ref["runtimeTargetsComplete"].get<bool>()); }
    EXPECT_TRUE(found); EXPECT_GE(facts["inputDigests"].size(), 3u);
}
TEST(CppAudit, VirtualAndFunctionPointerCallsStayUnverified) {
    Fixture f;
    f.write("main.cpp", "struct Base { virtual void run() {} }; void direct(Base& b, void (*fn)()) { b.run(); fn(); }\n");
    auto facts = auditCppCompileModel(f.database({"clang++", "-std=c++17", "-c", "main.cpp"}), f.root.string());
    EXPECT_TRUE(facts["complete"].get<bool>()) << facts["unresolved"].dump();
    int unresolved = 0; for (const auto& ref : facts["references"]) if (!ref["runtimeTargetsComplete"].get<bool>()) ++unresolved;
    EXPECT_EQ(unresolved, 2);
}
TEST(CppAudit, ProviderUsesTheSameSemanticSnapshotForChecks) {
    Fixture f; f.write("main.cpp", "namespace app { class Log { public: void write() {} private: void hidden() {} }; void run() { Log log; log.write(); } }\n");
    auto model = f.database({"clang++", "-std=c++17", "-c", "main.cpp"});
    auto provider = createCppAnalysisProvider(); provider->configureAnalysis(f.root.string(), model);
    auto facts = provider->auditFacts({}); ASSERT_TRUE(facts.supported);
    EXPECT_TRUE(facts.complete) << json(facts.unresolved).dump();
    auto files = provider->collectSourceFiles(f.root.string(), {}); ASSERT_EQ(files.size(), 1u);
    auto symbols = provider->createSymbolExtractor()->extractSymbols(files[0]);
    bool hidden = false;
    for (const auto& symbol : symbols) if (symbol.qualifiedName == "app::Log::hidden") { hidden = true; ASSERT_TRUE(symbol.hostVisibility); EXPECT_EQ(*symbol.hostVisibility, topo::Visibility::Private); }
    EXPECT_TRUE(hidden);
}
#endif

TEST(CppCMakeModel, RequiresConfiguredReplyWithoutExecutingProjectScripts) {
    Fixture f;
    f.write("CMakeLists.txt", "message(FATAL_ERROR unexpected execution)\n");
    auto model = readCppCMakeModel((f.root / "build").string(), f.root.string());
    EXPECT_FALSE(model.present); ASSERT_EQ(model.unresolved.size(), 1u);
    EXPECT_NE(model.unresolved.front().find("File API"), std::string::npos);
}
TEST(CppCMakeModel, SourceAndConfigMetadataRemainExplicit) {
    Fixture f; f.write("main.cpp", "int main() {}\n"); f.write("CMakeLists.txt", "project(test)\n");
    auto base = std::string("build/.cmake/api/v1/reply/");
    f.write(base + "index-1.json", json{{"objects", json::array({{{"kind", "codemodel"}, {"jsonFile", "model.json"}}, {{"kind", "cmakeFiles"}, {"jsonFile", "files.json"}}})}}.dump());
    f.write(base + "model.json", json{{"paths", {{"source", f.root.string()}, {"build", (f.root / "build").string()}}}, {"configurations", json::array({{{"targets", json::array({{{"jsonFile", "target.json"}}})}}})}}.dump());
    f.write(base + "files.json", json{{"inputs", json::array({{{"path", "CMakeLists.txt"}}})}}.dump());
    f.write(base + "target.json", json{{"sources", json::array({{{"path", "main.cpp"}}})}}.dump());
    auto model = readCppCMakeModel((f.root / "build").string(), f.root.string());
    EXPECT_TRUE(model.present); EXPECT_TRUE(model.unresolved.empty()); ASSERT_EQ(model.sources.size(), 1u);
    EXPECT_EQ(model.sources.front(), nativeCanonical(f.root / "main.cpp").string());
}
