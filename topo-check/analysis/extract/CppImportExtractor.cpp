#include "CppImportExtractor.h"
#include "CppUnsafeCatalog.h"

#include <fstream>
#include <regex>
#include <string>

namespace topo::check {

std::vector<HostImport> CppImportExtractor::extractImports(const std::string& filePath) {
    std::vector<HostImport> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    std::regex includeRegex(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
    std::string line;
    int lineNum = 0;

    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelimiter;

    while (std::getline(file, line)) {
        ++lineNum;

        // --- State machine: track block comments and raw strings ---

        if (inRawString) {
            // Look for closing )delimiter"
            std::string closer = ")" + rawDelimiter + "\"";
            if (line.find(closer) != std::string::npos) {
                inRawString = false;
                rawDelimiter.clear();
            }
            continue;
        }

        if (inBlockComment) {
            // Look for */ to end block comment
            auto closePos = line.find("*/");
            if (closePos != std::string::npos) {
                inBlockComment = false;
                // The rest of the line after */ could contain an #include,
                // but #include must start at column 0 (^\s*#), so after */
                // it would not match the regex anchored with ^. Safe to continue.
            }
            continue;
        }

        // Check if a block comment starts on this line (before any #include)
        // Scan for /* that is not inside a string literal or line comment
        {
            bool skipLine = false;
            for (size_t i = 0; i < line.size(); ++i) {
                char c = line[i];
                // Line comment — rest of line is comment, stop scanning
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
                // Block comment start
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                    // Check if it closes on the same line
                    auto closePos = line.find("*/", i + 2);
                    if (closePos != std::string::npos) {
                        // Same-line block comment: skip past it and continue scanning
                        i = closePos + 1;
                        continue;
                    }
                    // Multiline block comment starts here
                    inBlockComment = true;
                    skipLine = true;
                    break;
                }
                // Raw string literal start: R"delimiter(
                if (c == 'R' && i + 1 < line.size() && line[i + 1] == '"') {
                    auto parenPos = line.find('(', i + 2);
                    if (parenPos != std::string::npos) {
                        rawDelimiter = line.substr(i + 2, parenPos - (i + 2));
                        std::string closer = ")" + rawDelimiter + "\"";
                        auto closePos = line.find(closer, parenPos + 1);
                        if (closePos != std::string::npos) {
                            // Raw string opens and closes on same line — skip past it
                            i = closePos + closer.size() - 1;
                            rawDelimiter.clear();
                            continue;
                        }
                        // Multiline raw string
                        inRawString = true;
                        skipLine = true;
                        break;
                    }
                }
            }
            if (skipLine) continue;
        }

        std::smatch match;
        if (std::regex_search(line, match, includeRegex)) {
            HostImport imp;
            imp.normalizedPath = match[1].str();
            imp.file = filePath;
            imp.line = lineNum;
            imp.unsafeLevel = CppUnsafeCatalog::classifyImport(imp.normalizedPath);
            results.push_back(std::move(imp));
        }
    }
    return results;
}

} // namespace topo::check
