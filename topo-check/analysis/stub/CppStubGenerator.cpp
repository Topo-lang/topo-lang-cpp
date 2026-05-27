// CppStubGenerator — Stub function bodies in C++ source files.
//
// Strategy:
// 1. Read the source file
// 2. Search for the function name followed by '(' to locate the definition
// 3. Skip past the parameter list and any trailing qualifiers to find '{'
// 4. Use brace-balancing to find the complete body
// 5. Replace the body with a trivial stub: "{ }" or "{ return {}; }"
// 6. Write the modified source back
// 7. Preserve original content for restoration

#include "CppStubGenerator.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace topo::check {

namespace {

/// Read entire file into string.
bool readFile(const std::string& path, std::string& content) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    content = ss.str();
    return true;
}

/// Write string to file, replacing contents.
bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs << content;
    return ofs.good();
}

/// Check if position is inside a C-style comment or string literal.
/// This is a simplified check — it scans from the start up to `pos`.
bool isInsideStringOrComment(const std::string& source, size_t pos) {
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inString = false;
    bool inChar = false;
    char prev = 0;

    for (size_t i = 0; i < pos && i < source.size(); ++i) {
        char c = source[i];

        if (inLineComment) {
            if (c == '\n') inLineComment = false;
            prev = c;
            continue;
        }
        if (inBlockComment) {
            if (prev == '*' && c == '/') inBlockComment = false;
            prev = c;
            continue;
        }
        if (inString) {
            if (c == '"' && prev != '\\') inString = false;
            prev = c;
            continue;
        }
        if (inChar) {
            if (c == '\'' && prev != '\\') inChar = false;
            prev = c;
            continue;
        }

        if (c == '/' && i + 1 < source.size()) {
            if (source[i + 1] == '/') {
                inLineComment = true;
                prev = c;
                continue;
            }
            if (source[i + 1] == '*') {
                inBlockComment = true;
                prev = c;
                continue;
            }
        }
        if (c == '"') {
            inString = true;
            prev = c;
            continue;
        }
        if (c == '\'') {
            inChar = true;
            prev = c;
            continue;
        }

        prev = c;
    }

    return inLineComment || inBlockComment || inString || inChar;
}

} // anonymous namespace

size_t CppStubGenerator::findFunctionBodyStart(const std::string& source, const std::string& funcName) {
    // Search for all occurrences of funcName followed by '('.
    // We need to ensure it's a function definition (has a body), not just a
    // declaration or call. The pattern is: funcName '(' ... ')' ... '{'
    size_t searchStart = 0;

    while (searchStart < source.size()) {
        size_t namePos = source.find(funcName, searchStart);
        if (namePos == std::string::npos) return std::string::npos;

        // Verify it's a whole word match: character before must not be
        // alphanumeric or underscore
        if (namePos > 0) {
            char before = source[namePos - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_') {
                searchStart = namePos + funcName.size();
                continue;
            }
        }

        // Character after must be '(' or whitespace before '('
        size_t afterName = namePos + funcName.size();
        if (afterName >= source.size()) return std::string::npos;

        // Skip to '('
        size_t parenPos = afterName;
        while (parenPos < source.size() && std::isspace(static_cast<unsigned char>(source[parenPos])))
            ++parenPos;

        if (parenPos >= source.size() || source[parenPos] != '(') {
            searchStart = afterName;
            continue;
        }

        // Skip inside string/comment matches
        if (isInsideStringOrComment(source, namePos)) {
            searchStart = afterName;
            continue;
        }

        // Find matching ')' using paren-balancing
        int parenDepth = 1;
        size_t i = parenPos + 1;
        while (i < source.size() && parenDepth > 0) {
            char c = source[i];
            if (c == '(')
                ++parenDepth;
            else if (c == ')')
                --parenDepth;
            else if (c == '"' || c == '\'') {
                // Skip string/char literal
                char quote = c;
                ++i;
                while (i < source.size() && source[i] != quote) {
                    if (source[i] == '\\') ++i; // skip escaped char
                    ++i;
                }
            } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                // Skip line comment
                while (i < source.size() && source[i] != '\n')
                    ++i;
            } else if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
                    ++i;
                ++i; // skip '/'
            }
            ++i;
        }

        if (parenDepth != 0) {
            searchStart = i;
            continue;
        }

        // Now skip past any trailing qualifiers: const, noexcept, override,
        // -> return_type, etc. until we find '{' or ';'
        size_t afterParen = i;
        while (afterParen < source.size()) {
            char c = source[afterParen];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++afterParen;
                continue;
            }
            if (c == '{') {
                return afterParen;
            }
            if (c == ';') {
                // This is a declaration, not a definition
                break;
            }
            // Skip keywords and identifiers (const, noexcept, override, final, etc.)
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                while (afterParen < source.size() &&
                       (std::isalnum(static_cast<unsigned char>(source[afterParen])) || source[afterParen] == '_'))
                    ++afterParen;
                continue;
            }
            // Skip '->' for trailing return type
            if (c == '-' && afterParen + 1 < source.size() && source[afterParen + 1] == '>') {
                afterParen += 2;
                // Skip the return type (identifiers, colons, angle brackets, etc.)
                int angleBraceDepth = 0;
                while (afterParen < source.size()) {
                    char tc = source[afterParen];
                    if (tc == '<') {
                        ++angleBraceDepth;
                        ++afterParen;
                        continue;
                    }
                    if (tc == '>') {
                        --angleBraceDepth;
                        ++afterParen;
                        continue;
                    }
                    if (angleBraceDepth > 0) {
                        ++afterParen;
                        continue;
                    }
                    if (tc == '{' || tc == ';') break;
                    ++afterParen;
                }
                continue;
            }
            // Skip scope resolution '::'
            if (c == ':' && afterParen + 1 < source.size() && source[afterParen + 1] == ':') {
                afterParen += 2;
                continue;
            }
            // Skip template angle brackets in return type
            if (c == '<') {
                int depth = 1;
                ++afterParen;
                while (afterParen < source.size() && depth > 0) {
                    if (source[afterParen] == '<')
                        ++depth;
                    else if (source[afterParen] == '>')
                        --depth;
                    ++afterParen;
                }
                continue;
            }
            // Skip other punctuation like '&', '*', ','
            if (c == '&' || c == '*' || c == ',') {
                ++afterParen;
                continue;
            }
            // Unrecognized — stop
            break;
        }

        searchStart = afterParen;
    }

    return std::string::npos;
}

