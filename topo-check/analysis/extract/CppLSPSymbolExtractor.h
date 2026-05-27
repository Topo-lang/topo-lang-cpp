#ifndef TOPO_CHECK_CPPLSPSYMBOLEXTRACTOR_H
#define TOPO_CHECK_CPPLSPSYMBOLEXTRACTOR_H

#include "topo/Check/SymbolExtractor.h"

// Forward declaration
namespace topo::lsp { class ClangdBridge; }

namespace topo::check {

/// LSP-based C++ symbol extractor using clangd semantic tokens and hover.
///
/// Extracts symbols by:
/// 1. Opening the document for clangd analysis
/// 2. Requesting semantic tokens to find declarations/definitions
/// 3. Using hover to resolve qualified names and signatures
///
/// Falls back gracefully: if clangd returns empty tokens for a file,
/// the result is simply empty (caller can fall back to regex extractor).
class CppLSPSymbolExtractor : public SymbolExtractor {
public:
    explicit CppLSPSymbolExtractor(lsp::ClangdBridge& bridge);

    std::vector<HostSymbol> extractSymbols(const std::string& filePath) override;

private:
    /// Parse return type from a hover signature like "int ns::func(args)".
    static std::string parseReturnType(const std::string& hover);

    /// Parse parameter types from a hover signature.
    static std::vector<std::string> parseParamTypes(const std::string& hover);

    /// Detect enclosing class from a qualified name like "ns::Class::method".
    static std::string detectEnclosingClass(const std::string& qualifiedName);

    lsp::ClangdBridge& bridge_;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPLSPSYMBOLEXTRACTOR_H
