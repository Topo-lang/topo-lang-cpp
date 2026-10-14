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
#include <map>
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
    bool systemHeader = false;
};

std::string decodeLineMarkerFile(const std::string& escaped) {
    std::string file;
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '\\') { file += escaped[i]; continue; }
        if (++i == escaped.size()) throw std::runtime_error("unfinished escape in preprocessor filename");
        const char c = escaped[i];
        if (c >= '0' && c <= '7') {
            unsigned byte = static_cast<unsigned>(c - '0');
            for (int digit = 1; digit < 3 && i + 1 < escaped.size() &&
                 escaped[i + 1] >= '0' && escaped[i + 1] <= '7'; ++digit)
                byte = byte * 8 + static_cast<unsigned>(escaped[++i] - '0');
            if (byte == 0 || byte > 255) throw std::runtime_error("invalid byte in preprocessor filename");
            file += static_cast<char>(byte);
        } else {
            switch (c) {
            case '\\': case '"': case '\'': case '?': file += c; break;
            case 'a': file += '\a'; break;
            case 'b': file += '\b'; break;
            case 'f': file += '\f'; break;
            case 'n': file += '\n'; break;
            case 'r': file += '\r'; break;
            case 't': file += '\t'; break;
            case 'v': file += '\v'; break;
            default: throw std::runtime_error("unsupported escape in preprocessor filename");
            }
        }
    }
    return file;
}

std::unordered_map<int, OriginalLocation> buildLineMap(const std::string& ppOutput) {
    std::unordered_map<int, OriginalLocation> map;
    std::istringstream stream(ppOutput);
    std::string line;
    int ppLineNum = 0;

    std::string currentFile;
    int currentOrigLine = 0;
    bool currentSystemHeader = false;

    static const std::regex lineDirective(R"re(^#\s+(\d+)\s+"((?:[^"\\]|\\.)*)"(.*)$)re");

    while (std::getline(stream, line)) {
        ++ppLineNum;
        std::smatch m;
        if (std::regex_search(line, m, lineDirective)) {
            currentOrigLine = std::stoi(m[1].str());
            currentFile = decodeLineMarkerFile(m[2].str());
            // Clang's marker flag 3 identifies system-header tokens regardless
            // of SDK/install path. Expansion sites in project code have their
            // own non-system markers, even when the macro came from a system header.
            currentSystemHeader = false;
            std::istringstream flags(m[3].str());
            int flag;
            while (flags >> flag) if (flag == 3) currentSystemHeader = true;
            continue;
        }
        if (!currentFile.empty()) {
            map[ppLineNum] = {currentFile, currentOrigLine, currentSystemHeader};
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

CppPreprocessorResult CppPreprocessorScanner::scanDetailed(const std::string& filePath,
                                                         bool macroExpansionsOnly) {
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
        // Preserve line numbers while excluding system implementations from
        // lexical scope tracking and scanning. Project expansion sites survive.
        std::istringstream lines(ppOutput);
        std::string line;
        int number = 0;
        while (std::getline(lines, line)) {
            const auto location = lineMap.find(++number);
            if (location != lineMap.end() && !location->second.systemHeader) ofs << line;
            ofs << '\n';
        }
    }
    CppCallSiteExtractor extractor;
    auto ppSites = extractor.extractCallSites(tmpFile.path().string());
    using SiteKey = std::pair<int, std::string>;
    std::map<std::string, std::map<SiteKey, size_t>> unexpandedSites;

    // 4. Map results back to original locations and filter
    for (auto& site : ppSites) {
        auto it = lineMap.find(site.line);
        if (it == lineMap.end() || it->second.systemHeader) continue;
        site.file = it->second.file;
        site.line = it->second.line;
        if (macroExpansionsOnly) {
            auto [original, inserted] = unexpandedSites.try_emplace(site.file);
            if (inserted) {
                for (const auto& rawSite : extractor.extractCallSites(site.file))
                    ++original->second[{rawSite.line, rawSite.calleePattern}];
            }
            // Subtract occurrences, not whole lines or callee names: a direct
            // call and a dangerous macro expansion can share both on one line.
            auto& remaining = original->second[{site.line, site.calleePattern}];
            if (remaining > 0) { --remaining; continue; }
        }
        results.push_back(std::move(site));
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
