#ifndef TOPO_LSP_CLANGDBRIDGE_H
#define TOPO_LSP_CLANGDBRIDGE_H

#include "topo/LSP/LSPBridge.h"

namespace topo::lsp {

class ClangdBridge : public LSPBridge {
public:
    ClangdBridge();

    bool start(const std::string& rootUri) override;
    std::string displayName() const override { return "C++"; }

    // Start clangd subprocess with explicit paths.
    bool start(const std::string& clangdPath, const std::string& buildDir, const std::string& rootUri);

    /// Whether a usable clangd binary is reachable.
    /// Checks the bundled LLVM toolchain first (TOPO_LLVM_BINDIR) and falls
    /// back to the PATH lookup that ClangdBridge::start() uses internally,
    /// so callers (in particular tests) decide "L2 is supported" with the
    /// exact same predicate the production path uses.
    static bool isClangdAvailable();

    // Query implementations
    std::optional<SymbolResult> findDefinition(const std::string& qualifiedName,
                                               const std::vector<std::string>& cppFiles) override;

    std::vector<SymbolResult> findReferences(const std::string& qualifiedName,
                                             const std::vector<std::string>& cppFiles) override;

    std::optional<std::string> getHoverInfo(const std::string& qualifiedName,
                                            const std::vector<std::string>& cppFiles) override;

    /// Find host-language type definition for a named type.
    /// Queries clangd workspace index first; falls back to scanning header
    /// files in includeDirs for class/struct/enum/union/typedef/using
    /// declarations matching typeName.
    std::optional<SymbolResult> findTypeDefinition(const std::string& typeName,
                                                   const std::vector<std::string>& sourceFiles,
                                                   const std::vector<std::string>& includeDirs) override;

    std::string languageId() const override { return "cpp"; }
};

} // namespace topo::lsp

#endif // TOPO_LSP_CLANGDBRIDGE_H
