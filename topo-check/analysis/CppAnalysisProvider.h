#ifndef TOPO_CHECK_CPPANALYSISPROVIDER_H
#define TOPO_CHECK_CPPANALYSISPROVIDER_H

#include "topo/Check/LanguageAnalysisProvider.h"

#include <memory>

// Forward declaration
namespace topo::lsp { class ClangdBridge; }

namespace topo::check {
struct CppCompileReadPolicy;

class CppAnalysisProvider : public LanguageAnalysisProvider {
public:
    ~CppAnalysisProvider() override;
    bool configureAuditExecution(const AuditExecutionOptions&, std::string& diagnostic) override;
    void configureAnalysis(const std::string& projectDir, const std::string& projectModelPath) override;
    AuditFacts auditFacts(const std::vector<std::string>& sourceFiles) const override;
    std::vector<CheckDiagnostic> analysisDiagnostics() const override;
    nlohmann::json auditCacheIdentity(const std::string& projectDir, const std::string& modelPath) const override;
    nlohmann::json exportAuditCache() const override;
    bool importAuditCache(const nlohmann::json& payload) override;

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
    struct AuditSnapshot;
    std::shared_ptr<AuditSnapshot> audit_;
    std::string projectDir_, modelPath_;
    bool requireAuditIsolation_ = false;
    std::vector<std::string> auditInputRoots_;
    mutable nlohmann::json expectedAuditExecution_;
    mutable std::string expectedAuditProject_, expectedAuditModelPath_;
    mutable std::shared_ptr<CppCompileReadPolicy> auditReadPolicy_;
    void loadAuditSnapshot();
    void consumeAuditPayload(const nlohmann::json& payload, const std::string& binary);
};

/// Factory function (avoids incomplete-type issues when constructing via make_unique).
std::unique_ptr<LanguageAnalysisProvider> createCppAnalysisProvider();

} // namespace topo::check

#endif // TOPO_CHECK_CPPANALYSISPROVIDER_H
