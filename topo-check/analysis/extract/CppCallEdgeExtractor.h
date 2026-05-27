#ifndef TOPO_CHECK_CPPCALLEDGEEXTRACTOR_H
#define TOPO_CHECK_CPPCALLEDGEEXTRACTOR_H

#include "topo/Check/CallEdgeExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based C++ call edge extractor used by StageIsolationCheck and
/// VisibilityCheck. Scans function bodies for `identifier(...)` and
/// `scope::identifier(...)` calls and emits caller→callee edges qualified by
/// the enclosing namespace/class scope (mirrors CppCallSiteExtractor's
/// scope-tracking state machine).
class CppCallEdgeExtractor : public CallEdgeExtractor {
public:
    std::vector<CallEdge> extractCallEdges(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPCALLEDGEEXTRACTOR_H
