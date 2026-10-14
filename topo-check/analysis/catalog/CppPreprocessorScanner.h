#ifndef TOPO_CHECK_CPPPREPROCESSORSCANNER_H
#define TOPO_CHECK_CPPPREPROCESSORSCANNER_H

#include "../extract/CppCallSiteExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// F4b: Scans preprocessed (macro-expanded) C++ source for dangerous patterns.
/// Runs `clang -E` to expand macros, then applies L1 regex detection on
/// the expanded output. Maps results back to original source locations.
struct CppPreprocessorResult {
    std::vector<DetectedCallSite> sites;
    std::vector<std::string> errors;
    bool complete = false;
};
class CppPreprocessorScanner {
public:
    /// Set the clang executable path. If empty, searches PATH for "clang".
    void setClangPath(const std::string& path) { clangPath_ = path; }

    /// Set the build directory containing compile_commands.json.
    void setBuildDir(const std::string& dir) { buildDir_ = dir; }

    /// Scan a source file through the preprocessor and detect dangerous patterns.
    /// Returns call sites with original source locations (not preprocessed locations).
    std::vector<DetectedCallSite> scanExpanded(const std::string& filePath);
    /// L2 already resolves unexpanded calls semantically. In macro-only mode,
    /// retain newly expanded patterns without reclassifying those original calls.
    CppPreprocessorResult scanDetailed(const std::string& filePath,
                                      bool macroExpansionsOnly = false);

private:
    /// Run clang -E on the file, return preprocessed output.
    std::vector<std::string> preprocess(const std::string& filePath);

    std::string clangPath_;
    std::string buildDir_;
};

} // namespace topo::check

#endif // TOPO_CHECK_CPPPREPROCESSORSCANNER_H
