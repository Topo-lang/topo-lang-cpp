#ifndef TOPO_CHECK_CPPSTUBGENERATOR_H
#define TOPO_CHECK_CPPSTUBGENERATOR_H

#include "topo/Check/StubGenerator.h"

#include <string>

namespace topo::check {

/// C++ implementation of StubGenerator.
/// Finds function definitions in C++ source files by name matching,
/// then replaces the body using brace-balancing.
class CppStubGenerator : public StubGenerator {
public:
    StubResult stubFunction(const std::string& filePath, const std::string& funcName) override;

    bool restoreFile(const std::string& filePath, const StubResult& result) override;

    /// Find the position of a function body in C++ source text.
    /// Returns the index of the opening '{' of the function body,
    /// or std::string::npos if not found.
    static size_t findFunctionBodyStart(const std::string& source, const std::string& funcName);

    /// Find the matching closing '}' for a given opening '{' position.
    /// Uses brace-balancing, respecting string literals and comments.
    /// Returns the index of the closing '}', or std::string::npos on failure.
    static size_t findMatchingBrace(const std::string& source, size_t openPos);

    /// Determine if a function returns void by examining the text before
    /// the function body opening brace.
    static bool isVoidReturn(const std::string& source, size_t bodyStart);
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPSTUBGENERATOR_H
