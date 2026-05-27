#ifndef TOPO_CHECK_CPPLSPIMPORTEXTRACTOR_H
#define TOPO_CHECK_CPPLSPIMPORTEXTRACTOR_H

#include "CppImportExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// Clean line-based C++ #include extractor.
///
/// #include is deterministic syntax -- no need for LSP or regex.
/// This extractor does the same job as CppImportExtractor but with
/// a cleaner, more maintainable implementation:
///   - Direct string matching instead of regex
///   - Same block-comment and raw-string state machine
///   - UnsafeCatalog classification for each import
///
/// Functionally equivalent to CppImportExtractor.
class CppLSPImportExtractor {
public:
    /// Extract all #include paths from a single file.
    std::vector<HostImport> extractImports(const std::string& filePath);

    /// Extract imports from multiple files.
    std::vector<HostImport> extractAll(const std::vector<std::string>& files);
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPLSPIMPORTEXTRACTOR_H
