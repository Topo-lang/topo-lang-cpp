// CppPreprocessorScanner — F4b macro expansion detection.
//
// Runs clang -E to expand macros, then scans the expanded output with
// CppCallSiteExtractor regex patterns. Uses # line directives to map
// detected call sites back to their original source locations.

#include "CppPreprocessorScanner.h"
#include "CppUnsafeCatalog.h"
#include "topo/Check/CapabilityCatalog.h"
#include "topo/Platform/Process.h"
#include "topo/Platform/TempFile.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace topo::check {

namespace {

/// Parse # line directives from preprocessed output to build location map.
/// Format: # linenum "filename" [flags]
/// Returns: preprocessed-line-number → (original-file, original-line)
struct OriginalLocation {
    std::string file;
    int line = 0;
};

std::unordered_map<int, OriginalLocation> buildLineMap(const std::string& ppOutput) {
    std::unordered_map<int, OriginalLocation> map;
    std::istringstream stream(ppOutput);
    std::string line;
    int ppLineNum = 0;

    std::string currentFile;
    int currentOrigLine = 0;

    static const std::regex lineDirective(R"re(^#\s+(\d+)\s+"([^"]+)")re");

    while (std::getline(stream, line)) {
        ++ppLineNum;
        std::smatch m;
        if (std::regex_search(line, m, lineDirective)) {
            currentOrigLine = std::stoi(m[1].str());
            currentFile = m[2].str();
            continue;
        }
        if (!currentFile.empty()) {
            map[ppLineNum] = {currentFile, currentOrigLine};
            ++currentOrigLine;
        }
    }
    return map;
}

} // anonymous namespace

std::string CppPreprocessorScanner::preprocess(const std::string& filePath) {
    std::string clang = clangPath_.empty() ? "clang" : clangPath_;

    std::vector<std::string> args = {"-E", "-x", "c++", "-std=c++17"};

    // If build dir has compile_commands.json, use it for proper flags
    if (!buildDir_.empty()) {
        args.push_back("-p");
        args.push_back(buildDir_);
    }

    args.push_back(filePath);

    auto result = topo::platform::runProcessCapture(clang, args);
    if (result.exitCode != 0) return "";
    return result.stdoutOutput;
}

std::vector<DetectedCallSite> CppPreprocessorScanner::scanExpanded(const std::string& filePath) {
    std::vector<DetectedCallSite> results;

    // 1. Preprocess the file
    std::string ppOutput = preprocess(filePath);
    if (ppOutput.empty()) return results;

    // 2. Build line mapping (preprocessed → original)
    auto lineMap = buildLineMap(ppOutput);

    // 3. Write preprocessed output to a unique RAII temp file. The
    //    previous code wrote to a hardcoded predictable path under
    //    the POSIX temp dir — (a) broken on Windows (no such path
    //    by default), (b) a symlink-attack target that a pre-created
    //    symlink could redirect into any user-writable file, and
    //    (c) raced across topo-check workers so concurrent scans
    //    clobbered each other's preprocessed output (yielding
    //    nondeterministic call-site detection). TempFile uses
    //    O_CREAT|O_EXCL on POSIX / CREATE_NEW on Windows and removes
    //    the file on scope exit, eliminating all three failure modes.
    topo::platform::TempFile tmpFile("topo-pp-scan", ".cpp");
    {
        std::ofstream ofs(tmpFile.path());
        if (!ofs) return results;
        ofs << ppOutput;
    }
    CppCallSiteExtractor extractor;
    auto ppSites = extractor.extractCallSites(tmpFile.path().string());

    // 4. Map results back to original locations and filter
    for (auto& site : ppSites) {
        auto it = lineMap.find(site.line);
        if (it != lineMap.end()) {
            // Only include sites from the original file (not system headers)
            if (it->second.file == filePath ||
                it->second.file.find("/usr/") == std::string::npos) {
                site.file = it->second.file;
                site.line = it->second.line;
                // Mark as macro-expanded detection
                if (site.callerQualifiedName.find("<macro:") == std::string::npos) {
                    site.callerQualifiedName += " [macro-expanded]";
                }
                results.push_back(std::move(site));
            }
        }
    }
    // 5. Cleanup: TempFile's destructor removes the file.

    return results;
}

} // namespace topo::check
