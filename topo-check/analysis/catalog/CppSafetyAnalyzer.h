#ifndef TOPO_CHECK_CPPSAFETYANALYZER_H
#define TOPO_CHECK_CPPSAFETYANALYZER_H

#include "CppSafePatterns.h"
#include "topo/Check/ContainmentCheck.h"

#include <string>
#include <vector>

// Forward declarations
namespace topo::lsp { class ClangdBridge; }
namespace topo { class SymbolTable; }

namespace topo::check {

/// L2 whitelist-based containment analyzer using clangd semantic analysis.
/// Resolves call targets via LSP and checks them against CppSafePatterns.
class CppSafetyAnalyzer {
public:
    CppSafetyAnalyzer(lsp::ClangdBridge& bridge, const CppSafePatterns& patterns);

    /// Set build directory for preprocessor macro expansion (F4b).
    void setBuildDir(const std::string& dir) { buildDir_ = dir; }

    /// Analyze source files for containment violations.
    /// Non-external functions calling non-whitelisted targets are reported.
    /// If clangd is available: L2 semantic analysis.
    /// Supplementary: preprocessor macro expansion scan (F4b).
    CheckResult analyze(const SymbolTable& symbols,
                        const std::vector<std::string>& sourceFiles,
                        const ContainmentConfig& config);

private:
    /// Analyze a single source file, appending detected call sites.
    /// Returns true if clangd produced semantic tokens for this file
    /// (analysis ran, even if it found nothing). Returns false if tokens
    /// stayed empty after retries, signaling that L2 could not introspect
    /// this file at all — the caller must surface this as a visible
    /// warning rather than silently treating the file as clean
    /// (principle 16).
    bool analyzeFile(const std::string& filePath,
                     const SymbolTable& symbols,
                     const ContainmentConfig& config,
                     std::vector<DetectedCallSite>& callSites);

    lsp::ClangdBridge& bridge_;
    const CppSafePatterns& patterns_;
    std::string buildDir_;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPSAFETYANALYZER_H
