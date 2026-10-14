#include "CppAuditFrontend.h"
#include "../analysis/CppCompileModel.h"
#include <gtest/gtest.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef TOPO_FRONTEND_TEST_COMPILER
#error TOPO_FRONTEND_TEST_COMPILER must name the matching installed compiler
#endif
#ifndef TOPO_FRONTEND_TEST_RESOURCE
#error TOPO_FRONTEND_TEST_RESOURCE must name the matching resource directory
#endif
#ifndef TOPO_FRONTEND_TEST_SDK
#define TOPO_FRONTEND_TEST_SDK ""
#endif

using namespace topo::check;
namespace fs = std::filesystem;
namespace {
std::string text(CXString value) {
    const char* data = clang_getCString(value);
    std::string result = data ? data : "";
    clang_disposeString(value);
    return result;
}
struct Fixture {
    fs::path root;
    CppCompileUnit unit;
    CppAuditFrontendOptions options;
    Fixture() {
        llvm::SmallString<256> created;
        auto error = llvm::sys::fs::createUniqueDirectory("topo-frontend-test", created);
        if (error) throw std::runtime_error(error.message());
        root = fs::canonical(created.str().str());
        fs::permissions(root, fs::perms::owner_all);
        fs::create_directory(root / "private");
        fs::permissions(root / "private", fs::perms::owner_all);
        options.temporaryDirectory = root / "private";
        options.libclangResourceDirectory = TOPO_FRONTEND_TEST_RESOURCE;
        unit.file = (root / "main.cpp").string();
        unit.compiler = TOPO_FRONTEND_TEST_COMPILER;
        unit.directory = root.string();
        unit.arguments = {"--no-default-config", "-std=c++17", "-working-directory", root.string()};
        if (std::string(TOPO_FRONTEND_TEST_SDK).size()) {
            unit.arguments.push_back("-isysroot");
            unit.arguments.push_back(TOPO_FRONTEND_TEST_SDK);
        }
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void write(const std::string& name, const std::string& bytes) {
        fs::create_directories((root / name).parent_path());
        std::ofstream output(root / name, std::ios::binary); output << bytes;
    }
    CppAuditFrontendResult run() {
        return runCppAuditFrontend(unit, llvm::vfs::createPhysicalFileSystem(), options);
    }
    bool temporaryEmpty() const { return fs::directory_iterator(options.temporaryDirectory) == fs::directory_iterator{}; }
};
struct RawUnit {
    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;
    explicit RawUnit(const CppCompileUnit& unit) {
        auto strings = unit.arguments; strings.insert(strings.begin(), unit.compiler);
        std::vector<const char*> argv;
        for (const auto& arg : strings) argv.push_back(arg.c_str());
        clang_parseTranslationUnit2FullArgv(index, unit.file.c_str(), argv.data(),
            static_cast<int>(argv.size()), nullptr, 0, CXTranslationUnit_DetailedPreprocessingRecord, &tu);
    }
    ~RawUnit() { if (tu) clang_disposeTranslationUnit(tu); clang_disposeIndex(index); }
};
std::vector<std::string> cursors(CXTranslationUnit tu, const std::string& root) {
    std::vector<std::string> records;
    struct Visit { const std::string& root; std::vector<std::string>& records; } visit{root, records};
    clang_visitChildren(clang_getTranslationUnitCursor(tu), [](CXCursor cursor, CXCursor, CXClientData opaque) {
        auto& visit = *static_cast<Visit*>(opaque);
        CXFile file = nullptr; unsigned line = 0, column = 0;
        clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line, &column, nullptr);
        if (!file) return CXChildVisit_Continue;
        const auto path = text(clang_getFileName(file));
        if (path.rfind(visit.root + "/", 0) != 0) return CXChildVisit_Continue;
        auto referenced = clang_getCursorReferenced(cursor);
        std::ostringstream record;
        record << clang_getCursorKind(cursor) << '|' << text(clang_getCursorSpelling(cursor)) << '|'
               << text(clang_getCursorUSR(cursor)) << '|' << text(clang_getTypeSpelling(clang_getCursorType(cursor)))
               << '|' << path << ':' << line << ':' << column << '|'
               << text(clang_getCursorUSR(referenced)) << '|'
               << clang_Location_isInSystemHeader(clang_getCursorLocation(cursor));
        if (auto value = clang_Cursor_Evaluate(cursor)) {
            if (clang_EvalResult_getKind(value) == CXEval_Int)
                record << "|value=" << clang_EvalResult_getAsLongLong(value);
            clang_EvalResult_dispose(value);
        }
        visit.records.push_back(record.str());
        return CXChildVisit_Recurse;
    }, &visit);
    return records;
}
std::vector<std::string> errorMessages(CXTranslationUnit tu) {
    std::vector<std::string> result;
    for (unsigned n = 0; n < clang_getNumDiagnostics(tu); ++n) {
        auto diagnostic = clang_getDiagnostic(tu, n);
        if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error)
            result.push_back(text(clang_getDiagnosticSpelling(diagnostic)));
        clang_disposeDiagnostic(diagnostic);
    }
    return result;
}
std::vector<std::string> errorMessages(const CppAuditFrontendResult& result) {
    std::vector<std::string> messages;
    for (const auto& d : result.diagnostics) if (d.severity >= CXDiagnostic_Error) messages.push_back(d.message);
    return messages;
}
const char* semanticSource = R"cpp(
#include "local.h"
int global = 3;
struct Base { virtual int run() { return global; } };
template<class T> T same(T value) { return value; }
int leaf(int value) { return value + header; }
int call(Base& object) { return leaf(same(2)) + object.run(); }
#if __OPTIMIZE__
constexpr int optimized = 1;
#else
constexpr int optimized = 0;
#endif
)cpp";
}

TEST(CppAuditFrontend, PreservesRawCursorFactsAndEmbeddedInputs) {
    Fixture f; f.write("local.h", "constexpr int header = 42;\n"); f.write("main.cpp", semanticSource);
    RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr); ASSERT_TRUE(errorMessages(raw.tu).empty());
    auto result = f.run(); ASSERT_EQ(result.failure, CppAuditFrontendFailure::None) << result.reason;
    ASSERT_NE(result.get(), nullptr); EXPECT_EQ(cursors(raw.tu, f.root.string()), cursors(result.get(), f.root.string()));
    EXPECT_TRUE(result.invocation.embedAllFiles); EXPECT_GT(result.serializedBytes, 0u);
    EXPECT_FALSE(result.invocation.driverCC1Arguments.empty()); EXPECT_FALSE(result.invocation.effectiveCC1Arguments.empty());
    EXPECT_FALSE(result.serializationBufferQuotaEnforced); EXPECT_TRUE(f.temporaryEmpty());
    const auto oldFacts = cursors(result.get(), f.root.string());
    const auto header = f.root / "local.h"; const auto stamp = fs::last_write_time(header);
    f.write("local.h", "constexpr int header = 17;\n"); fs::last_write_time(header, stamp);
    std::ifstream disk(header); std::string line; std::getline(disk, line); EXPECT_EQ(line, "constexpr int header = 17;");
    size_t count = 0;
    const auto* bytes = clang_getFileContents(result.get(), clang_getFile(result.get(), header.c_str()), &count);
    ASSERT_NE(bytes, nullptr); EXPECT_EQ(std::string(bytes, count), "constexpr int header = 42;\n");
    EXPECT_EQ(oldFacts, cursors(result.get(), f.root.string()));
    RawUnit changed(f.unit); EXPECT_NE(oldFacts, cursors(changed.tu, f.root.string()));
}

TEST(CppAuditFrontend, PreservesFactsAcrossCodeGenAndLanguageOptions) {
    for (const std::vector<std::string>& flags : {
        std::vector<std::string>{"-O2", "-g3"}, {"-O0", "-gdwarf-5"},
        {"-fvisibility=hidden", "-fvisibility-inlines-hidden"}, {"-fno-omit-frame-pointer"},
        {"-fshort-wchar", "-funsigned-char"}, {"-std=c++20"}, {"-fno-exceptions", "-fno-rtti"}}) {
        Fixture f; f.unit.arguments.insert(f.unit.arguments.end(), flags.begin(), flags.end());
        f.write("local.h", "constexpr int header = sizeof(wchar_t);\n"); f.write("main.cpp", semanticSource);
        RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr); ASSERT_TRUE(errorMessages(raw.tu).empty());
        auto result = f.run(); ASSERT_NE(result.get(), nullptr) << result.reason;
        EXPECT_EQ(cursors(raw.tu, f.root.string()), cursors(result.get(), f.root.string())) << flags.front();
    }
}

TEST(CppAuditFrontend, PreservesRealSystemHeaderFacts) {
    Fixture f;
    f.write("main.cpp", "#include <vector>\n#include <cstddef>\nstd::size_t read(){std::vector<int> data{1,2}; return data.size();}\n");
    RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr); ASSERT_TRUE(errorMessages(raw.tu).empty());
    auto result = f.run(); ASSERT_NE(result.get(), nullptr) << result.reason;
    EXPECT_EQ(cursors(raw.tu, f.root.string()), cursors(result.get(), f.root.string()));
}

TEST(CppAuditFrontend, KeepsProducerErrorsAndErrorASTFacts) {
    Fixture f; f.write("main.cpp", "int missing_type(){ unknown value; return missing_name; }\n");
    RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr);
    auto result = f.run(); EXPECT_EQ(result.failure, CppAuditFrontendFailure::Parse);
    ASSERT_NE(result.get(), nullptr) << result.reason; EXPECT_TRUE(result.diagnosticsComplete);
    EXPECT_EQ(cursors(raw.tu, f.root.string()), cursors(result.get(), f.root.string()));
    EXPECT_EQ(errorMessages(raw.tu), errorMessages(result)); EXPECT_FALSE(result.diagnostics.empty());
    EXPECT_GT(result.serializedBytes, 0u); EXPECT_TRUE(f.temporaryEmpty());
}

TEST(CppAuditFrontend, KeepsDriverErrors) {
    Fixture f; f.write("main.cpp", "void run(){}\n"); f.unit.arguments.push_back("-std=not-a-language");
    auto result = f.run(); EXPECT_EQ(result.get(), nullptr); EXPECT_TRUE(result.hasErrors());
    EXPECT_NE(result.failure, CppAuditFrontendFailure::None);
    EXPECT_TRUE(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) {
        return d.message.find("not-a-language") != std::string::npos;
    })); EXPECT_TRUE(f.temporaryEmpty());
}

TEST(CppAuditFrontend, DetectsActualNestedAndPastedTimeBuiltinsWithWarningsSuppressed) {
    Fixture f;
    f.unit.arguments.push_back("-Wdate-time");
    f.write("main.cpp", R"cpp(
#pragma clang diagnostic ignored "-Wdate-time"
#define JOIN(a,b) a##b
#define INNER __TIME__
#define OUTER INNER
const char* a = OUTER;
const char* b = JOIN(__TI,ME__);
const char* c = __DATE__;
const char* d = __TIMESTAMP__;
)cpp");
    auto result = f.run(); ASSERT_NE(result.get(), nullptr) << result.reason;
    std::multiset<std::string> actual;
    for (const auto& expansion : result.builtinExpansions) if (expansion.volatileTime) actual.insert(expansion.name);
    EXPECT_EQ(actual, (std::multiset<std::string>{"__TIME__", "__TIME__", "__DATE__", "__TIMESTAMP__"}));
    EXPECT_TRUE(result.builtinExpansionsComplete);
}

TEST(CppAuditFrontend, UnexpandedAndRedefinedTimeMacrosAreDistinguished) {
    Fixture f;
    f.write("main.cpp", "#define UNUSED __TIME__\n#undef __DATE__\n#define __DATE__ \"constant\"\nconst char* text=__DATE__;\n");
    auto result = f.run(); ASSERT_NE(result.get(), nullptr) << result.reason;
    EXPECT_TRUE(std::none_of(result.builtinExpansions.begin(), result.builtinExpansions.end(),
                            [](const auto& expansion) { return expansion.volatileTime; }));
}

TEST(CppAuditFrontend, ResourceOverrideKeepsDriverSearchEntryOrder) {
    Fixture f; fs::create_directory(f.root / "empty-resource");
    f.unit.arguments.insert(f.unit.arguments.end(), {"-resource-dir", (f.root / "empty-resource").string()});
    f.write("main.cpp", "#include <stddef.h>\nvoid run() {}\n");
    RawUnit raw(f.unit); auto result = f.run();
    ASSERT_NE(raw.tu, nullptr);
    EXPECT_EQ(errorMessages(raw.tu), errorMessages(result));
    EXPECT_EQ(result.invocation.resourceDirectory, TOPO_FRONTEND_TEST_RESOURCE);
    EXPECT_NE(std::find(result.invocation.driverCC1Arguments.begin(), result.invocation.driverCC1Arguments.end(),
                        (f.root / "empty-resource").string()), result.invocation.driverCC1Arguments.end());
}

TEST(CppAuditFrontend, RejectsOversizedFileBeforeDiskWriteAndReportsUnenforcedBufferQuota) {
    Fixture f; f.write("main.cpp", "void run() {}\n"); f.options.limits.maxAstFileBytes = 1;
    auto result = f.run(); EXPECT_EQ(result.failure, CppAuditFrontendFailure::AstFileLimit);
    EXPECT_EQ(result.get(), nullptr); EXPECT_GT(result.serializedBytes, 1u); EXPECT_TRUE(f.temporaryEmpty());
    EXPECT_FALSE(result.serializationBufferQuotaEnforced);
}

TEST(CppAuditFrontend, BoundsRetainedDiagnosticsAndBuiltinCount) {
    Fixture f; f.write("main.cpp", "int a = missing_a; int b = missing_b;\n"); f.options.limits.maxDiagnostics = 1;
    auto diagnostics = f.run(); EXPECT_EQ(diagnostics.failure, CppAuditFrontendFailure::DiagnosticsLimit);
    EXPECT_LE(diagnostics.diagnostics.size(), 1u); EXPECT_FALSE(diagnostics.diagnosticsComplete);
    f.write("main.cpp", "const char* a=__TIME__; const char* b=__TIME__;\n");
    f.options.limits.maxBuiltinExpansions = 1;
    auto builtins = f.run(); EXPECT_EQ(builtins.failure, CppAuditFrontendFailure::BuiltinLimit);
    EXPECT_LE(builtins.builtinExpansions.size(), 1u); EXPECT_FALSE(builtins.builtinExpansionsComplete);
    EXPECT_TRUE(f.temporaryEmpty());
}

TEST(CppAuditFrontend, RequiresPrivateDirectoryAndMovesOwnership) {
    Fixture f; f.write("main.cpp", "void run() {}\n");
    fs::permissions(f.options.temporaryDirectory, fs::perms::owner_all | fs::perms::group_read);
    auto rejected = f.run(); EXPECT_EQ(rejected.failure, CppAuditFrontendFailure::TemporaryFile);
    fs::permissions(f.options.temporaryDirectory, fs::perms::owner_all);
    auto first = f.run(); ASSERT_NE(first.get(), nullptr) << first.reason;
    const auto original = first.get(); auto second = std::move(first);
    EXPECT_EQ(first.get(), nullptr); EXPECT_EQ(second.get(), original);
    auto third = f.run(); third = std::move(second);
    EXPECT_EQ(second.get(), nullptr); EXPECT_EQ(third.get(), original); EXPECT_TRUE(f.temporaryEmpty());
}

namespace {
class SubstitutedFile final : public llvm::vfs::File {
    std::unique_ptr<llvm::vfs::File> original_;
    std::string bytes_;
public:
    SubstitutedFile(std::unique_ptr<llvm::vfs::File> original, std::string bytes)
        : original_(std::move(original)), bytes_(std::move(bytes)) {}
    llvm::ErrorOr<llvm::vfs::Status> status() override { return original_->status(); }
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getBuffer(
        const llvm::Twine& name, int64_t, bool, bool) override {
        return llvm::MemoryBuffer::getMemBufferCopy(bytes_, name.str());
    }
    std::error_code close() override { return original_->close(); }
};
class SuppliedView final : public llvm::vfs::ProxyFileSystem {
    std::string source_, bytes_;
public:
    std::size_t sourceOpens = 0;
    SuppliedView(std::string source, std::string bytes)
        : ProxyFileSystem(llvm::vfs::createPhysicalFileSystem()),
          source_(std::move(source)), bytes_(std::move(bytes)) {}
    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>> openFileForRead(const llvm::Twine& path) override {
        auto file = getUnderlyingFS().openFileForRead(path);
        if (!file || path.str() != source_) return file;
        ++sourceOpens;
        return std::unique_ptr<llvm::vfs::File>(new SubstitutedFile(std::move(*file), bytes_));
    }
};
}
TEST(CppAuditFrontend, UsesCallerSuppliedBytesThroughEmbeddedAST) {
    Fixture f; f.write("main.cpp", "constexpr int selected = 17;\n");
    const std::string supplied = "constexpr int selected = 42;\n";
    auto view = llvm::makeIntrusiveRefCnt<SuppliedView>(f.unit.file, supplied);
    auto result = runCppAuditFrontend(f.unit, view, f.options);
    ASSERT_NE(result.get(), nullptr) << result.reason; EXPECT_GT(view->sourceOpens, 0u);
    size_t size = 0;
    const auto* bytes = clang_getFileContents(result.get(), clang_getFile(result.get(), f.unit.file.c_str()), &size);
    ASSERT_NE(bytes, nullptr); EXPECT_EQ(std::string(bytes, size), supplied);
    auto values = cursors(result.get(), f.root.string());
    EXPECT_TRUE(std::any_of(values.begin(), values.end(), [](const auto& value) { return value.find("value=42") != std::string::npos; }));
    RawUnit raw(f.unit); EXPECT_NE(values, cursors(raw.tu, f.root.string()));
}

#ifdef TOPO_FRONTEND_TEST_RECORDING_FS
#include "CppAuditFileSystem.h"
TEST(CppAuditFrontend, RealVectorUsesAuthorizedRecordingViewAndReplaysAfterSerialization) {
    Fixture f;
    f.write("main.cpp", "#include <vector>\n#include <cstddef>\nstd::size_t read(){std::vector<int> data{1,2}; return data.size();}\n");
    const auto resource = fs::canonical(TOPO_FRONTEND_TEST_RESOURCE);
    const auto package = resource.parent_path().parent_path().parent_path();
    CppAuditReadPolicy policy;
    policy.roots = {f.root.string(), resource.string(), fs::canonical(package / "include/c++/v1").string()};
    if (std::string(TOPO_FRONTEND_TEST_SDK).size()) policy.roots.push_back(fs::canonical(TOPO_FRONTEND_TEST_SDK).string());
    auto view = llvm::makeIntrusiveRefCnt<CppAuditFileSystem>(f.root.string(), policy);
    auto result = runCppAuditFrontend(f.unit, view, f.options);
    RecordProperty("observationCount", static_cast<int>(view->observations().size()));
    RecordProperty("capturedBytes", std::to_string(view->capturedBytes()));
    RecordProperty("recordingComplete", view->complete());
    RecordProperty("recordingFailure", view->failure());
    ASSERT_TRUE(view->complete()) << view->failure(); ASSERT_NE(result.get(), nullptr) << result.reason;
    RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr);
    EXPECT_EQ(cursors(raw.tu, f.root.string()), cursors(result.get(), f.root.string()));
    const auto count = view->observations().size(); const auto bytes = view->capturedBytes();
    const auto observations = view->observations();
    view->seal();
    for (const auto& observed : observations) {
        switch (observed.kind) {
        case CppAuditFileQuery::Status: {
            const auto actual = view->status(observed.path);
            EXPECT_EQ(actual ? std::error_code{} : actual.getError(), observed.error);
            if (actual && observed.status) EXPECT_EQ(actual->getUniqueID(), observed.status->getUniqueID());
            break;
        }
        case CppAuditFileQuery::Open: {
            auto actual = view->openFileForRead(observed.path);
            EXPECT_EQ(actual ? std::error_code{} : actual.getError(), observed.error);
            if (actual && observed.bytes) {
                auto buffer = (*actual)->getBuffer(observed.path);
                ASSERT_TRUE(buffer); EXPECT_EQ((*buffer)->getBuffer(), *observed.bytes);
            }
            break;
        }
        case CppAuditFileQuery::RealPath: {
            llvm::SmallString<256> path;
            EXPECT_EQ(view->getRealPath(observed.path, path), observed.error);
            if (!observed.error) EXPECT_EQ(path.str(), observed.physicalPath);
            break;
        }
        case CppAuditFileQuery::Locality: {
            bool local = false;
            EXPECT_EQ(view->isLocal(observed.path, local), observed.error);
            if (!observed.error) EXPECT_EQ(local, observed.local);
            break;
        }
        case CppAuditFileQuery::Directory: {
            std::error_code error; auto directory = view->dir_begin(observed.path, error);
            EXPECT_EQ(error, observed.error);
            std::size_t index = 0;
            while (!error && directory != llvm::vfs::directory_iterator{}) {
                ASSERT_TRUE(observed.entries); ASSERT_LT(index, observed.entries->size());
                EXPECT_EQ(directory->path(), observed.entries->at(index).path());
                directory.increment(error); ++index;
            }
            EXPECT_EQ(error, observed.iterationError); break;
        }
        }
    }
    EXPECT_EQ(view->observations().size(), count); EXPECT_EQ(view->capturedBytes(), bytes);
    EXPECT_TRUE(view->complete()) << view->failure(); EXPECT_TRUE(view->sealed()); EXPECT_TRUE(f.temporaryEmpty());
}
#endif

TEST(CppAuditFrontend, DetectsImplicitPCHBeforeReopeningItsSerializedChain) {
    Fixture f; f.write("prefix.h", "constexpr int from_pch = 42;\n");
    f.write("main.cpp", "int run(){return from_pch;}\n");
    std::vector<std::string> strings{f.unit.compiler};
    strings.insert(strings.end(), f.unit.arguments.begin(), f.unit.arguments.end());
    strings.insert(strings.end(), {"-Xclang", "-detailed-preprocessing-record", "-x", "c++-header",
        (f.root / "prefix.h").string(), "-o", (f.root / "prefix.h.pch").string()});
    std::vector<llvm::StringRef> arguments;
    for (const auto& argument : strings) arguments.emplace_back(argument);
    std::string error;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(f.unit.compiler, arguments, std::nullopt, {}, 30, 0, &error), 0) << error;
    f.unit.arguments.insert(f.unit.arguments.end(), {"-include", (f.root / "prefix.h").string()});
    RawUnit raw(f.unit); ASSERT_NE(raw.tu, nullptr); ASSERT_TRUE(errorMessages(raw.tu).empty());
    auto result = f.run(); EXPECT_EQ(result.failure, CppAuditFrontendFailure::ImportedAST) << result.reason;
    EXPECT_EQ(result.get(), nullptr); EXPECT_FALSE(result.hasErrors()); EXPECT_TRUE(f.temporaryEmpty());
}

TEST(CppAuditFrontend, RejectsGpuNativeBeforeDriverOrFilesystemPreparation) {
    for (const auto& flags : {std::vector<std::string>{"--target=amdgcn-amd-amdhsa", "-mcpu=native"},
                             std::vector<std::string>{"--target=nvptx64-nvidia-cuda", "-march=native"}
#if LLVM_VERSION_MAJOR >= 23
                             , std::vector<std::string>{"-x", "hip", "-mcpu=native"}
#endif
                             }) {
        Fixture f; f.write("main.cpp", "int run() { return 7; }\n");
        f.unit.arguments.insert(f.unit.arguments.end(), flags.begin(), flags.end());
        auto result = f.run();
        EXPECT_EQ(result.failure, CppAuditFrontendFailure::InvalidArguments);
        EXPECT_NE(result.reason.find("before frontend invocation"), std::string::npos);
        EXPECT_EQ(result.get(), nullptr);
        EXPECT_TRUE(result.invocation.driverArguments.empty());
        EXPECT_TRUE(result.diagnostics.empty());
        EXPECT_TRUE(f.temporaryEmpty());
    }
}
