// CppSafetyAnalyzer — L2 whitelist-based containment analysis engine.
//
// Uses clangd semantic tokens to find call sites, resolves each target
// via hover, and checks qualified names against CppSafePatterns whitelist.
// Non-whitelisted, non-.topo-declared calls are fed through the standard
// checkContainment() pipeline for external/non-external filtering.

#include "CppSafetyAnalyzer.h"
#include "CppLSPUtils.h"
#include "CppPreprocessorScanner.h"
#include "ClangdBridge.h"
#include "CppCallSiteExtractor.h"
#include "CppImportExtractor.h"
#include "CppUnsafeCatalog.h"

#include <chrono>
#include <set>
#include <string>
#include <thread>

using topo::check::extractQualifiedName;

namespace topo::check {

CppSafetyAnalyzer::CppSafetyAnalyzer(lsp::ClangdBridge& bridge, const CppSafePatterns& patterns)
    : bridge_(bridge), patterns_(patterns) {}

CheckResult CppSafetyAnalyzer::analyze(const SymbolTable& symbols,
                                       const std::vector<std::string>& sourceFiles,
                                       const ContainmentConfig& config) {
    CheckResult result;
    if (!config.isEnabled()) return result;
    if (!bridge_.isAvailable()) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "clangd unavailable — falling back to L1 regex scanning";
        result.addDiagnostic(std::move(d));
        return result;
    }

    // Collect all L2-detected call sites, then run through checkContainment.
    // L2 does not re-check imports (L1 handles that).
    std::vector<DetectedCallSite> callSites;
    std::vector<HostImport> imports;

    int filesWithEmptyTokens = 0;
    for (const auto& file : sourceFiles) {
        if (!analyzeFile(file, symbols, config, callSites)) {
            ++filesWithEmptyTokens;
        }
    }

    // Principle 16: if clangd produced no tokens for any file (after retries),
    // do not pretend L2 ran. Surface a loud warning and let CheckRunner fall
    // through to L1. Even though F4b preprocessor scan could still find some
    // macro-hidden calls, hover-resolved L2 is the primary signal — losing it
    // means users would silently get L1-only coverage for declared L2 paths.
    if (!sourceFiles.empty() && filesWithEmptyTokens == static_cast<int>(sourceFiles.size())) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "clangd returned no semantic tokens for any of " +
                    std::to_string(sourceFiles.size()) +
                    " source file(s) after retries — L2 cannot run, falling back to L1";
        result.addDiagnostic(std::move(d));
        return result;
    }

    // F4b: Supplementary macro expansion scan via clang preprocessor.
    // Catches dangerous calls hidden inside macros from system/third-party headers.
    {
        CppPreprocessorScanner ppScanner;
        ppScanner.setBuildDir(buildDir_);
        for (const auto& file : sourceFiles) {
            auto ppSites = ppScanner.scanExpanded(file);
            callSites.insert(callSites.end(), ppSites.begin(), ppSites.end());
        }
    }

    // Deduplicate: same file+line call sites from L2 and preprocessor
    {
        std::set<std::pair<std::string, int>> seen;
        std::vector<DetectedCallSite> deduped;
        for (auto& site : callSites) {
            auto key = std::make_pair(site.file + "::" + site.calleePattern, site.line);
            if (seen.insert(key).second) {
                deduped.push_back(std::move(site));
            }
        }
        callSites = std::move(deduped);
    }

    // Use the standard containment check with L2-resolved call sites
    checkContainment(symbols, imports, callSites, config, result);

    // Surface partial-extraction warning if some (but not all) files lacked tokens.
    if (filesWithEmptyTokens > 0) {
        CheckDiagnostic d;
        d.severity = Severity::Warning;
        d.check = "containment-l2";
        d.message = "clangd returned no semantic tokens for " +
                    std::to_string(filesWithEmptyTokens) + " of " +
                    std::to_string(sourceFiles.size()) +
                    " source file(s) — those files were covered only by F4b preprocessor scanning";
        result.addDiagnostic(std::move(d));
    }

    // Mark as real L2 result so CheckRunner does not fall through to L1.
    // The marker carries honest extraction stats so users can distinguish
    // "L2 ran cleanly" from "L2 ran but found nothing because LSP failed".
    {
        CheckDiagnostic d;
        d.severity = Severity::Note;
        d.check = "containment";
        d.message = "L2 deep analysis completed (" +
                    std::to_string(static_cast<int>(sourceFiles.size()) - filesWithEmptyTokens) +
                    "/" + std::to_string(sourceFiles.size()) + " file(s), " +
                    std::to_string(callSites.size()) + " call site(s))";
        result.addDiagnostic(std::move(d));
    }

    return result;
}

bool CppSafetyAnalyzer::analyzeFile(const std::string& filePath,
                                     const SymbolTable& symbols,
                                     const ContainmentConfig& /*config*/,
                                     std::vector<DetectedCallSite>& callSites) {
    // 1. Open document for clangd analysis
    bridge_.openDocument(filePath);
    struct DocGuard {
        lsp::ClangdBridge& b;
        const std::string& path;
        ~DocGuard() { b.closeDocument(path); }
    } guard{bridge_, filePath};

    // 2. Get semantic tokens (retry: clangd may need time to analyze after didOpen)
    auto tokens = bridge_.getSemanticTokens(filePath);
    for (int retry = 0; tokens.empty() && retry < 3; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds{500 * (retry + 1)});
        tokens = bridge_.getSemanticTokens(filePath);
    }
    if (tokens.empty()) {
        return false;
    }

    // Fetch the document outline once so every call site can be attributed
    // to its real enclosing function. Without this the synthetic
    // `<l2:file:line>` placeholder breaks isExternalCaller() for every L2
    // call site (without real enclosing-function attribution, the L2
    // synthetic-caller placeholder defeats isExternalCaller()).
    auto docSymbols = bridge_.getDocumentSymbols(filePath);

    // 3. For each function/method call token, resolve and check
    for (const auto& token : tokens) {
        // Only interested in function/method references (not declarations/definitions)
        if (token.type != "function" && token.type != "method") continue;
        if (token.modifiers.find("declaration") != std::string::npos ||
            token.modifiers.find("definition") != std::string::npos) continue;

        // Resolve the call target via hover
        auto hover = bridge_.getHoverAt(filePath, token.line, token.column);
        if (!hover) continue;

        // Extract qualified name from hover response
        std::string qualifiedName = extractQualifiedName(*hover);
        if (qualifiedName.empty()) continue;

        // Check if this call comes from a safe standard library header.
        // clangd hover includes "provided by <header>" for stdlib symbols.
        // This is more reliable than qualified name matching because clangd
        // hover format splits namespace info across separate lines.
        std::string header = extractProvidedByHeader(*hover);
        if (!header.empty() && patterns_.isHeaderSafe(header)) continue;

        // Check if this is a safe stdlib call (by qualified name)
        if (patterns_.isStdlibSymbolSafe(qualifiedName)) continue;

        // Check if this is a .topo-declared function (project code calling project code is OK)
        bool isDeclared = false;
        for (const auto& [name, fn] : symbols.functions()) {
            if (fn.qualifiedName == qualifiedName || fn.simpleName == qualifiedName) {
                isDeclared = true;
                break;
            }
        }
        if (isDeclared) continue;

        // Resolve the enclosing function via documentSymbol so external
        // functions are recognized by isExternalCaller(). The synthetic
        // placeholder survives only as a last-resort fallback when the
        // outline is empty.
        std::string callerQN = lsp::LSPBridge::findEnclosingFunction(
            docSymbols, token.line, "::");
        if (callerQN.empty()) {
            callerQN = "<l2:" + filePath + ":" +
                       std::to_string(token.line + 1) + ">";
        }

        // Not in whitelist and not .topo-declared -> report as unsafe
        DetectedCallSite site;
        site.calleePattern = qualifiedName;
        site.callerQualifiedName = callerQN;
        site.capability = std::nullopt;
        // Classify via CppUnsafeCatalog; default to System for unknown external calls
        auto catalogLevel = CppUnsafeCatalog::classifyCall(qualifiedName);
        site.unsafeLevel = (catalogLevel != UnsafeLevel::Safe) ? catalogLevel : UnsafeLevel::System;
        site.file = filePath;
        site.line = token.line + 1;  // semantic tokens are 0-based, diagnostics are 1-based
        callSites.push_back(std::move(site));
    }

    return true;
}

} // namespace topo::check
