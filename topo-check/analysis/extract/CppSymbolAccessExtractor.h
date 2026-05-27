#ifndef TOPO_CHECK_CPPSYMBOLACCESSEXTRACTOR_H
#define TOPO_CHECK_CPPSYMBOLACCESSEXTRACTOR_H

#include "topo/Check/SymbolAccessExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based C++ symbol access extractor used by PurityCheck.
///
/// Two-pass strategy:
///   1. Scan for file-scope globals (including `static`, `extern`, and
///      `thread_local` storage classes). Member variables and locals are
///      excluded by the scope state machine.
///   2. Inside function bodies, emit SymbolAccess{isWrite=true} for writes
///      to the detected globals (simple assignment + compound assignment +
///      `++x` / `x++` / `--x` / `x--`).
///
/// Reads are left as future work for L1 — writes are the load-bearing
/// parallel-purity signal.
class CppSymbolAccessExtractor : public SymbolAccessExtractor {
public:
    std::vector<SymbolAccess> extractSymbolAccesses(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPSYMBOLACCESSEXTRACTOR_H
