#include "CppAuditFrontend.h"
#include "CppAuditDriver.h"
#include "../analysis/CppCompileModel.h"
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Driver/CreateInvocationFromArgs.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <clang/Serialization/ASTWriter.h>
#include <clang/Serialization/ASTReader.h>
#include <clang/Serialization/ModuleCache.h>
#include <clang/Serialization/PCHContainerOperations.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Bitstream/BitstreamWriter.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>
#include <memory>
#include <system_error>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace topo::check {
namespace {
CppAuditFrontendLocation location(clang::SourceLocation loc,
                                  const clang::SourceManager& source) {
    CppAuditFrontendLocation result;
    if (loc.isInvalid()) return result;
    loc = source.getExpansionLoc(loc);
    if (loc.isInvalid()) return result;
    result.file = source.getFilename(loc).str();
    result.line = source.getExpansionLineNumber(loc);
    result.column = source.getExpansionColumnNumber(loc);
    result.offset = source.getFileOffset(loc);
    return result;
}
CXDiagnosticSeverity severity(clang::DiagnosticsEngine::Level level) {
    switch (level) {
    case clang::DiagnosticsEngine::Ignored: return CXDiagnostic_Ignored;
    case clang::DiagnosticsEngine::Note:
    case clang::DiagnosticsEngine::Remark: return CXDiagnostic_Note;
    case clang::DiagnosticsEngine::Warning: return CXDiagnostic_Warning;
    case clang::DiagnosticsEngine::Error: return CXDiagnostic_Error;
    case clang::DiagnosticsEngine::Fatal: return CXDiagnostic_Fatal;
    }
    return CXDiagnostic_Fatal;
}

class Diagnostics final : public clang::DiagnosticConsumer {
    CppAuditFrontendResult& result_;
    const CppAuditFrontendLimits& limits_;
    std::size_t bytes_ = 0;
public:
    Diagnostics(CppAuditFrontendResult& result, const CppAuditFrontendLimits& limits)
        : result_(result), limits_(limits) {}
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& diagnostic) override {
        clang::DiagnosticConsumer::HandleDiagnostic(level, diagnostic);
        if (!result_.diagnosticsComplete) return;
        if (result_.diagnostics.size() >= limits_.maxDiagnostics) {
            result_.diagnosticsComplete = false;
            return;
        }
        llvm::SmallString<256> message;
        diagnostic.FormatDiagnostic(message);
        CppAuditFrontendLocation loc;
        if (diagnostic.hasSourceManager())
            loc = location(diagnostic.getLocation(), diagnostic.getSourceManager());
        const auto remaining = limits_.maxRetainedDiagnosticBytes - bytes_;
        if (message.size() > remaining || loc.file.size() > remaining - message.size()) {
            result_.diagnosticsComplete = false;
            return;
        }
        bytes_ += message.size() + loc.file.size();
        result_.diagnostics.push_back({severity(level), diagnostic.getID(),
                                       message.str().str(), std::move(loc)});
    }
};

class Builtins final : public clang::PPCallbacks {
    CppAuditFrontendResult& result_;
    const CppAuditFrontendLimits& limits_;
    clang::SourceManager& source_;
public:
    Builtins(CppAuditFrontendResult& result, const CppAuditFrontendLimits& limits,
             clang::SourceManager& source)
        : result_(result), limits_(limits), source_(source) {}
    void MacroExpands(const clang::Token& token, const clang::MacroDefinition& definition,
                      clang::SourceRange, const clang::MacroArgs*) override {
        const auto* info = definition.getMacroInfo();
        const auto* identifier = token.getIdentifierInfo();
        if (!info || !info->isBuiltinMacro() || !identifier) return;
        if (result_.builtinExpansions.size() >= limits_.maxBuiltinExpansions) {
            result_.builtinExpansionsComplete = false;
            return;
        }
        const auto name = identifier->getName().str();
        result_.builtinExpansions.push_back({name, location(token.getLocation(), source_),
            name == "__DATE__" || name == "__TIME__" || name == "__TIMESTAMP__"});
    }
};

class Action final : public clang::SyntaxOnlyAction {
    CppAuditFrontendResult& result_;
    const CppAuditFrontendLimits& limits_;
public:
    Action(CppAuditFrontendResult& result, const CppAuditFrontendLimits& limits)
        : result_(result), limits_(limits) {}
    bool BeginSourceFileAction(clang::CompilerInstance& compiler) override {
        result_.invocation.effectiveCC1Arguments = compiler.getInvocation().getCC1CommandLine();
        result_.invocation.resourceDirectory = compiler.getHeaderSearchOpts().ResourceDir;
        result_.invocation.sysroot = compiler.getHeaderSearchOpts().Sysroot;
        result_.invocation.workingDirectory = compiler.getFileSystemOpts().WorkingDir;
        result_.invocation.embedAllFiles = compiler.getFrontendOpts().ModulesEmbedAllFiles;
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<Builtins>(result_, limits_, compiler.getSourceManager()));
        return true;
    }
};

class TemporaryAst {
    std::filesystem::path path_;
public:
    ~TemporaryAst() { std::error_code ignored; if (!path_.empty()) std::filesystem::remove(path_, ignored); }
    std::string path() const { return path_.string(); }
    std::error_code write(const std::filesystem::path& directory,
                          llvm::ArrayRef<char> buffer) {
        llvm::SmallString<256> created;
        int descriptor = -1;
        auto error = llvm::sys::fs::createUniqueFile(
            (directory / "topo-audit-%%%%%%.ast").string(), descriptor, created,
            llvm::sys::fs::OF_None, 0600);
        if (error) return error;
        path_ = created.str().str();
        llvm::raw_fd_ostream output(descriptor, true);
        output.write(buffer.data(), buffer.size());
        output.close();
        const auto result = output.error();
        output.clear_error();
        return result;
    }
    std::error_code remove() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (!error) path_.clear();
        return error;
    }
};

bool privateDirectory(const std::filesystem::path& directory, std::string& reason) {
    if (!directory.is_absolute()) { reason = "AST temporary directory must be absolute"; return false; }
#ifndef _WIN32
    struct stat status{};
    if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 0777) != 0700) {
        reason = "AST temporary directory must be an owned mode-0700 directory";
        return false;
    }
    return true;
#else
    reason = "AST temporary-directory ownership validation is unavailable on this platform";
    return false;
#endif
}

} // namespace

CppAuditFrontendResult::~CppAuditFrontendResult() {
    if (translationUnit_) clang_disposeTranslationUnit(translationUnit_);
    if (index_) clang_disposeIndex(index_);
}
CppAuditFrontendResult::CppAuditFrontendResult(CppAuditFrontendResult&& other) noexcept {
    *this = std::move(other);
}
CppAuditFrontendResult& CppAuditFrontendResult::operator=(CppAuditFrontendResult&& other) noexcept {
    if (this == &other) return *this;
    if (translationUnit_) clang_disposeTranslationUnit(translationUnit_);
    if (index_) clang_disposeIndex(index_);
    index_ = std::exchange(other.index_, nullptr);
    translationUnit_ = std::exchange(other.translationUnit_, nullptr);
    failure = other.failure;
    reason = std::move(other.reason);
    diagnostics = std::move(other.diagnostics);
    builtinExpansions = std::move(other.builtinExpansions);
    invocation = std::move(other.invocation);
    serializedBytes = other.serializedBytes;
    diagnosticsComplete = other.diagnosticsComplete;
    builtinExpansionsComplete = other.builtinExpansionsComplete;
    serializationBufferQuotaEnforced = other.serializationBufferQuotaEnforced;
    return *this;
}
bool CppAuditFrontendResult::hasErrors() const {
    return !diagnosticsComplete || std::any_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.severity >= CXDiagnostic_Error; });
}

CppAuditFrontendResult runCppAuditFrontend(
    const CppCompileUnit& unit,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    const CppAuditFrontendOptions& options) {
    CppAuditFrontendResult result;
    auto fail = [&](CppAuditFrontendFailure kind, std::string reason) {
        result.failure = kind;
        result.reason = std::move(reason);
    };
    if (!fileSystem || unit.file.empty() || unit.compiler.empty() || !unit.errors.empty() ||
        options.libclangResourceDirectory.empty() || options.limits.maxAstFileBytes == 0 ||
        options.limits.maxRetainedDiagnosticBytes == 0 || options.limits.maxDiagnostics == 0) {
        fail(CppAuditFrontendFailure::InvalidArguments, "incomplete C++ audit frontend arguments");
        return result;
    }
    if (const auto rejection = cppAuditDriverRejection(unit); !rejection.empty()) {
        fail(CppAuditFrontendFailure::InvalidArguments, rejection);
        return result;
    }
    if (!privateDirectory(options.temporaryDirectory, result.reason)) {
        result.failure = CppAuditFrontendFailure::TemporaryFile;
        return result;
    }
    try {
        auto arguments = unit.arguments;
        arguments.insert(arguments.begin(), unit.compiler);
        std::vector<const char*> original;
        for (const auto& arg : arguments) original.push_back(arg.c_str());
        auto diagnosticOptions = std::shared_ptr<clang::DiagnosticOptions>(
            clang::CreateAndPopulateDiagOpts(original));
        Diagnostics consumer(result, options.limits);
        auto diagnostics = clang::CompilerInstance::createDiagnostics(
            *fileSystem, *diagnosticOptions, &consumer, false);
        if (std::find(arguments.begin(), arguments.end(), "-fno-spell-checking") == arguments.end() &&
            std::find(arguments.begin(), arguments.end(), "-fspell-checking") == arguments.end())
            arguments.insert(arguments.begin() + 1, "-fno-spell-checking");
        arguments.push_back(unit.file);
        arguments.push_back("-Xclang");
        arguments.push_back("-detailed-preprocessing-record");
        arguments.push_back("-fallow-editor-placeholders");
        arguments.push_back("-Xclang");
        arguments.push_back("-fmodules-embed-all-files");
        result.invocation.driverArguments = arguments;
        std::vector<const char*> argv;
        for (const auto& arg : arguments) argv.push_back(arg.c_str());
        clang::CreateInvocationOptions invocationOptions;
        invocationOptions.VFS = fileSystem;
        invocationOptions.Diags = diagnostics;
        invocationOptions.ProbePrecompiled = true;
        invocationOptions.CC1Args = &result.invocation.driverCC1Arguments;
        std::shared_ptr<clang::CompilerInvocation> invocation =
            clang::createInvocation(argv, invocationOptions);
        if (!result.diagnosticsComplete) {
            fail(CppAuditFrontendFailure::DiagnosticsLimit, "C++ driver diagnostics exceed retention limits");
            return result;
        }
        if (!invocation || result.hasErrors()) {
            fail(CppAuditFrontendFailure::Invocation, "cannot construct C++ audit frontend invocation");
            return result;
        }
        auto containers = std::make_shared<clang::PCHContainerOperations>();
        auto& preprocessor = invocation->getPreprocessorOpts();
        preprocessor.RemappedFilesKeepOriginalName = true;
        preprocessor.AllowPCHWithCompilerErrors = true;
        preprocessor.SingleFileParseMode = false;
        preprocessor.RetainExcludedConditionalBlocks = false;
        // CIndex applies this after Driver has already populated UserEntries.
        invocation->getHeaderSearchOpts().ResourceDir = options.libclangResourceDirectory;
        invocation->getHeaderSearchOpts().ModuleFormat =
            containers->getRawReader().getFormats().front().str();
        invocation->getFrontendOpts().SkipFunctionBodies = false;
        if (invocation->getFrontendOpts().Inputs.size() != 1 ||
            !invocation->getFrontendOpts().Inputs.front().isFile() ||
            invocation->getFrontendOpts().Inputs.front().getKind().getFormat() != clang::InputKind::Source ||
            invocation->getFrontendOpts().Inputs.front().getKind().getLanguage() == clang::Language::LLVM_IR) {
            fail(CppAuditFrontendFailure::Invocation, "C++ audit requires one frontend source input");
            return result;
        }
        // The current compile-model sanitizer excludes user VFS overlays.
        // ASTUnit::create() internally creates a default VFS before its file
        // manager can be replaced, so guard that boundary explicitly.
        if (!invocation->getHeaderSearchOpts().VFSOverlayFiles.empty()) {
            fail(CppAuditFrontendFailure::Invocation, "user VFS overlays require a separately authorized frontend path");
            return result;
        }
        // Retain the bounded external consumer; ASTUnit's All mode stores a
        // second, unlimited diagnostic vector and replaces the consumer.
        auto ast = clang::ASTUnit::create(invocation, diagnosticOptions, diagnostics,
                                          clang::CaptureDiagsKind::None, true);
        ast->getFileManager().setVirtualFileSystem(fileSystem);
        Action action(result, options.limits);
        auto* loaded = clang::ASTUnit::LoadFromCompilerInvocationAction(
            invocation, containers, diagnosticOptions, diagnostics, &action, ast.get(),
            true, "", false, clang::CaptureDiagsKind::None, 0, false, true);
        if (!result.diagnosticsComplete) {
            fail(CppAuditFrontendFailure::DiagnosticsLimit, "C++ producer diagnostics exceed retention limits");
            return result;
        }
        if (!result.builtinExpansionsComplete) {
            fail(CppAuditFrontendFailure::BuiltinLimit, "actual builtin expansion count exceeds limit");
            return result;
        }
        if (!loaded) {
            fail(CppAuditFrontendFailure::Parse, "C++ producer reported a parse failure");
            return result;
        }
        // Embedding source buffers does not flatten an imported PCH/PCM
        // chain. The C API loader would reopen that chain outside this view.
        if (const auto reader = ast->getASTReader();
            reader && reader->getModuleManager().size() != 0) {
            fail(CppAuditFrontendFailure::ImportedAST,
                 "imported PCH/PCM requires a pinned serialized-AST dependency bridge");
            return result;
        }
        const auto producerDiagnosticCount = result.diagnostics.size();
        const bool producerHadErrors = result.hasErrors();
        // ASTUnit::create() leaves its private CodeGenOptions defaulted. Use
        // the public writer with the actual invocation's options explicitly.
        llvm::SmallVector<char, 0> buffer;
        {
            llvm::BitstreamWriter stream(buffer);
            auto cache = clang::createCrossProcessModuleCache();
            clang::ASTWriter writer(stream, buffer, *cache, invocation->getCodeGenOpts(), {});
            writer.WriteAST(&ast->getSema(), std::string(), nullptr, "");
        }
        result.serializedBytes = buffer.size();
        if (!result.diagnosticsComplete || std::any_of(
                result.diagnostics.begin() + producerDiagnosticCount, result.diagnostics.end(),
                [](const auto& d) { return d.severity >= CXDiagnostic_Error; })) {
            fail(CppAuditFrontendFailure::Serialization, "C++ AST serialization reported diagnostics");
            return result;
        }
        if (buffer.size() > options.limits.maxAstFileBytes) {
            fail(CppAuditFrontendFailure::AstFileLimit, "serialized C++ AST exceeds file limit");
            return result;
        }
        TemporaryAst temporary;
        if (auto error = temporary.write(options.temporaryDirectory, buffer)) {
            fail(CppAuditFrontendFailure::TemporaryFile, "cannot write private C++ AST: " + error.message());
            return result;
        }
        result.index_ = clang_createIndex(0, 0);
        const auto status = clang_createTranslationUnit2(result.index_, temporary.path().c_str(),
                                                        &result.translationUnit_);
        const auto cleanup = temporary.remove();
        if (cleanup) {
            if (result.translationUnit_) clang_disposeTranslationUnit(result.translationUnit_);
            result.translationUnit_ = nullptr;
            fail(CppAuditFrontendFailure::TemporaryFile, "cannot remove private C++ AST: " + cleanup.message());
        } else if (status != CXError_Success || !result.translationUnit_) {
            fail(CppAuditFrontendFailure::Load, "cannot load embedded C++ AST through libclang");
        } else if (producerHadErrors) {
            fail(CppAuditFrontendFailure::Parse, "C++ producer reported errors; error AST is available for partial facts");
        }
    } catch (const std::exception& error) {
        fail(CppAuditFrontendFailure::Serialization, std::string("C++ frontend failure: ") + error.what());
    }
    return result;
}
} // namespace topo::check
