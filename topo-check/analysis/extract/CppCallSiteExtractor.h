#ifndef TOPO_CHECK_CPPCALLSITEEXTRACTOR_H
#define TOPO_CHECK_CPPCALLSITEEXTRACTOR_H

#include "topo/Check/CallSiteExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// Extracts external API call sites from C++ source files using
/// regex-based scanning with braceDepth/scope tracking.
class CppCallSiteExtractor : public CallSiteExtractor {
public:
    /// Extract call sites matching the CapabilityCatalog API list from a single file.
    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPCALLSITEEXTRACTOR_H
