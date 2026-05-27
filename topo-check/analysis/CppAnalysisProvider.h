#ifndef TOPO_CHECK_CPPANALYSISPROVIDER_H
#define TOPO_CHECK_CPPANALYSISPROVIDER_H

#include "topo/Check/LanguageAnalysisProvider.h"

#include <memory>

// Forward declaration
namespace topo::lsp { class ClangdBridge; }

namespace topo::check {

class CppAnalysisProvider : public LanguageAnalysisProvider {
public:
    ~CppAnalysisProvider() override;

    std::unique_ptr<SymbolExtractor> createSymbolExtractor() override;
    std::unique_ptr<ImportExtractor> createImportExtractor() override;
    std::unique_ptr<CallSiteExtractor> createCallSiteExtractor() override;
    std::unique_ptr<CallEdgeExtractor> createCallEdgeExtractor() override;
    std::unique_ptr<SymbolAccessExtractor> createSymbolAccessExtractor() override;
    std::vector<std::string> collectSourceFiles(
        const std::string& projectDir,
        const std::vector<std::string>& includeDirs) const override;
    std::optional<CheckResult> runDeepContainment(
        const SymbolTable& symbols,
        const std::vector<std::string>& sourceFiles,
        const ContainmentConfig& config,
        const std::string& projectDir,
        bool verbose) override;

    bool initLSP(const std::string& projectDir, bool verbose) override;
    void shutdownLSP() override;
    bool isLSPReady() const override;

private:
    std::unique_ptr<lsp::ClangdBridge> bridge_;
};

/// Factory function (avoids incomplete-type issues when constructing via make_unique).
std::unique_ptr<LanguageAnalysisProvider> createCppAnalysisProvider();

} // namespace topo::check

#endif // TOPO_CHECK_CPPANALYSISPROVIDER_H
