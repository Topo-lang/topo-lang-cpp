#ifndef TOPO_CHECK_CPPIMPORTEXTRACTOR_H
#define TOPO_CHECK_CPPIMPORTEXTRACTOR_H

#include "topo/Check/ImportExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// Extracts #include directives from C++ source files.
class CppImportExtractor : public ImportExtractor {
public:
    /// Extract all #include paths from a single file.
    std::vector<HostImport> extractImports(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPIMPORTEXTRACTOR_H
