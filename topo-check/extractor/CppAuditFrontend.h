#ifndef TOPO_CPP_AUDIT_FRONTEND_H
#define TOPO_CPP_AUDIT_FRONTEND_H

#include <clang-c/Index.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace topo::check {
struct CppCompileUnit;

struct CppAuditFrontendLocation {
    std::string file;
    unsigned line = 0, column = 0, offset = 0;
};
struct CppAuditFrontendDiagnostic {
    CXDiagnosticSeverity severity = CXDiagnostic_Ignored;
    unsigned id = 0;
    std::string message;
    CppAuditFrontendLocation location;
};
struct CppAuditBuiltinExpansion {
    std::string name;
    CppAuditFrontendLocation location;
    bool volatileTime = false;
};
struct CppAuditFrontendInvocation {
    std::vector<std::string> driverArguments;
    std::vector<std::string> driverCC1Arguments;
    std::vector<std::string> effectiveCC1Arguments;
    std::string resourceDirectory, sysroot, workingDirectory;
    bool embedAllFiles = false;
};
struct CppAuditFrontendLimits {
    std::size_t maxDiagnostics = 4096;
    std::size_t maxRetainedDiagnosticBytes = 1024 * 1024;
    std::size_t maxBuiltinExpansions = 65536;
    // Ceiling on the serialized AST admitted to disk/C API. LLVM's public
    // ASTWriter currently fills its SmallVector before this check; callers
    // must also enforce a worker memory quota for a hard allocation bound.
    std::size_t maxAstFileBytes = 256 * 1024 * 1024;
};
struct CppAuditFrontendOptions {
    // The existing AnalysisProcess invocation's private directory. The
    // helper validates its ownership/mode and exclusively creates its AST.
    std::filesystem::path temporaryDirectory;
    // Resource path belonging to the same libclang package. Like CIndex,
    // applied after internal driver translation; an explicit -resource-dir
    // in the model still affects the driver's generated search entries.
    std::string libclangResourceDirectory;
    CppAuditFrontendLimits limits;
};

enum class CppAuditFrontendFailure {
    None, InvalidArguments, Invocation, Parse, DiagnosticsLimit,
    BuiltinLimit, ImportedAST, Serialization, AstFileLimit, Load, TemporaryFile
};

// Owns its CXIndex and translation unit. Move-only; the caller may borrow
// get() through the existing State walker, and must keep this owner alive.
class CppAuditFrontendResult {
public:
    CppAuditFrontendResult() = default;
    ~CppAuditFrontendResult();
    CppAuditFrontendResult(CppAuditFrontendResult&&) noexcept;
    CppAuditFrontendResult& operator=(CppAuditFrontendResult&&) noexcept;
    CppAuditFrontendResult(const CppAuditFrontendResult&) = delete;
    CppAuditFrontendResult& operator=(const CppAuditFrontendResult&) = delete;
    CXTranslationUnit get() const { return translationUnit_; }
    bool hasErrors() const;
    CppAuditFrontendFailure failure = CppAuditFrontendFailure::None;
    std::string reason;
    std::vector<CppAuditFrontendDiagnostic> diagnostics;
    std::vector<CppAuditBuiltinExpansion> builtinExpansions;
    CppAuditFrontendInvocation invocation;
    std::size_t serializedBytes = 0;
    bool diagnosticsComplete = true;
    bool builtinExpansionsComplete = true;
    // This property is deliberately separate from maxAstFileBytes. The
    // upstream public writer has no per-buffer allocation ceiling hook.
    bool serializationBufferQuotaEnforced = false;
private:
    CXIndex index_ = nullptr;
    CXTranslationUnit translationUnit_ = nullptr;
    friend CppAuditFrontendResult runCppAuditFrontend(
        const CppCompileUnit&, llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>,
        const CppAuditFrontendOptions&);
};

// Use the supplied view through internal-driver preparation, AST parsing
// and serialization. It remains unsealed on return so the caller controls
// its observation lifecycle. Errors retain original producer diagnostics;
// error ASTs may supply partial facts with failure=Parse and are never
// advertised as successful analysis. The returned C API
// unit contains embedded original input bytes; it is never reparsed here.
// Imported PCH/PCM chains return ImportedAST until their serialized inputs
// can be pinned for the C API loader as well as for the producing view.
CppAuditFrontendResult runCppAuditFrontend(
    const CppCompileUnit& unit,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    const CppAuditFrontendOptions& options);
} // namespace topo::check
#endif