size_t CppStubGenerator::findMatchingBrace(const std::string& source, size_t openPos) {
    if (openPos >= source.size() || source[openPos] != '{') return std::string::npos;

    int depth = 1;
    size_t i = openPos + 1;

    while (i < source.size() && depth > 0) {
        char c = source[i];

        // Handle string literals
        if (c == '"') {
            ++i;
            while (i < source.size() && source[i] != '"') {
                if (source[i] == '\\') ++i;
                ++i;
            }
            if (i < source.size()) ++i;
            continue;
        }

        // Handle char literals
        if (c == '\'') {
            ++i;
            while (i < source.size() && source[i] != '\'') {
                if (source[i] == '\\') ++i;
                ++i;
            }
            if (i < source.size()) ++i;
            continue;
        }

        // Handle line comments
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n')
                ++i;
            continue;
        }

        // Handle block comments
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/'))
                ++i;
            i += 2; // skip */
            continue;
        }

        if (c == '{')
            ++depth;
        else if (c == '}')
            --depth;

        ++i;
    }

    return (depth == 0) ? (i - 1) : std::string::npos;
}

bool CppStubGenerator::isVoidReturn(const std::string& source, size_t bodyStart) {
    // Look backwards from the function body start to find the return type.
    // Skip past function name + params + qualifiers to find the type keyword.
    // Simple heuristic: search backwards for "void" as the return type.

    // Find the start of this line area (go back past params)
    // We look for "void" preceding the function name on the same definition line.
    size_t searchEnd = bodyStart;
    // Go backwards to find the function definition start (look for a line
    // that starts the return type, roughly within 200 chars before '{')
    size_t searchStart = (bodyStart > 300) ? (bodyStart - 300) : 0;

    std::string region = source.substr(searchStart, searchEnd - searchStart);

    // Find last occurrence of "void" in the region that's a whole word
    size_t pos = region.rfind("void");
    while (pos != std::string::npos) {
        // Check word boundary
        bool leftOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(region[pos - 1]));
        bool rightOk = (pos + 4 >= region.size()) || !std::isalnum(static_cast<unsigned char>(region[pos + 4]));
        if (leftOk && rightOk) {
            // Make sure there's no '(' between "void" and our search area start
            // (which would mean "void" is a parameter type, not the return type).
            // Check that we're before the function name/params.
            size_t parenPos = region.find('(', pos);
            if (parenPos != std::string::npos) {
                return true;
            }
        }
        if (pos == 0) break;
        pos = region.rfind("void", pos - 1);
    }

    return false;
}

StubResult CppStubGenerator::stubFunction(const std::string& filePath, const std::string& funcName) {
    StubResult result;

    if (!readFile(filePath, result.originalContent)) {
        result.error = "failed to read file: " + filePath;
        return result;
    }

    size_t bodyStart = findFunctionBodyStart(result.originalContent, funcName);
    if (bodyStart == std::string::npos) {
        result.error = "function '" + funcName + "' not found in " + filePath;
        return result;
    }

    size_t bodyEnd = findMatchingBrace(result.originalContent, bodyStart);
    if (bodyEnd == std::string::npos) {
        result.error = "unmatched brace for function '" + funcName + "' in " + filePath;
        return result;
    }

    // Determine stub body
    bool isVoid = isVoidReturn(result.originalContent, bodyStart);
    std::string stubBody = isVoid ? "{ }" : "{ return {}; }";

    // Replace the body
    std::string modified =
        result.originalContent.substr(0, bodyStart) + stubBody + result.originalContent.substr(bodyEnd + 1);

    if (!writeFile(filePath, modified)) {
        result.error = "failed to write modified file: " + filePath;
        return result;
    }

    result.success = true;
    return result;
}

bool CppStubGenerator::restoreFile(const std::string& filePath, const StubResult& result) {
    if (result.originalContent.empty()) return false;
    return writeFile(filePath, result.originalContent);
}

} // namespace topo::check
