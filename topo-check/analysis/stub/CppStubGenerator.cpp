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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace topo::check {

namespace {

/// True if the `'` at `pos` is a C++14 digit separator (e.g. `1'000`,
/// `0xFF'FF`) rather than the opening quote of a char literal. A digit
/// separator sits between two digits of a numeric literal, so both neighbours
/// are hexadecimal digits. Char literals — including the encoding-prefixed
/// forms `L'a'` / `u'a'` / `U'a'` / `u8'a'` — have a non-digit on at least one
/// side (the opener is preceded by an operator / prefix letter, the closer is
/// followed by punctuation), so they are not misclassified here.
bool isDigitSeparator(const std::string& source, size_t pos) {
    if (pos == 0 || pos + 1 >= source.size()) return false;
    char prev = source[pos - 1];
    char next = source[pos + 1];
    return std::isxdigit(static_cast<unsigned char>(prev)) &&
           std::isxdigit(static_cast<unsigned char>(next));
}

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
        if (c == '\'' && !isDigitSeparator(source, i)) {
            // A digit separator (`1'000`) is not a char-literal opener.
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
            else if (c == '"' || (c == '\'' && !isDigitSeparator(source, i))) {
                // Skip string/char literal (a `'` that is a digit separator,
                // e.g. inside `1'000`, is not a literal and falls through).
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

        // Handle char literals. A `'` that is a C++14 digit separator (e.g.
        // inside `1'000`) must NOT open a literal scan, or it would consume to
        // the next `'` and swallow the function's closing brace.
        if (c == '\'' && !isDigitSeparator(source, i)) {
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
    // Determine the return type of THIS definition only, anchored to its body
    // brace at `bodyStart`. The old heuristic scanned a fixed window backward
    // and returned true on any whole-word `void` followed by a `(`, so a
    // preceding `void other() {}`, a `std::function<void()>` return type, or a
    // `void*` parameter misclassified a non-void function as void and produced
    // a return-less stub that fails to compile. We instead walk back from the
    // body brace to the function name and inspect only the return-type text.

    if (bodyStart == 0 || bodyStart > source.size()) return false;

    // Step 1: from just before '{', skip whitespace and trailing qualifiers
    // (const, noexcept, override, final, trailing return type via '->', etc.)
    // back to the ')' that closes the parameter list.
    size_t i = bodyStart;
    while (i > 0) {
        char c = source[i - 1];
        if (c == ')') break;
        --i;
    }
    if (i == 0 || source[i - 1] != ')') return false;
    size_t closeParen = i - 1;

    // Step 2: backward paren-balance to the matching '(' opening the params.
    int depth = 1;
    size_t j = closeParen;
    while (j > 0 && depth > 0) {
        char c = source[j - 1];
        if (c == ')')
            ++depth;
        else if (c == '(')
            --depth;
        --j;
    }
    if (depth != 0) return false;
    size_t openParen = j; // index of '('

    // Step 3: skip whitespace before '(' to the end of the function name.
    size_t nameEnd = openParen;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(source[nameEnd - 1]))) --nameEnd;
    // Walk back over the function name identifier (and any operator/destructor
    // punctuation that may precede it, e.g. `operator+`, `~Foo`).
    size_t nameStart = nameEnd;
    while (nameStart > 0) {
        char c = source[nameStart - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            --nameStart;
            continue;
        }
        break;
    }
    if (nameStart == nameEnd) return false; // no identifier — give up safely

    // Step 4: collect the return-type text: from the start of this declaration
    // back to a statement boundary, then everything before the name.
    size_t declStart = nameStart;
    while (declStart > 0) {
        char c = source[declStart - 1];
        // Stop at a statement / scope boundary.
        if (c == ';' || c == '{' || c == '}') break;
        --declStart;
    }
    std::string returnArea = source.substr(declStart, nameStart - declStart);

    // Step 5: strip leading specifiers/keywords that are not part of the type
    // (static, virtual, inline, constexpr, explicit, friend, extern, etc.),
    // template clauses, and scope qualifiers, leaving the bare return type.
    // A function is void-returning iff that bare type is exactly `void`
    // (not `void*`, not `void&`, not a `<...void...>` template argument).
    static const std::vector<std::string> leadingKeywords = {
        "static", "virtual", "inline", "constexpr", "consteval", "explicit",
        "friend", "extern", "noexcept"};

    auto trim = [](std::string s) {
        auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };

    std::string type = trim(returnArea);

    // Remove any template/attribute clause by dropping everything up to the
    // last top-level '>' or ']' — a `void` buried inside `<...>` (e.g.
    // `std::function<void()>`) is an argument, not the return type.
    {
        int angle = 0, square = 0;
        std::string out;
        for (char c : type) {
            if (c == '<') ++angle;
            else if (c == '>') { if (angle > 0) --angle; out.clear(); continue; }
            else if (c == '[') ++square;
            else if (c == ']') { if (square > 0) --square; out.clear(); continue; }
            if (angle == 0 && square == 0) out += c;
        }
        type = trim(out);
    }

    // Strip leading non-type keywords repeatedly.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& kw : leadingKeywords) {
            if (type.size() >= kw.size() && type.compare(0, kw.size(), kw) == 0) {
                size_t after = kw.size();
                bool wholeWord = (after >= type.size()) ||
                                 !(std::isalnum(static_cast<unsigned char>(type[after])) || type[after] == '_');
                if (wholeWord) {
                    type = trim(type.substr(after));
                    changed = true;
                    break;
                }
            }
        }
    }

    // The bare return type is void only when it is exactly the token `void`
    // with no pointer/reference/qualifier suffix (so `void*` / `void&` are not
    // treated as void-returning).
    return type == "void";
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
