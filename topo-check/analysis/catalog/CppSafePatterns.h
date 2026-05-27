#ifndef TOPO_CHECK_CPPSAFEPATTERNS_H
#define TOPO_CHECK_CPPSAFEPATTERNS_H

#include <string>
#include <unordered_set>
#include <vector>

namespace topo::check {

/// Loads and queries the C++ safety whitelist (CppSafePatterns.toml).
/// Used by L2 (LSP) analysis to determine if a resolved symbol is safe.
class CppSafePatterns {
public:
    /// Load patterns from a TOML file. Returns false on parse error.
    bool load(const std::string& tomlPath);

    /// Load from the default location relative to the topo installation.
    /// Searches: $TOPO_PATTERNS_DIR, then alongside the executable.
    bool loadDefault();

    // --- Header whitelist ---

    /// Is this #include path a known safe header?
    bool isHeaderSafe(const std::string& headerName) const;

    /// Is this a known unsafe construct keyword?
    bool isConstructUnsafe(const std::string& keyword) const;

    /// Is this a known safe construct keyword?
    bool isConstructSafe(const std::string& keyword) const;

    // --- stdlib symbol whitelist (L2) ---

    /// Is this fully qualified symbol name safe?
    /// e.g., "std::vector", "std::sort", "std::chrono::steady_clock"
    bool isStdlibSymbolSafe(const std::string& qualifiedName) const;

    // --- Accessors ---
    const std::unordered_set<std::string>& safeHeaders() const { return safeHeaders_; }
    const std::unordered_set<std::string>& safeConstructs() const { return safeConstructs_; }
    const std::unordered_set<std::string>& unsafeConstructs() const { return unsafeConstructs_; }
    const std::unordered_set<std::string>& safeStdlib() const { return safeStdlib_; }

private:
    std::unordered_set<std::string> safeHeaders_;
    std::unordered_set<std::string> safeConstructs_;
    std::unordered_set<std::string> unsafeConstructs_;
    std::unordered_set<std::string> safeStdlib_;
    bool loaded_ = false;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPSAFEPATTERNS_H
