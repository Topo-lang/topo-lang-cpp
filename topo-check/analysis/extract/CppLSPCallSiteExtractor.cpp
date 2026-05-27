// CppLSPCallSiteExtractor -- LSP-based C++ call site extraction via clangd.
//
// Uses semantic tokens to find function/method call references (tokens
// without "declaration"/"definition" modifier), then resolves each
// via hover to get the qualified callee name for UnsafeCatalog classification.

#include "CppLSPCallSiteExtractor.h"
#include "CppLSPUtils.h"
#include "CppUnsafeCatalog.h"
#include "ClangdBridge.h"
#include "topo/Check/CapabilityCatalog.h"

#include <string>
#include <vector>

namespace topo::check {

CppLSPCallSiteExtractor::CppLSPCallSiteExtractor(lsp::ClangdBridge& bridge)
    : bridge_(bridge) {}

std::vector<DetectedCallSite> CppLSPCallSiteExtractor::extractCallSites(const std::string& filePath) {
    std::vector<DetectedCallSite> results;

    if (!bridge_.isAvailable()) return results;

    // 1. Open document for clangd analysis
    bridge_.openDocument(filePath);
    struct DocGuard {
        lsp::ClangdBridge& b;
        const std::string& path;
        ~DocGuard() { b.closeDocument(path); }
    } guard{bridge_, filePath};

    // 2. Get semantic tokens
    auto tokens = bridge_.getSemanticTokens(filePath);
    if (tokens.empty()) {
        return results;
    }

    // 3. Process call reference tokens (function/method without declaration/definition)
    for (const auto& token : tokens) {
        // Only interested in function/method references (call sites)
        if (token.type != "function" && token.type != "method") continue;

        // Skip declarations and definitions -- those are symbol definitions, not calls
        if (hasModifier(token.modifiers, "declaration") ||
            hasModifier(token.modifiers, "definition")) {
            continue;
        }

        // 4. Resolve the call target via hover
        auto hover = bridge_.getHoverAt(filePath, token.line, token.column);
        if (!hover) continue;

        // Extract qualified name from hover response
        std::string qualifiedName = extractQualifiedName(*hover);
        if (qualifiedName.empty()) continue;

        // 5. Classify via UnsafeCatalog (primary gate)
        auto unsafeLevel = CppUnsafeCatalog::classifyCall(qualifiedName);
        if (unsafeLevel == UnsafeLevel::Safe) {
            // Also try with just the simple name (unqualified) for broader matching
            auto lastSep = qualifiedName.rfind("::");
            if (lastSep != std::string::npos) {
                std::string simpleName = qualifiedName.substr(lastSep + 2);
                unsafeLevel = CppUnsafeCatalog::classifyCall(simpleName);
            }
        }

        // Only report non-safe call sites
        if (unsafeLevel == UnsafeLevel::Safe) continue;

        // 6. Classify capability (optional)
        auto capability = classifyApiCall(qualifiedName);
        if (!capability) {
            auto lastSep = qualifiedName.rfind("::");
            if (lastSep != std::string::npos) {
                capability = classifyApiCall(qualifiedName.substr(lastSep + 2));
            }
        }

        // 7. Build DetectedCallSite
        DetectedCallSite site;
        site.calleePattern = qualifiedName;
        site.callerQualifiedName = "<lsp:" + filePath + ":" + std::to_string(token.line + 1) + ">";
        site.capability = capability;
        site.unsafeLevel = unsafeLevel;
        site.file = filePath;
        site.line = token.line + 1; // semantic tokens are 0-based, diagnostics are 1-based
        results.push_back(std::move(site));
    }

    return results;
}

} // namespace topo::check
