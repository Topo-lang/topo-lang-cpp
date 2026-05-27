#ifndef TOPO_CHECK_CPPSYMBOLEXTRACTOR_H
#define TOPO_CHECK_CPPSYMBOLEXTRACTOR_H

#include "topo/Check/SymbolExtractor.h"

namespace topo::check {

/// C++ implementation of SymbolExtractor.
/// Uses regex-based line scanning with namespace/class scope tracking.
class CppSymbolExtractor : public SymbolExtractor {
public:
    std::vector<HostSymbol> extractSymbols(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPSYMBOLEXTRACTOR_H
