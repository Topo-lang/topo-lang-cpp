#include "CppAnalysisProvider.h"
#include "CppCallEdgeExtractor.h"
#include "CppCallSiteExtractor.h"
#include "CppImportExtractor.h"
#include "CppSafePatterns.h"
#include "CppSafetyAnalyzer.h"
#include "CppSymbolAccessExtractor.h"
#include "CppSymbolExtractor.h"
#include "ClangdBridge.h"
#include "CppLSPSymbolExtractor.h"
#include "CppLSPCallSiteExtractor.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>

namespace topo::check {

namespace {

class CppMergingCallSiteExtractor : public CallSiteExtractor {
public:
    explicit CppMergingCallSiteExtractor(lsp::ClangdBridge& bridge)
        : lspExtractor_(bridge) {}

    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath) override {
        auto callSites = lspExtractor_.extractCallSites(filePath);

        auto regexSites = regexExtractor_.extractCallSites(filePath);
        for (auto& site : regexSites) {
            if (site.unsafeLevel == UnsafeLevel::Escape ||
                site.unsafeLevel == UnsafeLevel::System) {
                bool duplicate = false;
                for (const auto& existing : callSites) {
                    if (existing.callerQualifiedName == site.callerQualifiedName &&
                        existing.calleePattern == site.calleePattern &&
                        existing.file == site.file &&
                        existing.line == site.line) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    callSites.push_back(std::move(site));
                }
            }
        }
        return callSites;
    }

private:
    CppLSPCallSiteExtractor lspExtractor_;
    CppCallSiteExtractor regexExtractor_;
};

} // anonymous namespace

CppAnalysisProvider::~CppAnalysisProvider() {
    shutdownLSP();
}

bool CppAnalysisProvider::initLSP(const std::string& projectDir, bool /*verbose*/) {
    namespace fs = std::filesystem;
    if (bridge_ && bridge_->isAvailable()) return true;

    auto bridge = std::make_unique<lsp::ClangdBridge>();
    std::string buildDir = (fs::path(projectDir) / "build").string();
    std::string rootUri = "file://" + fs::canonical(projectDir).string();

    if (!bridge->start("", buildDir, rootUri)) {
        return false;
    }

    if (!bridge->isAvailable()) {
        bridge->stop();
        return false;
    }

    if (!bridge->waitForIndex(std::chrono::milliseconds{30000})) {
        std::cerr << "[topo-lsp] clangd index not ready after 30s, L2 analysis may be incomplete\n";
    }

    bridge_ = std::move(bridge);
    return true;
}

void CppAnalysisProvider::shutdownLSP() {
    if (bridge_) {
        bridge_->stop();
        bridge_.reset();
    }
}

bool CppAnalysisProvider::isLSPReady() const {
    return bridge_ && bridge_->isAvailable();
}

std::unique_ptr<SymbolExtractor> CppAnalysisProvider::createSymbolExtractor() {
    if (isLSPReady()) {
        return std::make_unique<CppLSPSymbolExtractor>(*bridge_);
    }
    return std::make_unique<CppSymbolExtractor>();
}

std::unique_ptr<ImportExtractor> CppAnalysisProvider::createImportExtractor() {
    return std::make_unique<CppImportExtractor>();
}

std::unique_ptr<CallSiteExtractor> CppAnalysisProvider::createCallSiteExtractor() {
    if (isLSPReady()) {
        return std::make_unique<CppMergingCallSiteExtractor>(*bridge_);
    }
    return std::make_unique<CppCallSiteExtractor>();
}

std::unique_ptr<CallEdgeExtractor> CppAnalysisProvider::createCallEdgeExtractor() {
    // L1-only — no LSP/L2 merging layer.
    return std::make_unique<CppCallEdgeExtractor>();
}

std::unique_ptr<SymbolAccessExtractor> CppAnalysisProvider::createSymbolAccessExtractor() {
    return std::make_unique<CppSymbolAccessExtractor>();
}

std::vector<std::string> CppAnalysisProvider::collectSourceFiles(
    const std::string& projectDir,
    const std::vector<std::string>& includeDirs) const {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    std::vector<fs::path> searchDirs = {
        fs::path(projectDir) / "src",
        fs::path(projectDir)};
    for (const auto& incDir : includeDirs) {
        searchDirs.push_back(fs::path(incDir));
    }
    std::set<std::string> seen;
    auto addIfSource = [&](const fs::path& p) {
        auto ext = p.extension().string();
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
            ext == ".h" || ext == ".hpp" || ext == ".hxx") {
            std::string path = p.string();
            if (seen.insert(path).second) {
                files.push_back(path);
            }
        }
    };
    for (const auto& dir : searchDirs) {
        std::error_code ec;
        // [build].sources entries are commonly plain files ("main.cpp") and
        // arrive here verbatim; analyze them as single TUs. Feeding a file to
        // recursive_directory_iterator throws ("Not a directory") and aborted
        // the checker before this guard.
        if (fs::is_regular_file(dir, ec)) {
            addIfSource(dir);
            continue;
        }
        if (!fs::exists(dir, ec)) continue;
        // Non-throwing iteration (error_code construction + increment, same
        // pattern as CheckRunner::discoverRelevantFiles and the Rust
        // provider): an unreadable or vanishing entry must degrade to
        // skipping it, never abort the process.
        fs::recursive_directory_iterator it(dir, ec);
        if (ec) continue;
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            addIfSource(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::optional<CheckResult> CppAnalysisProvider::runDeepContainment(
    const SymbolTable& symbols,
    const std::vector<std::string>& sourceFiles,
    const ContainmentConfig& config,
    const std::string& projectDir,
    bool verbose) {
    namespace fs = std::filesystem;
    CheckResult result;

    CppSafePatterns patterns;
    if (!patterns.loadDefault()) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "CppSafePatterns.toml not found — cannot run L2 analysis";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (!isLSPReady()) {
        initLSP(projectDir, verbose);
    }
    if (!isLSPReady()) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "clangd unavailable — falling back to L1";
        result.addDiagnostic(std::move(d));
        return result;
    }

    if (verbose) {
        std::cerr << "L2 containment: clangd started, analyzing...\n";
    }

    std::string buildDir = (fs::path(projectDir) / "build").string();
    CppSafetyAnalyzer analyzer(*bridge_, patterns);
    analyzer.setBuildDir(buildDir);
    result = analyzer.analyze(symbols, sourceFiles, config);

    return result;
}

std::unique_ptr<LanguageAnalysisProvider> createCppAnalysisProvider() {
    return std::unique_ptr<LanguageAnalysisProvider>(new CppAnalysisProvider());
}

} // namespace topo::check
