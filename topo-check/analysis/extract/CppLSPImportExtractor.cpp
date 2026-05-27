// CppLSPImportExtractor -- Clean line-based #include extraction for C++.
//
// #include is deterministic syntax (no need for regex or LSP).
// This extractor uses direct string matching with the same block-comment
// and raw-string state machine as CppImportExtractor.

#include "CppLSPImportExtractor.h"
#include "CppUnsafeCatalog.h"

#include <cctype>
#include <fstream>
#include <string>

namespace topo::check {

namespace {

/// Try to parse "#include <path>" or '#include "path"' from a line.
/// Returns the include path, or empty string if the line is not an #include.
std::string tryParseInclude(const std::string& line) {
    // Find first non-whitespace
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;

    // Must start with #
    if (pos >= line.size() || line[pos] != '#') return "";
    ++pos;

    // Skip whitespace after #
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;

    // Must be "include"
    if (pos + 7 > line.size()) return "";
    if (line.compare(pos, 7, "include") != 0) return "";
    pos += 7;

    // Must be followed by whitespace or < or "
    if (pos >= line.size()) return "";
    char next = line[pos];
    if (next != ' ' && next != '\t' && next != '<' && next != '"') return "";

    // Skip whitespace
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;

    if (pos >= line.size()) return "";

    // Extract path from <...> or "..."
    char open = line[pos];
    char close;
    if (open == '<') {
        close = '>';
    } else if (open == '"') {
        close = '"';
    } else {
        return "";
    }

    ++pos;
    size_t pathStart = pos;
    while (pos < line.size() && line[pos] != close) ++pos;
    if (pos >= line.size()) return "";

    return line.substr(pathStart, pos - pathStart);
}

} // anonymous namespace

std::vector<HostImport> CppLSPImportExtractor::extractImports(const std::string& filePath) {
    std::vector<HostImport> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    std::string line;
    int lineNum = 0;
    bool inBlockComment = false;
    bool inRawString = false;
    std::string rawDelimiter;

    while (std::getline(file, line)) {
        ++lineNum;

        // --- State machine: raw string tracking ---
        if (inRawString) {
            std::string closer = ")" + rawDelimiter + "\"";
            if (line.find(closer) != std::string::npos) {
                inRawString = false;
                rawDelimiter.clear();
            }
            continue;
        }

        // --- State machine: block comment tracking ---
        if (inBlockComment) {
            auto closePos = line.find("*/");
            if (closePos != std::string::npos) {
                inBlockComment = false;
                // #include must start at column 0 (anchored with ^), so after */
                // on the same line it would not match. Safe to continue.
            }
            continue;
        }

        // Scan for block comment / raw string openings
        {
            bool skipLine = false;
            for (size_t i = 0; i < line.size(); ++i) {
                char c = line[i];
                // Line comment -- rest of line is comment, stop scanning
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
                // Block comment start
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                    auto closePos = line.find("*/", i + 2);
                    if (closePos != std::string::npos) {
                        // Same-line block comment: skip past it
                        i = closePos + 1;
                        continue;
                    }
                    // Multiline block comment
                    inBlockComment = true;
                    skipLine = true;
                    break;
                }
                // Raw string literal start: R"delimiter(
                if (c == 'R' && i + 1 < line.size() && line[i + 1] == '"') {
                    auto parenPos = line.find('(', i + 2);
                    if (parenPos != std::string::npos) {
                        std::string delim = line.substr(i + 2, parenPos - (i + 2));
                        std::string closer = ")" + delim + "\"";
                        auto closePos = line.find(closer, parenPos + 1);
                        if (closePos != std::string::npos) {
                            // Same-line raw string: skip past it
                            i = closePos + closer.size() - 1;
                            continue;
                        }
                        // Multiline raw string
                        inRawString = true;
                        rawDelimiter = delim;
                        skipLine = true;
                        break;
                    }
                }
            }
            if (skipLine) continue;
        }

        // Try to parse #include
        std::string includePath = tryParseInclude(line);
        if (!includePath.empty()) {
            HostImport imp;
            imp.normalizedPath = includePath;
            imp.file = filePath;
            imp.line = lineNum;
            imp.unsafeLevel = CppUnsafeCatalog::classifyImport(includePath);
            results.push_back(std::move(imp));
        }
    }

    return results;
}

std::vector<HostImport> CppLSPImportExtractor::extractAll(const std::vector<std::string>& files) {
    std::vector<HostImport> results;
    for (const auto& f : files) {
        auto imports = extractImports(f);
        results.insert(results.end(),
                       std::make_move_iterator(imports.begin()),
                       std::make_move_iterator(imports.end()));
    }
    return results;
}

} // namespace topo::check
