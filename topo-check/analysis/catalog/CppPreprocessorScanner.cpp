// CppPreprocessorScanner — F4b macro expansion detection.
//
// Runs clang -E to expand macros, then scans the expanded output with
// CppCallSiteExtractor regex patterns. Uses # line directives to map
// detected call sites back to their original source locations.

#include "CppPreprocessorScanner.h"
#include "CppCompileModel.h"
#include "topo/Platform/ToolResolution.h"
#include <filesystem>
#include <stdexcept>
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

std::vector<std::string> CppPreprocessorScanner::preprocess(const std::string& filePath) {
    auto clang = clangPath_.empty() ? topo::platform::llvmClangxx() : clangPath_;
    std::vector<std::vector<std::string>> commands;
    if (!buildDir_.empty()) {
        auto model = loadCppCompileModel((std::filesystem::path(buildDir_) / "compile_commands.json").string());
        if (!model.errors.empty()) throw std::runtime_error(model.errors.front());
        auto file = cppCanonicalPath(filePath);
        for (const auto& unit : model.units) {
            if (cppCanonicalPath(unit.file) != file) continue;
            if (!unit.errors.empty()) throw std::runtime_error(unit.errors.front());
            commands.push_back(unit.arguments);
        }
        if (commands.empty()) throw std::runtime_error("no compile command for " + file);
    } else {
        commands.push_back({"-x", "c++", "-std=c++17"});
    }
    std::vector<std::string> outputs;
    for (auto args : commands) {
        args.push_back("-E"); args.push_back(cppCanonicalPath(filePath));
        auto result = topo::platform::runProcessCaptureWithTimeout(clang, args, 20000);
        if (result.exitCode != 0)
            throw std::runtime_error("clang preprocessing failed (exit " + std::to_string(result.exitCode) + "): " + result.stderrOutput);
        if (result.stdoutOutput.size() > 64 * 1024 * 1024)
            throw std::runtime_error("preprocessed output exceeds 64 MiB limit");
        outputs.push_back(std::move(result.stdoutOutput));
    }
    return outputs;
}

std::vector<DetectedCallSite> CppPreprocessorScanner::scanExpanded(const std::string& filePath) {
    auto result = scanDetailed(filePath);
    if (!result.complete) throw std::runtime_error(result.errors.empty() ? "preprocessor scan incomplete" : result.errors.front());
    return std::move(result.sites);
}

CppPreprocessorResult CppPreprocessorScanner::scanDetailed(const std::string& filePath) {
    CppPreprocessorResult output;
    try {
    for (const auto& ppOutput : preprocess(filePath)) {
    auto& results = output.sites;
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
        if (!ofs) throw std::runtime_error("cannot write preprocessor scan temporary file");
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
                results.push_back(std::move(site));
            }
        }
    }
    // 5. Cleanup: TempFile's destructor removes the file.

    }
    output.complete = true;
    } catch (const std::exception& error) {
        output.errors.push_back(error.what());
    }
    return output;
}

} // namespace topo::check
