#ifndef TOPO_CHECK_CPPUNSAFECATALOG_H
#define TOPO_CHECK_CPPUNSAFECATALOG_H

#include "topo/Check/CapabilityCatalog.h"
#include <string>

namespace topo::check {

/// C++ unsafe behavior catalog.
/// Classifies C++ patterns by unsafe level.
/// Level 1 (System): stdlib I/O, system calls
/// Level 2 (Dep): third-party library patterns
/// Level 3 (Input): user-input handling patterns
/// Level 4 (Escape): language escape mechanisms (reinterpret_cast, asm, raw pointers)
class CppUnsafeCatalog {
public:
    /// Classify a call site pattern (function name or qualified call).
    static UnsafeLevel classifyCall(const std::string& pattern);

    /// Classify an import/include path.
    static UnsafeLevel classifyImport(const std::string& path);
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPUNSAFECATALOG_H
