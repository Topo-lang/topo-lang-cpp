#ifndef TOPO_CHECK_CPPLSPCALLSITEEXTRACTOR_H
#define TOPO_CHECK_CPPLSPCALLSITEEXTRACTOR_H

#include "CppCallSiteExtractor.h"

#include <string>
#include <vector>

// Forward declaration
namespace topo::lsp { class ClangdBridge; }

namespace topo::check {

/// LSP-based C++ call site extractor using clangd semantic tokens and hover.
///
/// Extracts function/method call references by:
/// 1. Opening the document for clangd analysis
/// 2. Requesting semantic tokens to find call references (non-declaration tokens)
/// 3. Using hover to resolve qualified callee names
/// 4. Classifying each callee via UnsafeCatalog and CapabilityCatalog
///
/// This extractor handles what regex cannot: qualified name resolution,
/// template instantiation detection, and macro-expanded code.
/// Language escape constructs (casts, asm, etc.) are left to the
/// regex-based CppCallSiteExtractor as a supplement.
class CppLSPCallSiteExtractor {
public:
    explicit CppLSPCallSiteExtractor(lsp::ClangdBridge& bridge);

    /// Extract call sites from a single source file.
    /// Returns only function/method call references with resolved qualified names.
    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath);

private:
    lsp::ClangdBridge& bridge_;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPLSPCALLSITEEXTRACTOR_H
